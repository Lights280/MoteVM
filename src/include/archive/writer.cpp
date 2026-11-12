#include "archive/writer.hpp"


#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "archive/name.hpp"
#include "archive/util.hpp"
#include "parse.hpp"

namespace ARCHIVE
{

    void Writer::clear(void)
    {
        entryList.clear();
        exactNames.clear();
        foldedNames.clear();
        metadata.clear();
    }

    bool Writer::nameTaken(std::string_view name) const
    {
        return exactNames.find(std::string(name)) != exactNames.end() ||
               foldedNames.find(foldAscii(name)) != foldedNames.end();
    }

    bool Writer::addModule(std::string_view name, std::span<const byte> data, std::string &error)
    {
        error.clear();
        if (!validName(name, &error))
            return false;
        if (!hasModuleSuffix(name))
        {
            error = "module name must end with .mbc";
            return false;
        }
        if (data.empty())
        {
            error = "module '" + std::string(name) + "' is empty";
            return false;
        }
        if (data.size() > static_cast<std::size_t>(MAX_ENTRY_BYTES))
        {
            error = "module '" + std::string(name) + "' exceeds 64 MiB";
            return false;
        }
        if (nameTaken(name))
        {
            error = "duplicate entry name '" + std::string(name) + "'";
            return false;
        }

        Parse parser;
        int result = parser.load(data);
        if (result == PARSE::OK)
            result = parser.parse();
        if (result != PARSE::OK)
        {
            error = "module '" + std::string(name) + "': " + PARSE::errorText(result);
            return false;
        }

        const std::string owned(name);
        const std::string folded = foldAscii(name);
        exactNames.insert(owned);
        foldedNames.insert(folded);

        InputEntry entry;
        entry.name = owned;
        entry.kind = EntryKind::module;
        entry.data.assign(data.begin(), data.end());
        entryList.push_back(std::move(entry));
        return true;
    }

    bool Writer::addResource(std::string_view name, std::span<const byte> data,
                             std::string &error)
    {
        error.clear();
        if (!validName(name, &error))
            return false;
        if (data.size() > static_cast<std::size_t>(MAX_ENTRY_BYTES))
        {
            error = "resource '" + std::string(name) + "' exceeds 64 MiB";
            return false;
        }
        if (nameTaken(name))
        {
            error = "duplicate entry name '" + std::string(name) + "'";
            return false;
        }

        const std::string owned(name);
        const std::string folded = foldAscii(name);
        exactNames.insert(owned);
        foldedNames.insert(folded);

        InputEntry entry;
        entry.name = owned;
        entry.kind = EntryKind::resource;
        entry.data.assign(data.begin(), data.end());
        entryList.push_back(std::move(entry));
        return true;
    }

    bool Writer::setMetadata(word key, std::string_view value, std::string &error)
    {
        error.clear();
        return metadata.set(key, value, error);
    }

    std::size_t Writer::moduleCount(void) const
    {
        std::size_t count = 0;
        for (const InputEntry &entry : entryList)
            if (entry.kind == EntryKind::module)
                ++count;
        return count;
    }

    std::size_t Writer::resourceCount(void) const
    {
        std::size_t count = 0;
        for (const InputEntry &entry : entryList)
            if (entry.kind == EntryKind::resource)
                ++count;
        return count;
    }

    bool Writer::build(std::vector<byte> &out, std::string &error) const
    {
        error.clear();
        out.clear();

        std::vector<InputEntry> entries = entryList;
        std::sort(entries.begin(), entries.end(),
                  [](const InputEntry &left, const InputEntry &right)
                  {
                      return left.name < right.name;
                  });

        if (entries.size() > static_cast<std::size_t>(MAX_ENTRY_COUNT))
        {
            error = "archive contains more than 65535 entries";
            return false;
        }

        std::unordered_set<std::string> seenExact;
        std::unordered_set<std::string> seenFolded;
        for (const InputEntry &entry : entries)
        {
            if (!seenExact.insert(entry.name).second)
            {
                error = "duplicate entry name '" + entry.name + "'";
                return false;
            }
            if (!seenFolded.insert(foldAscii(entry.name)).second)
            {
                error = "entry name '" + entry.name + "' collides case-insensitively";
                return false;
            }
        }

        for (const InputEntry &entry : entries)
        {
            const std::string folded = foldAscii(entry.name);
            for (std::size_t slash = folded.find('/');
                 slash != std::string::npos;
                 slash = folded.find('/', slash + 1))
            {
                if (seenFolded.find(folded.substr(0, slash)) != seenFolded.end())
                {
                    error = "entry '" + entry.name +
                            "' conflicts with a file entry used as a directory prefix";
                    return false;
                }
            }
        }

        Manifest manifestData = metadata;
        std::size_t modules = 0;
        std::string onlyModule;
        for (const InputEntry &entry : entries)
        {
            if (entry.kind != EntryKind::module)
                continue;
            ++modules;
            onlyModule = entry.name;
        }

        const ManifestItem *entryItem = manifestData.find(META_ENTRY_MODULE);
        if (entryItem != nullptr)
        {
            const std::string entryName = manifestData.text(META_ENTRY_MODULE);
            const auto position = std::lower_bound(
                entries.begin(), entries.end(), entryName,
                [](const InputEntry &entry, const std::string &name)
                {
                    return entry.name < name;
                });
            if (position == entries.end() || position->name != entryName ||
                position->kind != EntryKind::module)
            {
                error = "manifest entry module '" + entryName +
                        "' is not a module entry";
                return false;
            }
        }
        else if (modules == 1)
        {
            if (!manifestData.set(META_ENTRY_MODULE, onlyModule, error))
                return false;
        }
        else if (modules > 1)
        {
            error = "entry module is required when the package contains multiple modules";
            return false;
        }

        std::vector<qword> nameOffsets(entries.size(), 0);
        qword stringsSize = 0;
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            nameOffsets[i] = stringsSize;
            stringsSize += static_cast<qword>(entries[i].name.size());
            if (stringsSize > MAX_STRING_BYTES)
            {
                error = "string table exceeds 16 MiB";
                return false;
            }
        }

