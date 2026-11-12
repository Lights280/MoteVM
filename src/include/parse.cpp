#include "parse.hpp"
#include "verifier.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// Hardware CRC-32C. The SSE4.2 path is selected at run time; the ARM path is
// compiled only when the target already implies the CRC extension. Both fall
// back to the portable slicing-by-8 implementation below. Define
// MOTEVM_CRC32C_DISABLE_HW to force the portable path (tests/CI).
#if !defined(MOTEVM_CRC32C_DISABLE_HW)
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define MOTEVM_CRC32C_ARM 1
#elif (defined(_M_X64) || defined(_M_IX86)) && defined(_MSC_VER)
#include <intrin.h>
#include <nmmintrin.h>
#define MOTEVM_CRC32C_X86 1
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define MOTEVM_CRC32C_X86 1
#endif
#endif

namespace
{
    // CRC-32C, reflected polynomial 0x82F63B78. Table 0 is the classic byte
    // table; tables 1..7 let the portable path consume eight bytes per step.
    struct Crc32cTables
    {
        dword value[8][256]{};
        constexpr Crc32cTables()
        {
            for (dword i = 0; i < 256; ++i)
            {
                dword crc = i;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc & 1u) ? ((crc >> 1) ^ 0x82F63B78u) : (crc >> 1);
                value[0][i] = crc;
            }
            for (std::size_t table = 1; table < 8; ++table)
                for (std::size_t i = 0; i < 256; ++i)
                {
                    const dword previous = value[table - 1][i];
                    value[table][i] = (previous >> 8) ^ value[0][previous & 0xFFu];
                }
        }
    };
    constexpr Crc32cTables CRC32C_TABLES{};

    dword crc32cUpdateBytewise(dword crc, const byte *data, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i)
            crc = CRC32C_TABLES.value[0][(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        return crc;
    }

    dword crc32cUpdatePortable(dword crc, const byte *data, std::size_t size)
    {
        if constexpr (std::endian::native == std::endian::little)
        {
            while (size >= 8)
            {
                dword low;
                std::memcpy(&low, data, sizeof(low));
                crc ^= low;
                crc = CRC32C_TABLES.value[7][crc & 0xFFu] ^
                      CRC32C_TABLES.value[6][(crc >> 8) & 0xFFu] ^
                      CRC32C_TABLES.value[5][(crc >> 16) & 0xFFu] ^
                      CRC32C_TABLES.value[4][(crc >> 24) & 0xFFu] ^
                      CRC32C_TABLES.value[3][data[4]] ^ CRC32C_TABLES.value[2][data[5]] ^
                      CRC32C_TABLES.value[1][data[6]] ^ CRC32C_TABLES.value[0][data[7]];
                data += 8;
                size -= 8;
            }
        }
        return crc32cUpdateBytewise(crc, data, size);
    }

#if defined(MOTEVM_CRC32C_X86)
#if defined(__GNUC__) || defined(__clang__)
#define MOTEVM_CRC32C_TARGET __attribute__((target("sse4.2")))
#else
#define MOTEVM_CRC32C_TARGET
#endif

    // CRC32 instruction semantics match the reflected table algorithm exactly:
    // same 0xFFFFFFFF pre/post inversion at the call boundary.
    MOTEVM_CRC32C_TARGET dword crc32cUpdateSse42(dword crc, const byte *data,
                                                 std::size_t size)
    {
#if defined(__x86_64__) || defined(_M_X64)
        while (size >= 8)
        {
            qword value;
            std::memcpy(&value, data, sizeof(value));
            crc = static_cast<dword>(_mm_crc32_u64(crc, value));
            data += 8;
            size -= 8;
        }
#endif
        while (size >= 4)
        {
            dword value;
            std::memcpy(&value, data, sizeof(value));
            crc = _mm_crc32_u32(crc, value);
            data += 4;
            size -= 4;
        }
        while (size != 0)
        {
            crc = _mm_crc32_u8(crc, *data);
            ++data;
            --size;
        }
        return crc;
    }

    bool crc32cHasSse42(void)
    {
#if defined(_MSC_VER)
        int info[4] = {0, 0, 0, 0};
        __cpuid(info, 1);
        return (info[2] & (1 << 20)) != 0;
#else
        return __builtin_cpu_supports("sse4.2") != 0;
#endif
    }
#undef MOTEVM_CRC32C_TARGET
#endif

#if defined(MOTEVM_CRC32C_ARM)
    dword crc32cUpdateArm(dword crc, const byte *data, std::size_t size)
    {
        while (size >= 8)
        {
            qword value;
            std::memcpy(&value, data, sizeof(value));
            crc = __crc32cd(crc, value);
            data += 8;
            size -= 8;
        }
        while (size >= 4)
        {
            dword value;
            std::memcpy(&value, data, sizeof(value));
            crc = __crc32cw(crc, value);
            data += 4;
            size -= 4;
        }
        while (size != 0)
        {
            crc = __crc32cb(crc, *data);
            ++data;
            --size;
        }
        return crc;
    }
#endif

    dword crc32cUpdate(dword crc, const byte *data, std::size_t size)
    {
#if defined(MOTEVM_CRC32C_ARM)
        return crc32cUpdateArm(crc, data, size);
#elif defined(MOTEVM_CRC32C_X86)
        if constexpr (std::endian::native == std::endian::little)
        {
            static const bool hardware = crc32cHasSse42();
            if (hardware)
                return crc32cUpdateSse42(crc, data, size);
        }
        return crc32cUpdatePortable(crc, data, size);
#else
        return crc32cUpdatePortable(crc, data, size);
#endif
    }

    // The header checksum field is read as zero: CRC is streamable, so feed the
    // three byte ranges (prefix, zeroed field, suffix) in order.
    dword crc32cOf(std::span<const byte> data, qword zeroBegin, qword zeroEnd)
    {
        constexpr byte ZEROS[64]{};
        const std::size_t size = data.size();
        std::size_t begin = static_cast<std::size_t>(
            std::min<qword>(zeroBegin, static_cast<qword>(size)));
        std::size_t end = static_cast<std::size_t>(
            std::min<qword>(zeroEnd, static_cast<qword>(size)));
        if (end < begin)
            end = begin;

        const byte *bytes = data.data();
        dword crc = 0xFFFFFFFFu;
        crc = crc32cUpdate(crc, bytes, begin);
        std::size_t zeros = end - begin;
        while (zeros != 0)
        {
            const std::size_t chunk = std::min<std::size_t>(zeros, sizeof(ZEROS));
            crc = crc32cUpdate(crc, ZEROS, chunk);
            zeros -= chunk;
        }
        if (size > end)
            crc = crc32cUpdate(crc, bytes + end, size - end);
        return ~crc;
    }

    // readers
    inline bool inBounds(qword offset, qword length, qword total)
    {
        return offset <= total && length <= total - offset;
    }

    inline byte at8(std::span<const byte> data, qword offset)
    {
        return data[static_cast<std::size_t>(offset)];
    }

    inline word at16(std::span<const byte> data, qword offset)
    {
        return static_cast<word>(at8(data, offset) | (static_cast<word>(at8(data, offset + 1)) << 8));
    }

    inline dword at32(std::span<const byte> data, qword offset)
    {
        return static_cast<dword>(at8(data, offset)) |
               (static_cast<dword>(at8(data, offset + 1)) << 8) |
               (static_cast<dword>(at8(data, offset + 2)) << 16) |
               (static_cast<dword>(at8(data, offset + 3)) << 24);
    }

    inline qword at64(std::span<const byte> data, qword offset)
    {
        return static_cast<qword>(at32(data, offset)) |
               (static_cast<qword>(at32(data, offset + 4)) << 32);
    }

    bool allZero(std::span<const byte> data, qword begin, qword end)
    {
        for (qword i = begin; i < end; ++i)
            if (at8(data, i) != 0)
                return false;
        return true;
    }

    std::string_view bytesView(std::span<const byte> data)
    {
        if (data.empty())
            return std::string_view{};
        return std::string_view(reinterpret_cast<const char *>(data.data()), data.size());
    }

    // no overlap
    bool rangesOverlap(std::vector<std::pair<qword, qword>> ranges)
    {
        std::sort(ranges.begin(), ranges.end());
        for (std::size_t i = 1; i < ranges.size(); ++i)
            if (ranges[i].first < ranges[i - 1].second)
                return true;
        return false;
    }
}

