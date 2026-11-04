#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "common/types.hpp"

namespace TOOL
{
    // Strict UTF-8 validation (rejects overlongs, surrogates, > U+10FFFF).
    bool validUtf8(std::span<const byte> data, std::size_t &badOffset);
    bool validUtf8(std::string_view text);

    // 1-based Unicode code point column of byteOffset inside line.
    std::size_t codePointColumn(std::string_view line, std::size_t byteOffset);

    // [A-Za-z_][A-Za-z0-9_]* (plan/05 §5.3).
    bool isIdentifier(std::string_view text);

    std::string toLowerAscii(std::string_view text);

    // Canonical string escaping (plan/05 §2.3): \n \r \t \" \\ plus \xHH for
    // other control bytes and bytes that are not part of valid UTF-8.
    std::string escapeBytes(std::span<const byte> bytes);
    std::string escapeBytes(std::string_view text);

    std::string formatInt64(std::int64_t value);

    // Canonical f64 rendering: shortest round-trip, integral values get ".0",
    // -0.0 stays "-0.0", nan/inf/-inf, and f64=0x<16 hex> when the bit pattern
    // cannot be reproduced (NaN payloads).
    std::string formatFloatBits(std::uint64_t bits);

    // Parses the literal forms accepted by moteasm (including f64=0x...).
    bool parseFloatBits(std::string_view text, std::uint64_t &bits);

    // Integer literal parsing: [-]dec, [-]0xHEX, [-]0bBIN, '_' separators.
    bool parseInt64(std::string_view text, std::int64_t &value);
    bool parseUInt64(std::string_view text, std::uint64_t &value);

    std::string hexFixed(std::uint64_t value, int width);
}
