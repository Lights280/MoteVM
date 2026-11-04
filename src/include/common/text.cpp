#include "common/text.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace
{
    // Decodes one UTF-8 sequence. Rejects overlongs, surrogates and > U+10FFFF.
    bool decodeOne(std::span<const byte> data, std::size_t &index, std::uint32_t &codePoint)
    {
        if (index >= data.size())
            return false;
        const byte first = data[index];
        if (first < 0x80)
        {
            codePoint = first;
            ++index;
            return true;
        }

        int extra = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if ((first & 0xE0u) == 0xC0u)
        {
            extra = 1;
            value = first & 0x1Fu;
            minimum = 0x80u;
        }
        else if ((first & 0xF0u) == 0xE0u)
        {
            extra = 2;
            value = first & 0x0Fu;
            minimum = 0x800u;
        }
        else if ((first & 0xF8u) == 0xF0u)
        {
            extra = 3;
            value = first & 0x07u;
            minimum = 0x10000u;
        }
        else
        {
            return false;
        }

        if (index + static_cast<std::size_t>(extra) >= data.size())
            return false;
        for (int i = 1; i <= extra; ++i)
        {
            const byte next = data[index + static_cast<std::size_t>(i)];
            if ((next & 0xC0u) != 0x80u)
                return false;
            value = (value << 6) | (next & 0x3Fu);
        }
        if (value < minimum || value > 0x10FFFFu)
            return false;
        if (value >= 0xD800u && value <= 0xDFFFu)
            return false;

        codePoint = value;
        index += static_cast<std::size_t>(extra) + 1;
        return true;
    }

    bool parseDigits(std::string_view text, int base, std::uint64_t limit, std::uint64_t &value)
    {
        if (text.empty())
            return false;
        std::uint64_t result = 0;
        bool previousDigit = false;
        bool sawDigit = false;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char character = text[i];
            if (character == '_')
            {
                if (!previousDigit || i + 1 >= text.size())
                    return false;
                previousDigit = false;
                continue;
            }
            int digit = -1;
            if (character >= '0' && character <= '9')
                digit = character - '0';
            else if (character >= 'a' && character <= 'f')
                digit = character - 'a' + 10;
            else if (character >= 'A' && character <= 'F')
                digit = character - 'A' + 10;
            if (digit < 0 || digit >= base)
                return false;
            if (result > (limit - static_cast<std::uint64_t>(digit)) /
                             static_cast<std::uint64_t>(base))
                return false;
            result = result * static_cast<std::uint64_t>(base) +
                     static_cast<std::uint64_t>(digit);
            sawDigit = true;
            previousDigit = true;
        }
        if (!sawDigit)
            return false;
        value = result;
        return true;
    }

    void splitPrefix(std::string_view text, std::string_view &digits, int &base)
    {
        if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            digits = text.substr(2);
            base = 16;
            return;
        }
        if (text.size() >= 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B'))
        {
            digits = text.substr(2);
            base = 2;
            return;
        }
        digits = text;
        base = 10;
    }

    void appendHexEscape(std::string &out, byte value)
    {
        static constexpr char DIGITS[] = "0123456789ABCDEF";
        out += '\\';
        out += 'x';
        out += DIGITS[(value >> 4) & 0x0Fu];
        out += DIGITS[value & 0x0Fu];
    }
}

bool TOOL::validUtf8(std::span<const byte> data, std::size_t &badOffset)
{
    std::size_t index = 0;
    while (index < data.size())
    {
        const std::size_t start = index;
        std::uint32_t codePoint = 0;
        if (!decodeOne(data, index, codePoint))
        {
            badOffset = start;
            return false;
        }
    }
    badOffset = data.size();
    return true;
}

bool TOOL::validUtf8(std::string_view text)
{
    std::size_t bad = 0;
    return validUtf8(
        std::span<const byte>(reinterpret_cast<const byte *>(text.data()), text.size()), bad);
}

std::size_t TOOL::codePointColumn(std::string_view line, std::size_t byteOffset)
{
    if (byteOffset > line.size())
        byteOffset = line.size();
    std::size_t column = 1;
    for (std::size_t i = 0; i < byteOffset; ++i)
    {
        const unsigned char character = static_cast<unsigned char>(line[i]);
        if ((character & 0xC0u) != 0x80u)
            ++column;
    }
    return column;
}

bool TOOL::isIdentifier(std::string_view text)
{
    if (text.empty())
        return false;
    const auto isStart = [](char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') || character == '_';
    };
    const auto isPart = [&isStart](char character) {
        return isStart(character) || (character >= '0' && character <= '9');
    };
    if (!isStart(text[0]))
        return false;
    for (std::size_t i = 1; i < text.size(); ++i)
        if (!isPart(text[i]))
            return false;
    return true;
}

std::string TOOL::toLowerAscii(std::string_view text)
{
    std::string lowered;
    lowered.reserve(text.size());
    for (char character : text)
    {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
        lowered.push_back(character);
    }
    return lowered;
}