// lifecycle
void Parse::clearImage(void)
{
    imageData = PARSE::Image{};
    copyExtents_.clear();
    parsed = false;
}

std::span<const byte> Parse::sectionBytes(dword id) const
{
    const std::span<const byte> file(buffer);
    for (const PARSE::SecBlock &section : imageData.sections)
        if (section.id == id)
            return file.subspan(static_cast<std::size_t>(section.offset), static_cast<std::size_t>(section.size));
    return std::span<const byte>{};
}

bool Parse::isStringConstant(dword index) const
{
    return index < imageData.constants.size() && imageData.constants[index].kind == PARSE::ConstKind::string;
}

dword Parse::entryPc(void) const
{
    if (imageData.header.entry >= imageData.functions.size())
        return 0;
    return imageData.functions[imageData.header.entry].entry;
}

std::span<const byte> Parse::text(dword functionIndex) const
{
    if (functionIndex >= imageData.functions.size())
        return {};
    const PARSE::Function &fn = imageData.functions[functionIndex];
    return imageData.code.subspan(static_cast<std::size_t>(fn.entry) * ISA::INSTRUCTION_SIZE,
                                  static_cast<std::size_t>(fn.instr) * ISA::INSTRUCTION_SIZE);
}

std::span<const byte> Parse::entryText(void) const
{
    return text(imageData.header.entry);
}

