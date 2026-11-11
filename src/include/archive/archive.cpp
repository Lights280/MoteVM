#include "archive/archive.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "archive/name.hpp"
#include "archive/util.hpp"
#include "parse.hpp"

namespace
{
    struct Range
    {
        qword begin = 0;
        qword end = 0;
    };
}

namespace ARCHIVE
{

    void Archive::resetImage(void)
    {
        headerData = Header{};
        entryList.clear();
        manifestData.clear();
        parsed = false;
    }

    int Archive::fail(int code, std::string detail)
    {
        lastError = code;
        lastDetail = std::move(detail);
        parsed = false;
        entryList.clear();
        manifestData.clear();
        return code;
    }

    int Archive::load(const std::string &path)
    {
        resetImage();
        buffer.clear();
        lastError = OK;
        lastDetail.clear();

        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error)
            return lastError = FAIL_OPEN;
        if (size > static_cast<std::uintmax_t>(MAX_ARCHIVE_BYTES))
            return lastError = TOO_LARGE;

        std::ifstream file(path, std::ios::binary);
        if (!file)
            return lastError = FAIL_OPEN;

        buffer.resize(static_cast<std::size_t>(size));
        if (size != 0)
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(size));
        if (!file || static_cast<std::uintmax_t>(file.gcount()) != size)
        {
            buffer.clear();
            return lastError = TRUNC;
        }
        return OK;
    }

    int Archive::load(std::span<const byte> bytes)
    {
        resetImage();
        lastError = OK;
        lastDetail.clear();
        if (bytes.size() > static_cast<std::size_t>(MAX_ARCHIVE_BYTES))
            return lastError = TOO_LARGE;
        buffer.assign(bytes.begin(), bytes.end());
        return OK;
    }

    int Archive::parse(bool verifyModules)
    {
        resetImage();
        lastError = OK;
        lastDetail.clear();

        if (buffer.empty())
            return fail(EMPTY, "empty input");

        const std::span<const byte> file(buffer);
        const qword total = static_cast<qword>(buffer.size());

        if (total < HEADER_SIZE)
            return fail(TRUNC, "input is shorter than the 96-byte header");
        if (total > MAX_ARCHIVE_BYTES)
            return fail(TOO_LARGE, "archive exceeds the 256 MiB limit");
        if ((total & 7u) != 0)
            return fail(BAD_HEADER, "file size is not a multiple of 8");

        if (std::memcmp(file.data(), MAGIC, sizeof(MAGIC)) != 0)
            return fail(BAD_HEADER, "bad magic (expected MOTEARC\\0)");

        headerData.versionMajor = at16(file, 8);
        headerData.versionMinor = at16(file, 10);
        headerData.flags = at32(file, 12);
        headerData.entryCount = at32(file, 16);
        headerData.tocEntrySize = at32(file, 20);
        headerData.tocOffset = at64(file, 24);
        headerData.tocSize = at64(file, 32);
        headerData.stringsOffset = at64(file, 40);
        headerData.stringsSize = at64(file, 48);
        headerData.manifestOffset = at64(file, 56);
        headerData.manifestSize = at64(file, 64);
        headerData.dataOffset = at64(file, 72);
        headerData.fileSize = at64(file, 80);
        headerData.checksum = at32(file, 88);
        headerData.reserved0 = at32(file, 92);

        if (headerData.versionMajor != SUPPORTED_MAJOR ||
            headerData.versionMinor > SUPPORTED_MINOR)
            return fail(BAD_HEADER, "unsupported MTR version");
        if (headerData.flags != FLAG_HAS_MANIFEST)
            return fail(BAD_HEADER, "unsupported header flags");
        if (headerData.reserved0 != 0)
            return fail(BAD_HEADER, "header reserved field is not zero");
        if (headerData.entryCount > MAX_ENTRY_COUNT)
            return fail(BAD_HEADER, "entry count exceeds 65535");
        if (headerData.tocEntrySize != TOC_ENTRY_SIZE)
            return fail(BAD_HEADER, "toc_entry_size is not 40");
        if (headerData.fileSize != total)
            return fail(BAD_HEADER, "header file_size does not match the input length");
        if ((headerData.fileSize & 7u) != 0)
            return fail(BAD_HEADER, "file_size is not a multiple of 8");

        const qword expectedTocSize =
            static_cast<qword>(headerData.entryCount) * TOC_ENTRY_SIZE;
        if (headerData.tocSize != expectedTocSize)
            return fail(BAD_HEADER, "toc_size does not match entry_count * 40");
        if (headerData.tocOffset != HEADER_SIZE)
            return fail(BAD_HEADER, "toc_offset is not 96");
        if (headerData.stringsOffset != HEADER_SIZE + expectedTocSize)
            return fail(BAD_HEADER, "strings_offset is not immediately after the TOC");
        if (headerData.stringsSize > MAX_STRING_BYTES)
            return fail(BAD_HEADER, "string table exceeds 16 MiB");
        if (headerData.manifestSize > MAX_MANIFEST_BYTES ||
            (headerData.manifestSize & 7u) != 0)
            return fail(BAD_HEADER, "manifest size is invalid");
        if (headerData.manifestOffset !=
            align8(headerData.stringsOffset + headerData.stringsSize))
            return fail(BAD_HEADER, "manifest_offset is not canonical");
        if (headerData.dataOffset !=
            align8(headerData.manifestOffset + headerData.manifestSize))
            return fail(BAD_HEADER, "data_offset is not canonical");
        if ((headerData.dataOffset & 7u) != 0 || headerData.dataOffset > total)
            return fail(BAD_HEADER, "data_offset is invalid");

        if (!inBounds(headerData.tocOffset, headerData.tocSize, total) ||
            !inBounds(headerData.stringsOffset, headerData.stringsSize, total) ||
            !inBounds(headerData.manifestOffset, headerData.manifestSize, total))
            return fail(BAD_HEADER, "header section range is outside the file");

        const dword computed = Parse::crc32c(file, HEADER_CHECKSUM_OFFSET,
                                             HEADER_CHECKSUM_OFFSET + HEADER_CHECKSUM_SIZE);
        if (computed != headerData.checksum)
            return fail(BAD_CHECKSUM, "whole-file CRC-32C mismatch");

        qword expectedNameOffset = 0;
        qword expectedDataOffset = headerData.dataOffset;
        std::string previousName;
        std::unordered_set<std::string> foldedNames;

        for (dword index = 0; index < headerData.entryCount; ++index)
        {
            const qword base = headerData.tocOffset +
                               static_cast<qword>(index) * TOC_ENTRY_SIZE;

            const dword nameOffset = at32(file, base);
            const dword nameLength = at32(file, base + 4);
            if (static_cast<qword>(nameOffset) != expectedNameOffset)
                return fail(BAD_TOC, "entry #" + std::to_string(index) +
                                         ": name_offset is not contiguous");
            if (nameLength == 0 ||
                static_cast<std::size_t>(nameLength) > MAX_NAME_BYTES)
                return fail(BAD_TOC, "entry #" + std::to_string(index) +
                                         ": invalid name length");
            if (!inBounds(nameOffset, nameLength, headerData.stringsSize))
                return fail(BAD_TOC, "entry #" + std::to_string(index) +
                                         ": name is outside the string table");

            const std::span<const byte> nameBytes =
                file.subspan(static_cast<std::size_t>(headerData.stringsOffset + nameOffset),
                             static_cast<std::size_t>(nameLength));
            std::string name(reinterpret_cast<const char *>(nameBytes.data()),
                             nameBytes.size());

            std::string nameError;
            if (!validName(name, &nameError))
                return fail(BAD_NAME, "entry #" + std::to_string(index) + ": " + nameError);

            const byte kindByte = at8(file, base + 8);
            const byte entryFlags = at8(file, base + 9);
            const word entryReserved = at16(file, base + 10);
            if (kindByte != static_cast<byte>(EntryKind::module) &&
                kindByte != static_cast<byte>(EntryKind::resource))
                return fail(BAD_TOC, "entry #" + std::to_string(index) + ": unknown kind");
            if (entryFlags != 0 || entryReserved != 0)
                return fail(BAD_TOC, "entry #" + std::to_string(index) +
                                         ": flags/reserved must be zero");

            const EntryKind kind = static_cast<EntryKind>(kindByte);
            if (kind == EntryKind::module && !hasModuleSuffix(name))
                return fail(BAD_NAME, "entry #" + std::to_string(index) +
                                          ": module name must end with .mbc");

            const dword entryCrc = at32(file, base + 12);
            const qword dataOffset = at64(file, base + 16);
            const qword dataSize = at64(file, base + 24);
            const qword entryReserved1 = at64(file, base + 32);
            if (entryReserved1 != 0)
                return fail(BAD_TOC, "entry #" + std::to_string(index) +
                                         ": reserved field is not zero");
            if (dataOffset != expectedDataOffset)
                return fail(BAD_TOC, "entry #" + std::to_string(index) +
                                         ": data is not contiguous");
            if (dataSize > MAX_ENTRY_BYTES)
                return fail(BAD_DATA, "entry #" + std::to_string(index) +
                                          ": data exceeds 64 MiB");
            if (kind == EntryKind::module && dataSize == 0)
                return fail(BAD_DATA, "entry #" + std::to_string(index) +
                                          ": module entry is empty");
            if (!inBounds(dataOffset, dataSize, total))
                return fail(BAD_DATA, "entry #" + std::to_string(index) +
                                          ": data is outside the file");

            const std::span<const byte> data =
                file.subspan(static_cast<std::size_t>(dataOffset),
                             static_cast<std::size_t>(dataSize));
            if (Parse::crc32c(data) != entryCrc)
                return fail(BAD_DATA, "entry #" + std::to_string(index) +
                                          ": entry CRC-32C mismatch");

            if (index != 0 && !(previousName < name))
                return fail(BAD_TOC, "entry #" + std::to_string(index) +
                                         ": names are not strictly ascending");

            const std::string folded = foldAscii(name);
            if (!foldedNames.insert(folded).second)
                return fail(DUPLICATE, "entry #" + std::to_string(index) +
                                           ": name collides case-insensitively");

            Entry entry;
            entry.name = name;
            entry.kind = kind;
            entry.crc32c = entryCrc;
            entry.offset = dataOffset;
            entry.size = dataSize;
            entry.data = data;

            previousName = std::move(name);
            expectedNameOffset += static_cast<qword>(nameLength);
            expectedDataOffset = align8(dataOffset + dataSize);
            entryList.push_back(std::move(entry));
        }

        // A file entry cannot also be a directory prefix of another entry: the
        // unpacked tree would have to be a file and a directory at the same path.
        // Check the fold set so Windows-style case collisions are covered too.
        for (const Entry &entry : entryList)
        {
            const std::string folded = foldAscii(entry.name);
            for (std::size_t slash = folded.find('/');
                 slash != std::string::npos;
                 slash = folded.find('/', slash + 1))
            {
                if (foldedNames.find(folded.substr(0, slash)) != foldedNames.end())
                    return fail(BAD_NAME, "entry '" + entry.name +
                                              "' conflicts with a file entry used as a directory prefix");
            }
        }

        if (expectedNameOffset != headerData.stringsSize)
            return fail(BAD_TOC, "string table has unreferenced bytes");
        if (expectedDataOffset != headerData.fileSize)
            return fail(BAD_TOC, "payload does not end at file_size");

        std::vector<Range> ranges;
        ranges.push_back({0, HEADER_SIZE});
        if (headerData.tocSize != 0)
            ranges.push_back({headerData.tocOffset,
                              headerData.tocOffset + headerData.tocSize});
        if (headerData.stringsSize != 0)
            ranges.push_back({headerData.stringsOffset,
                              headerData.stringsOffset + headerData.stringsSize});
        if (headerData.manifestSize != 0)
            ranges.push_back({headerData.manifestOffset,
                              headerData.manifestOffset + headerData.manifestSize});
        for (const Entry &entry : entryList)
            if (entry.size != 0)
                ranges.push_back({entry.offset, entry.offset + entry.size});

        std::sort(ranges.begin(), ranges.end(), [](const Range &left, const Range &right)
                  { return left.begin < right.begin; });

        qword cursor = 0;
        for (const Range &range : ranges)
        {
            if (range.begin < cursor)
                return fail(BAD_DATA, "archive regions overlap");
            if (range.end > total)
                return fail(BAD_DATA, "archive region is outside the file");
            if (!allZero(file, cursor, range.begin))
                return fail(BAD_DATA, "non-zero bytes between archive regions");
            cursor = range.end;
        }
        if (cursor < total && !allZero(file, cursor, total))
            return fail(BAD_DATA, "non-zero bytes after the last archive region");

        const std::span<const byte> manifestBytes =
            file.subspan(static_cast<std::size_t>(headerData.manifestOffset),
                         static_cast<std::size_t>(headerData.manifestSize));
        std::string manifestError;
        if (!Manifest::decode(manifestBytes, manifestData, manifestError))
            return fail(BAD_MANIFEST, manifestError);

        const ManifestItem *entryItem = manifestData.find(META_ENTRY_MODULE);
        if (entryItem != nullptr)
        {
            const std::string entryName = manifestData.text(META_ENTRY_MODULE);
            const Entry *entry = find(entryName);
            if (entry == nullptr || entry->kind != EntryKind::module)
                return fail(BAD_MANIFEST, "manifest entry module '" + entryName +
                                              "' is not a module entry");
        }

        if (verifyModules)
        {
            for (const Entry &entry : entryList)
            {
                if (entry.kind != EntryKind::module)
                    continue;
                Parse parser;
                int result = parser.load(entry.data);
                if (result == PARSE::OK)
                    result = parser.parse();
                if (result != PARSE::OK)
                    return fail(BAD_MODULE, "entry '" + entry.name +
                                                "': " + PARSE::errorText(result));
            }
        }

        parsed = true;
        lastError = OK;
        lastDetail.clear();
        return OK;
    }

    const ARCHIVE::Entry *Archive::find(std::string_view name) const
    {
        for (const Entry &entry : entryList)
            if (entry.name == name)
                return &entry;
        return nullptr;
    }

    std::string Archive::entryModuleName(void) const
    {
        const ManifestItem *item = manifestData.find(META_ENTRY_MODULE);
        if (item != nullptr)
            return manifestData.text(META_ENTRY_MODULE);

        const Entry *only = nullptr;
        std::size_t count = 0;
        for (const Entry &entry : entryList)
        {
            if (entry.kind != EntryKind::module)
                continue;
            only = &entry;
            ++count;
        }
        return count == 1 ? only->name : std::string();
    }

    bool Archive::resolveEntry(std::string &name, std::string &error) const
    {
        const ManifestItem *item = manifestData.find(META_ENTRY_MODULE);
        if (item != nullptr)
        {
            name = manifestData.text(META_ENTRY_MODULE);
        }
        else
        {
            std::size_t count = 0;
            for (const Entry &entry : entryList)
            {
                if (entry.kind != EntryKind::module)
                    continue;
                name = entry.name;
                ++count;
            }
            if (count == 0)
            {
                error = "archive contains no module";
                return false;
            }
            if (count != 1)
            {
                error = "archive has multiple modules and no manifest entry module";
                return false;
            }
        }

        const Entry *entry = find(name);
        if (entry == nullptr || entry->kind != EntryKind::module)
        {
            error = "entry module '" + name + "' is not a module entry";
            return false;
        }
        return true;
    }

    const char *errorText(int code)
    {
        switch (code)
        {
        case OK:
            return "ok";
        case FAIL_OPEN:
            return "cannot open or read archive";
        case EMPTY:
            return "empty archive";
        case TRUNC:
            return "truncated archive";
        case TOO_LARGE:
            return "archive exceeds the size limit";
        case BAD_HEADER:
            return "invalid archive header";
        case BAD_CHECKSUM:
            return "archive CRC-32C mismatch";
        case BAD_TOC:
            return "invalid archive table of contents";
        case BAD_NAME:
            return "invalid archive entry name";
        case DUPLICATE:
            return "duplicate archive entry name";
        case BAD_MANIFEST:
            return "invalid archive manifest";
        case BAD_MODULE:
            return "invalid MoteBC module in archive";
        case BAD_DATA:
            return "invalid archive entry data";
        case BAD_ENTRY:
            return "cannot select an archive entry module";
        default:
            return "unknown archive error";
        }
    }

} // namespace ARCHIVE
