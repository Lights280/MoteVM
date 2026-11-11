#pragma once
// MoteAR (.mtr) shared constants and bounded byte readers.
//
// Every value read from an archive is untrusted. These helpers keep the
// offset arithmetic explicit and overflow-safe; callers must still check
// inBounds() before using a span or reading a field.

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "define.hpp"

namespace ARCHIVE
{
    inline constexpr const char *MTR_VERSION = "1.0";

    inline constexpr qword HEADER_SIZE = 96;
    inline constexpr qword TOC_ENTRY_SIZE = 40;

    // Hard limits (plan/archieve/mtr-format.md §11).
    inline constexpr qword MAX_ARCHIVE_BYTES = 256ull << 20;
    inline constexpr qword MAX_ENTRY_BYTES = 64ull << 20;
    inline constexpr qword MAX_STRING_BYTES = 16ull << 20;
    inline constexpr qword MAX_MANIFEST_BYTES = 1ull << 20;
    inline constexpr dword MAX_ENTRY_COUNT = 65535;

    inline constexpr std::size_t MAX_NAME_BYTES = 1024;
    inline constexpr std::size_t MAX_SEGMENT_BYTES = 255;

    inline constexpr byte MAGIC[8] = {'M', 'O', 'T', 'E', 'A', 'R', 'C', 0x00};
    inline constexpr word SUPPORTED_MAJOR = 1;
    inline constexpr word SUPPORTED_MINOR = 0;
    inline constexpr dword FLAG_HAS_MANIFEST = 0x00000001u;

    inline constexpr qword HEADER_CHECKSUM_OFFSET = 88;
    inline constexpr qword HEADER_CHECKSUM_SIZE = 4;

    inline constexpr qword align8(qword value)
    {
        return (value + 7) & ~qword(7);
    }

    inline bool inBounds(qword offset, qword length, qword total)
    {
        return offset <= total && length <= total - offset;
    }

    inline std::size_t indexOf(qword offset)
    {
        return static_cast<std::size_t>(offset);
    }

    inline byte at8(std::span<const byte> data, qword offset)
    {
        return data[indexOf(offset)];
    }

    inline word at16(std::span<const byte> data, qword offset)
    {
        return static_cast<word>(at8(data, offset) |
                                 (static_cast<word>(at8(data, offset + 1)) << 8));
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

    inline void put8(std::vector<byte> &data, qword offset, byte value)
    {
        data[indexOf(offset)] = value;
    }

    inline void put16(std::vector<byte> &data, qword offset, word value)
    {
        data[indexOf(offset)] = static_cast<byte>(value & 0xFFu);
        data[indexOf(offset + 1)] = static_cast<byte>((value >> 8) & 0xFFu);
    }

    inline void put32(std::vector<byte> &data, qword offset, dword value)
    {
        data[indexOf(offset)] = static_cast<byte>(value & 0xFFu);
        data[indexOf(offset + 1)] = static_cast<byte>((value >> 8) & 0xFFu);
        data[indexOf(offset + 2)] = static_cast<byte>((value >> 16) & 0xFFu);
        data[indexOf(offset + 3)] = static_cast<byte>((value >> 24) & 0xFFu);
    }

    inline void put64(std::vector<byte> &data, qword offset, qword value)
    {
        put32(data, offset, static_cast<dword>(value & 0xFFFFFFFFull));
        put32(data, offset + 4, static_cast<dword>((value >> 32) & 0xFFFFFFFFull));
    }

    // Strict UTF-8 validation: rejects overlongs, surrogates and > U+10FFFF.
    bool validUtf8(std::span<const byte> data, std::size_t &badOffset);

    // True when every byte in [begin, end) is zero. The caller guarantees bounds.
    bool allZero(std::span<const byte> data, qword begin, qword end);

    // Text payloads (names and known manifest values) must also avoid NUL and
    // C0/DEL control bytes; validUtf8() alone allows them.
    bool validText(std::span<const byte> data, std::size_t &badOffset);
}