std::string TOOL::escapeBytes(std::span<const byte> bytes)
{
    std::string out;
    out.reserve(bytes.size() + 8);
    std::size_t index = 0;
    while (index < bytes.size())
    {
        const byte value = bytes[index];
        switch (value)
        {
        case '\\':
            out += "\\\\";
            ++index;
            continue;
        case '"':
            out += "\\\"";
            ++index;
            continue;
        case '\n':
            out += "\\n";
            ++index;
            continue;
        case '\r':
            out += "\\r";
            ++index;
            continue;
        case '\t':
            out += "\\t";
            ++index;
            continue;
        default:
            break;
        }
        if (value >= 0x20 && value < 0x7F)
        {
            out.push_back(static_cast<char>(value));
            ++index;
            continue;
        }
        if (value >= 0x80)
        {
            const std::size_t start = index;
            std::uint32_t codePoint = 0;
            if (decodeOne(bytes, index, codePoint))
            {
                out.append(reinterpret_cast<const char *>(bytes.data()) + start, index - start);
                continue;
            }
            index = start;
        }
        appendHexEscape(out, value);
        ++index;
    }
    return out;
}

std::string TOOL::escapeBytes(std::string_view text)
{
    return escapeBytes(
        std::span<const byte>(reinterpret_cast<const byte *>(text.data()), text.size()));
}

std::string TOOL::formatInt64(std::int64_t value)
{
    return std::to_string(value);
}


std::string TOOL::formatFloatBits(std::uint64_t bits)
{
    const double value = std::bit_cast<double>(bits);
    if (std::isnan(value))
    {
        if (bits == 0x7FF8000000000000ull)
            return "nan";
        if (bits == 0xFFF8000000000000ull)
            return "-nan";
    }
    else if (std::isinf(value))
    {
        return bits == 0xFFF0000000000000ull ? "-inf" : "inf";
    }
    else
    {
        char buffer[64] = {};
        const std::to_chars_result result =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
        if (result.ec == std::errc())
        {
            std::string text(buffer, result.ptr);
            if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
                text.find('E') == std::string::npos)
                text += ".0";
            std::uint64_t parsed = 0;
            if (parseFloatBits(text, parsed) && parsed == bits)
                return text;
        }
    }
    return "f64=" + hexFixed(bits, 16);
}

bool TOOL::parseFloatBits(std::string_view text, std::uint64_t &bits)
{
    if (text.size() > 4 && text.substr(0, 4) == "f64=")
    {
        const std::string_view payload = text.substr(4);
        std::string_view digits;
        int base = 10;
        splitPrefix(payload, digits, base);
        std::uint64_t value = 0;
        if (base != 16 ||
            !parseDigits(digits, 16, std::numeric_limits<std::uint64_t>::max(), value))
            return false;
        bits = value;
        return true;
    }

    std::string literal(text);
    if (!literal.empty() && (literal.back() == 'f' || literal.back() == 'F'))
        literal.pop_back();

    const std::string lowered = toLowerAscii(literal);
    if (lowered == "nan" || lowered == "+nan")
    {
        bits = 0x7FF8000000000000ull;
        return true;
    }
    if (lowered == "-nan")
    {
        bits = 0xFFF8000000000000ull;
        return true;
    }
    if (lowered == "inf" || lowered == "+inf")
    {
        bits = 0x7FF0000000000000ull;
        return true;
    }
    if (lowered == "-inf")
    {
        bits = 0xFFF0000000000000ull;
        return true;
    }

    std::string cleaned;
    cleaned.reserve(literal.size() + 1);
    for (char character : literal)
        if (character != '_')
            cleaned.push_back(character);
    if (cleaned.empty())
        return false;
    if (cleaned[0] == '.')
        cleaned.insert(cleaned.begin(), '0');
    else if (cleaned.size() >= 2 && (cleaned[0] == '-' || cleaned[0] == '+') && cleaned[1] == '.')
        cleaned.insert(cleaned.begin() + 1, '0');

    double value = 0.0;
    const std::from_chars_result result =
        std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), value,
                        std::chars_format::general);
    if (result.ec != std::errc() || result.ptr != cleaned.data() + cleaned.size())
        return false;
    bits = std::bit_cast<std::uint64_t>(value);
    return true;
}


bool TOOL::parseInt64(std::string_view text, std::int64_t &value)
{
    bool negative = false;
    if (!text.empty() && text[0] == '-')
    {
        negative = true;
        text.remove_prefix(1);
    }
    else if (!text.empty() && text[0] == '+')
    {
        text.remove_prefix(1);
    }
    if (text.empty())
        return false;
    std::string_view digits;
    int base = 10;
    splitPrefix(text, digits, base);
    const std::uint64_t limit = negative ? (1ull << 63) : ((1ull << 63) - 1ull);
    std::uint64_t magnitude = 0;
    if (!parseDigits(digits, base, limit, magnitude))
        return false;
    if (negative)
    {
        if (magnitude == (1ull << 63))
            value = std::numeric_limits<std::int64_t>::min();
        else
            value = -static_cast<std::int64_t>(magnitude);
    }
    else
    {
        value = static_cast<std::int64_t>(magnitude);
    }
    return true;
}

bool TOOL::parseUInt64(std::string_view text, std::uint64_t &value)
{
    if (!text.empty() && text[0] == '+')
        text.remove_prefix(1);
    if (!text.empty() && text[0] == '-')
        return false;
    std::string_view digits;
    int base = 10;
    splitPrefix(text, digits, base);
    return parseDigits(digits, base, std::numeric_limits<std::uint64_t>::max(), value);
}

std::string TOOL::hexFixed(std::uint64_t value, int width)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(width) << std::setfill('0') << value;
    return out.str();
}