bool Parse::hasDebug(void) const
{
    return !sectionBytes(PARSE::SECTION_DEBUG).empty();
}

dword Parse::crc32c(std::span<const byte> data)
{
    return crc32cOf(data, 0, 0);
}

dword Parse::crc32c(std::span<const byte> data, qword zeroBegin, qword zeroEnd)
{
    return crc32cOf(data, zeroBegin, zeroEnd);
}

// load
int Parse::load(const std::string &path)
{
    clearImage();
    buffer.clear();

    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return PARSE::FAIL_OPEN;
    if (size > PARSE::MAX_MODULE_BYTES)
        return PARSE::TOO_LARGE;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return PARSE::FAIL_OPEN;

    buffer.resize(static_cast<std::size_t>(size));
    if (size != 0)
        file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(size));
    if (!file || static_cast<qword>(file.gcount()) != size)
    {
        buffer.clear();
        return PARSE::TRUNC;
    }
    return PARSE::OK;
}

int Parse::load(std::span<const byte> bytes)
{
    clearImage();
    buffer.assign(bytes.begin(), bytes.end());
    return PARSE::OK;
}

// parse
int Parse::parse(void)
{
    clearImage();
    if (buffer.empty())
        return PARSE::EMPTY;
    if (buffer.size() < PARSE::HEADER_SIZE)
        return PARSE::TRUNC;

    int result = check();
    if (result == PARSE::OK)
        result = readSections();
    std::vector<word> copyExtents;
    if (result == PARSE::OK)
    {
        PARSE::Verifier verifier;
        result = verifier.verify(imageData, copyExtents);
    }
    if (result != PARSE::OK)
    {
        clearImage();
        return result;
    }
    copyExtents_ = std::move(copyExtents);
    parsed = true;
    return PARSE::OK;
}

int Parse::readSections(void)
{
    int result = readMeta();
    if (result == PARSE::OK)
        result = readConst();
    if (result == PARSE::OK)
        result = readCode();
    if (result == PARSE::OK)
        result = readFunc();
    if (result == PARSE::OK)
        result = readGlobal();
    if (result == PARSE::OK)
        result = readImport();
    if (result == PARSE::OK)
        result = readDebug();
    if (result == PARSE::OK)
        result = readCrossTable();
    return result;
}