        std::vector<byte> manifestBytes;
        if (!manifestData.encode(manifestBytes, error))
            return false;

        const qword entryCount = static_cast<qword>(entries.size());
        const qword tocSize = entryCount * TOC_ENTRY_SIZE;
        const qword stringsOffset = HEADER_SIZE + tocSize;
        const qword manifestOffset = align8(stringsOffset + stringsSize);
        const qword manifestSize = static_cast<qword>(manifestBytes.size());
        const qword dataOffset = align8(manifestOffset + manifestSize);
        if (dataOffset > MAX_ARCHIVE_BYTES)
        {
            error = "archive header sections exceed 256 MiB";
            return false;
        }

        std::vector<qword> dataOffsets(entries.size(), 0);
        qword cursor = dataOffset;
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            cursor = align8(cursor);
            dataOffsets[i] = cursor;
            cursor += static_cast<qword>(entries[i].data.size());
            if (cursor > MAX_ARCHIVE_BYTES)
            {
                error = "archive payload exceeds 256 MiB";
                return false;
            }
        }
        const qword fileSize = align8(cursor);
        if (fileSize > MAX_ARCHIVE_BYTES)
        {
            error = "archive exceeds 256 MiB";
            return false;
        }

        out.assign(static_cast<std::size_t>(fileSize), 0);

        for (std::size_t i = 0; i < sizeof(MAGIC); ++i)
            out[i] = MAGIC[i];
        put16(out, 8, SUPPORTED_MAJOR);
        put16(out, 10, SUPPORTED_MINOR);
        put32(out, 12, FLAG_HAS_MANIFEST);
        put32(out, 16, static_cast<dword>(entryCount));
        put32(out, 20, static_cast<dword>(TOC_ENTRY_SIZE));
        put64(out, 24, HEADER_SIZE);
        put64(out, 32, tocSize);
        put64(out, 40, stringsOffset);
        put64(out, 48, stringsSize);
        put64(out, 56, manifestOffset);
        put64(out, 64, manifestSize);
        put64(out, 72, dataOffset);
        put64(out, 80, fileSize);
        put32(out, 88, 0);
        put32(out, 92, 0);

        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const InputEntry &entry = entries[i];
            const qword base = HEADER_SIZE + static_cast<qword>(i) * TOC_ENTRY_SIZE;
            put32(out, base, static_cast<dword>(nameOffsets[i]));
            put32(out, base + 4, static_cast<dword>(entry.name.size()));
            put8(out, base + 8, static_cast<byte>(entry.kind));
            put8(out, base + 9, 0);
            put16(out, base + 10, 0);
            put32(out, base + 12,
                  Parse::crc32c(std::span<const byte>(entry.data.data(), entry.data.size())));
            put64(out, base + 16, dataOffsets[i]);
            put64(out, base + 24, static_cast<qword>(entry.data.size()));
            put64(out, base + 32, 0);
        }

        qword nameCursor = stringsOffset;
        for (const InputEntry &entry : entries)
        {
            for (char value : entry.name)
                out[indexOf(nameCursor++)] =
                    static_cast<byte>(static_cast<unsigned char>(value));
        }

        std::copy(manifestBytes.begin(), manifestBytes.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(manifestOffset));

        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i].data.empty())
                continue;
            std::copy(entries[i].data.begin(), entries[i].data.end(),
                      out.begin() + static_cast<std::ptrdiff_t>(dataOffsets[i]));
        }

        const dword checksum =
            Parse::crc32c(std::span<const byte>(out), HEADER_CHECKSUM_OFFSET,
                          HEADER_CHECKSUM_OFFSET + HEADER_CHECKSUM_SIZE);
        put32(out, HEADER_CHECKSUM_OFFSET, checksum);
        return true;
    }

} // namespace ARCHIVE