// container
// order: size, header, CRC, table
int Parse::check(void)
{
    const std::span<const byte> file(buffer);
    const qword total = static_cast<qword>(buffer.size());
    if (total < PARSE::HEADER_SIZE)
        return PARSE::TRUNC;
    if (total > PARSE::MAX_MODULE_BYTES)
        return PARSE::TOO_LARGE;

    if (std::memcmp(buffer.data(), PARSE::MAGIC, 4) != 0)
        return PARSE::BAD_FILE;

    const word major = at16(file, 4);
    const word minor = at16(file, 6);
    if (major != PARSE::SUPPORTED_MAJOR || minor > PARSE::SUPPORTED_MINOR)
        return PARSE::BAD_FILE;

    const dword flags = at32(file, 8);
    if ((flags & ~PARSE::FLAG_DEBUG_PRESENT) != 0)
        return PARSE::BAD_FILE;
    if (at16(file, 18) != 0 || at32(file, 28) != 0)
        return PARSE::BAD_FILE;
    if (at32(file, 20) != total)
        return PARSE::BAD_FILE;

    const word seccnt = at16(file, 16);
    if (seccnt < 6)
        return PARSE::BAD_FILE;
    const qword tableEnd = PARSE::HEADER_SIZE + static_cast<qword>(seccnt) * PARSE::SECBLOCK_SIZE;
    if (tableEnd > total)
        return PARSE::BAD_FILE;

    const dword checksum = at32(file, 24);
    if (crc32cOf(file, 24, 28) != checksum)
        return PARSE::BAD_FILE;

    std::memcpy(imageData.header.magic, buffer.data(), 4);
    imageData.header.majorVer = major;
    imageData.header.minorVer = minor;
    imageData.header.flags = flags;
    imageData.header.entry = at32(file, 12);
    imageData.header.seccnt = seccnt;
    imageData.header.reserved0 = at16(file, 18);
    imageData.header.fileSize = at32(file, 20);
    imageData.header.checksum = checksum;
    imageData.header.reserved1 = at32(file, 28);

    bool present[8] = {};
    imageData.sections.resize(seccnt);
    for (word i = 0; i < seccnt; ++i)
    {
        const qword at = PARSE::HEADER_SIZE + static_cast<qword>(i) * PARSE::SECBLOCK_SIZE;
        PARSE::SecBlock &section = imageData.sections[i];
        section.id = at32(file, at);
        section.flags = at32(file, at + 4);
        section.offset = at64(file, at + 8);
        section.size = at64(file, at + 16);

        if (section.id == 0)
            return PARSE::BAD_FILE;
        if (i > 0 && section.id <= imageData.sections[i - 1].id)
            return PARSE::BAD_FILE;

        if (section.id <= PARSE::SECTION_DEBUG)
        {
            if (section.flags != 0)
                return PARSE::BAD_FILE;
            present[static_cast<std::size_t>(section.id)] = true;
        }
        else if (section.flags != 0) // unknown section flags
            return PARSE::BAD_FILE;

        if ((section.offset & 7ull) != 0 || (section.size & 7ull) != 0)
            return PARSE::BAD_FILE;
        if (section.offset < tableEnd)
            return PARSE::BAD_FILE;
        if (!inBounds(section.offset, section.size, total))
            return PARSE::BAD_FILE;
    }

    for (dword id = PARSE::SECTION_META; id <= PARSE::SECTION_IMPORTS; ++id)
        if (!present[id])
            return PARSE::BAD_FILE;

    const bool debugPresent = present[PARSE::SECTION_DEBUG];
    if (debugPresent != ((flags & PARSE::FLAG_DEBUG_PRESENT) != 0))
        return PARSE::BAD_FILE;

    std::vector<std::pair<qword, qword>> ranges;
    ranges.reserve(imageData.sections.size());
    for (const PARSE::SecBlock &section : imageData.sections)
        ranges.emplace_back(section.offset, section.offset + section.size);
    if (rangesOverlap(std::move(ranges)))
        return PARSE::BAD_FILE;

    return PARSE::OK;
}

// segments
int Parse::readMeta(void)
{
    const std::span<const byte> bytes = sectionBytes(PARSE::SECTION_META);
    if ((bytes.size() & 7u) != 0)
        return PARSE::BAD_CONTENT;

    bool seen[5] = {};
    qword at = 0;
    while (at < bytes.size())
    {
        if (!inBounds(at, 4, bytes.size()))
            return PARSE::BAD_CONTENT;

        const word key = at16(bytes, at);
        const word length = at16(bytes, at + 2);
        const qword recordSize = align8(4 + static_cast<qword>(length));
        if (!inBounds(at, recordSize, bytes.size()))
            return PARSE::BAD_CONTENT;
        if (!allZero(bytes, at + 4 + length, at + recordSize))
            return PARSE::BAD_CONTENT;

        if (key >= PARSE::META_MODULE_NAME && key <= PARSE::META_COMMENT)
        {
            if (seen[static_cast<std::size_t>(key)])
                return PARSE::BAD_CONTENT;
            seen[static_cast<std::size_t>(key)] = true;
        }

        const std::span<const byte> payload = bytes.subspan(static_cast<std::size_t>(at + 4), length);
        imageData.meta.push_back({key, length, payload});
        switch (key)
        {
        case PARSE::META_MODULE_NAME:
            imageData.moduleName = payload;
            break;
        case PARSE::META_SOURCE_FILE:
            imageData.sourceFile = payload;
            break;
        case PARSE::META_BUILD_ID:
            imageData.buildId = payload;
            break;
        case PARSE::META_COMMENT:
            imageData.comment = payload;
            break;
        default:
            break;
        }
        at += recordSize;
    }
    return PARSE::OK;
}

int Parse::readConst(void)
{
    const std::span<const byte> bytes = sectionBytes(PARSE::SECTION_CONST);
    if (bytes.size() < 0x8)
        return PARSE::BAD_CONTENT;

    const dword count = at32(bytes, 0);
    if (count > PARSE::MAX_STATIC_COUNT || at32(bytes, 4) != 0)
        return PARSE::BAD_CONTENT;

    imageData.constants.reserve(count);
    qword at = 0x8;
    for (dword i = 0; i < count; ++i)
    {
        if (!inBounds(at, 0x8, bytes.size()))
            return PARSE::BAD_CONTENT;

        const byte kind = at8(bytes, at);
        const dword length = at32(bytes, at + 4);
        if (kind > static_cast<byte>(PARSE::ConstKind::string))
            return PARSE::BAD_CONTENT;
        if (at8(bytes, at + 1) != 0 || at16(bytes, at + 2) != 0)
            return PARSE::BAD_CONTENT;

        const bool isString = kind == static_cast<byte>(PARSE::ConstKind::string);
        if (!isString && length != 8)
            return PARSE::BAD_CONTENT;
        if (isString && length > PARSE::MAX_STRING_BYTES)
            return PARSE::BAD_CONTENT;

        const qword entrySize = 0x8 + align8(length);
        if (!inBounds(at, entrySize, bytes.size()))
            return PARSE::BAD_CONTENT;
        if (!allZero(bytes, at + 0x8 + length, at + entrySize))
            return PARSE::BAD_CONTENT;

        const std::span<const byte> payload = bytes.subspan(static_cast<std::size_t>(at + 0x8), length);
        if (kind == static_cast<byte>(PARSE::ConstKind::null) && !allZero(payload, 0, payload.size()))
            return PARSE::BAD_CONTENT;
        if (kind == static_cast<byte>(PARSE::ConstKind::boolean) && at64(bytes, at + 0x8) > 1)
            return PARSE::BAD_CONTENT;

        imageData.constants.push_back({static_cast<PARSE::ConstKind>(kind), at, payload});
        at += entrySize;
    }
    if (at != bytes.size()) // exact cover
        return PARSE::BAD_CONTENT;
    return PARSE::OK;
}

int Parse::readCode(void)
{
    const std::span<const byte> bytes = sectionBytes(PARSE::SECTION_CODE);
    if ((bytes.size() % 4u) != 0 || (bytes.size() & 7u) != 0) // N must be even
        return PARSE::BAD_CONTENT;
    imageData.code = bytes;
    imageData.codeCount = static_cast<dword>(bytes.size() / 4);
    return PARSE::OK;
}

int Parse::readFunc(void)
{
    const std::span<const byte> bytes = sectionBytes(PARSE::SECTION_FUNCS);
    if (bytes.size() < 0x8)
        return PARSE::BAD_CONTENT;

    const dword count = at32(bytes, 0);
    if (count > PARSE::MAX_STATIC_COUNT)
        return PARSE::BAD_CONTENT;
    if (static_cast<qword>(bytes.size()) != 0x8 + 0x20ull * count)
        return PARSE::BAD_CONTENT;

    const std::span<const byte> debug = sectionBytes(PARSE::SECTION_DEBUG);
    std::unordered_set<std::string_view> names;
    std::vector<std::pair<qword, qword>> ranges;
    imageData.functions.reserve(count);
    ranges.reserve(count);

    for (dword i = 0; i < count; ++i)
    {
        const qword at = 4 + static_cast<qword>(i) * 0x20;
        PARSE::Function fn{};
        fn.name = at32(bytes, at);
        fn.entry = at32(bytes, at + 4);
        fn.instr = at32(bytes, at + 8);
        fn.regcnt = at16(bytes, at + 12);
        fn.arity = at8(bytes, at + 14);
        fn.flags = at8(bytes, at + 15);
        fn.local = at16(bytes, at + 16);
        fn.reserved0 = at16(bytes, at + 18);
        fn.dbgOffset = at32(bytes, at + 20);
        fn.reserved1 = at32(bytes, at + 24);
        fn.reserved2 = at32(bytes, at + 28);

        if (fn.name != NO_INDEX)
        {
            if (!isStringConstant(fn.name))
                return PARSE::BAD_CONTENT;
            if (!names.insert(bytesView(imageData.constants[fn.name].payload)).second)
                return PARSE::BAD_CONTENT; // duplicate name
        }
        if (fn.instr < 1)
            return PARSE::BAD_CONTENT;
        if (static_cast<qword>(fn.entry) + fn.instr > imageData.codeCount)
            return PARSE::BAD_CONTENT;
        if (fn.regcnt < 1 || fn.regcnt > ISA::REGISTER_COUNT)
            return PARSE::BAD_CONTENT;
        if (fn.arity > 16 || fn.arity > fn.regcnt)
            return PARSE::BAD_CONTENT;
        if ((fn.flags & 0xFEu) != 0)
            return PARSE::BAD_CONTENT;
        if (fn.local > fn.regcnt)
            return PARSE::BAD_CONTENT;
        if (fn.reserved0 != 0 || fn.reserved1 != 0 || fn.reserved2 != 0)
            return PARSE::BAD_CONTENT;
        if (fn.dbgOffset != NO_INDEX)
        {
            if (debug.empty() || (fn.dbgOffset & 7u) != 0 || !inBounds(fn.dbgOffset, 0x10, debug.size()))
                return PARSE::BAD_CONTENT;
        }

        ranges.emplace_back(fn.entry, static_cast<qword>(fn.entry) + fn.instr);
        imageData.functions.push_back(fn);
    }

    if (!allZero(bytes, 4 + 0x20ull * count, bytes.size())) // tail pad
        return PARSE::BAD_CONTENT;
    if (rangesOverlap(std::move(ranges)))
        return PARSE::BAD_CONTENT;
    return PARSE::OK;
}

int Parse::readGlobal(void)
{
    const std::span<const byte> bytes = sectionBytes(PARSE::SECTION_GLOBALS);
    if (bytes.size() < 0x8)
        return PARSE::BAD_CONTENT;

    const dword count = at32(bytes, 0);
    if (count > PARSE::MAX_STATIC_COUNT)
        return PARSE::BAD_CONTENT;
    if (static_cast<qword>(bytes.size()) != align8(4ull + 12ull * count))
        return PARSE::BAD_CONTENT;

    std::unordered_set<std::string_view> names;
    imageData.globals.reserve(count);
    for (dword i = 0; i < count; ++i)
    {
        const qword at = 4 + static_cast<qword>(i) * 12;
        PARSE::Global global{};
        global.name = at32(bytes, at);
        global.init = at32(bytes, at + 4);
        global.flags = at32(bytes, at + 8);

        if (global.name != NO_INDEX)
        {
            if (!isStringConstant(global.name))
                return PARSE::BAD_CONTENT;
            if (!names.insert(bytesView(imageData.constants[global.name].payload)).second)
                return PARSE::BAD_CONTENT; // duplicate name
        }
        if (global.init != NO_INDEX && global.init >= imageData.constants.size())
            return PARSE::BAD_CONTENT;
        if ((global.flags & ~PARSE::GLOBAL_FLAG_CONST) != 0)
            return PARSE::BAD_CONTENT;

        imageData.globals.push_back(global);
    }

    if (!allZero(bytes, 4 + 12ull * count, bytes.size())) // pad
        return PARSE::BAD_CONTENT;
    return PARSE::OK;
}

int Parse::readImport(void)
{
    const std::span<const byte> bytes = sectionBytes(PARSE::SECTION_IMPORTS);
    if (bytes.size() < 0x8)
        return PARSE::BAD_CONTENT;

    const dword count = at32(bytes, 0);
    if (count > PARSE::MAX_STATIC_COUNT)
        return PARSE::BAD_CONTENT;
    if (static_cast<qword>(bytes.size()) != 0x8 + 8ull * count)
        return PARSE::BAD_CONTENT;

    std::unordered_set<std::string_view> names;
    imageData.imports.reserve(count);
    for (dword i = 0; i < count; ++i)
    {
        const qword at = 4 + static_cast<qword>(i) * 8;
        PARSE::Import import{};
        import.name = at32(bytes, at);
        import.arity = at8(bytes, at + 4);
        import.flags = at8(bytes, at + 5);
        import.reserved = at16(bytes, at + 6);

        if (!isStringConstant(import.name)) // anonymous not allowed
            return PARSE::BAD_CONTENT;
        if (!names.insert(bytesView(imageData.constants[import.name].payload)).second)
            return PARSE::BAD_CONTENT; // duplicate name
        if (import.arity > 16 || import.flags != 0 || import.reserved != 0)
            return PARSE::BAD_CONTENT;

        imageData.imports.push_back(import);
    }

    if (!allZero(bytes, 4 + 8ull * count, bytes.size())) // tail pad
        return PARSE::BAD_CONTENT;
    return PARSE::OK;
}

int Parse::readDebug(void)
{
    const std::span<const byte> bytes = sectionBytes(PARSE::SECTION_DEBUG);
    if (bytes.empty()) // no DEBUG
        return PARSE::OK;
    if (bytes.size() < 0x8 || (bytes.size() & 7u) != 0)
        return PARSE::BAD_CONTENT;
    if (at8(bytes, 0) != PARSE::DEBUG_FORMAT_VERSION)
        return PARSE::BAD_CONTENT;
    if (at8(bytes, 1) != 0 || at16(bytes, 2) != 0 || at32(bytes, 4) != 0)
        return PARSE::BAD_CONTENT;

    imageData.debugHeader.version = at8(bytes, 0);
    imageData.debugHeader.reserved0 = at8(bytes, 1);
    imageData.debugHeader.reserved1 = at16(bytes, 2);
    imageData.debugHeader.flags = at32(bytes, 4);

    std::vector<std::pair<qword, qword>> ranges;
    imageData.debug.resize(imageData.functions.size());
    for (dword i = 0; i < static_cast<dword>(imageData.functions.size()); ++i)
    {
        const PARSE::Function &fn = imageData.functions[i];
        if (fn.dbgOffset == NO_INDEX)
            continue;

        const qword at = fn.dbgOffset;
        if (!inBounds(at, 0x10, bytes.size()))
            return PARSE::BAD_CONTENT;

        const dword function = at32(bytes, at);
        const dword lines = at32(bytes, at + 4);
        const dword locals = at32(bytes, at + 8);
        if (function != i || at32(bytes, at + 12) != 0)
            return PARSE::BAD_CONTENT;
        if (lines > fn.instr || locals > PARSE::MAX_STATIC_COUNT)
            return PARSE::BAD_CONTENT;

        const qword blockSize = align8(0x10 + 12ull * lines + 16ull * locals);
        if (!inBounds(at, blockSize, bytes.size()))
            return PARSE::BAD_CONTENT;

        PARSE::DebugInfo info;
        info.function = i;
        info.lines.reserve(lines);
        info.locals.reserve(locals);

        qword cursor = at + 0x10;
        for (dword k = 0; k < lines; ++k, cursor += 12)
        {
            PARSE::LineEntry entry{};
            entry.pc = at32(bytes, cursor);
            entry.line = at32(bytes, cursor + 4);
            entry.column = at32(bytes, cursor + 8);
            if (k == 0 && entry.pc != 0)
                return PARSE::BAD_CONTENT;
            if (k > 0 && entry.pc <= info.lines.back().pc)
                return PARSE::BAD_CONTENT;
            if (entry.line == 0 || entry.line > 0x7FFFFFFFu || entry.pc >= fn.instr)
                return PARSE::BAD_CONTENT;
            info.lines.push_back(entry);
        }

        std::unordered_set<word> slots;
        for (dword k = 0; k < locals; ++k, cursor += 16)
        {
            PARSE::LocalEntry entry{};
            entry.name = at32(bytes, cursor);
            entry.slot = at16(bytes, cursor + 4);
            entry.flags = at8(bytes, cursor + 6);
            entry.reserved = at8(bytes, cursor + 7);
            entry.startPc = at32(bytes, cursor + 8);
            entry.endPc = at32(bytes, cursor + 12);
            if (!isStringConstant(entry.name))
                return PARSE::BAD_CONTENT;
            if (entry.slot >= fn.regcnt)
                return PARSE::BAD_CONTENT;
            if ((entry.flags & 0xFEu) != 0 || entry.reserved != 0)
                return PARSE::BAD_CONTENT;
            if (entry.startPc > entry.endPc || entry.endPc > fn.instr)
                return PARSE::BAD_CONTENT;
            if ((entry.flags & PARSE::LOCAL_FLAG_PARAMETER) != 0 && entry.slot >= fn.arity)
                return PARSE::BAD_CONTENT;
            slots.insert(entry.slot);
            info.locals.push_back(entry);
        }

        if (static_cast<std::size_t>(fn.local) != slots.size()) // V45
            return PARSE::BAD_CONTENT;

        ranges.emplace_back(at, at + blockSize);
        imageData.debug[i] = std::move(info);
    }

    if (rangesOverlap(std::move(ranges))) // V75
        return PARSE::BAD_CONTENT;
    return PARSE::OK;
}

int Parse::readCrossTable(void)
{
    if (imageData.header.entry >= imageData.functions.size()) // V76
        return PARSE::BAD_CONTENT;
    if (imageData.functions[imageData.header.entry].arity != 0) // V77
        return PARSE::BAD_CONTENT;
    return PARSE::OK;
}

const char *PARSE::errorText(int code)
{
    switch (code)
    {
    case PARSE::OK:
        return "ok";
    case PARSE::FAIL_OPEN:
        return "cannot open module file";
    case PARSE::EMPTY:
        return "module file is empty";
    case PARSE::TRUNC:
        return "module file is truncated";
    case PARSE::TOO_LARGE:
        return "module exceeds the 64 MiB limit";
    case PARSE::BAD_FILE:
        return "invalid header, section table or checksum";
    case PARSE::BAD_CONTENT:
        return "invalid section content";
    case PARSE::BAD_CODE:
        return "invalid instruction stream (verifier)";
    default:
        return "unknown load error";
    }
}
