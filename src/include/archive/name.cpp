#include "archive/name.hpp"

#include <algorithm>
#include <span>

#include "archive/util.hpp"

namespace
{
    void setError(std::string *error, const std::string &text)
    {
        if (error != nullptr)
            *error = text;
    }

    bool equalsIgnoreCase(std::string_view text, std::string_view expected)
    {
        if (text.size() != expected.size())
            return false;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            char left = text[i];
            char right = expected[i];
            if (left >= 'A' && left <= 'Z')
                left = static_cast<char>(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z')
                right = static_cast<char>(right - 'A' + 'a');
            if (left != right)
                return false;
        }
        return true;
    }

    bool reservedDeviceName(std::string_view segment)
    {
        const std::size_t dot = segment.find('.');
        const std::string_view base = segment.substr(0, dot);

        if (equalsIgnoreCase(base, "CON") || equalsIgnoreCase(base, "PRN") ||
            equalsIgnoreCase(base, "AUX") || equalsIgnoreCase(base, "NUL"))
            return true;

        if (base.size() == 4)
        {
            const std::string_view prefix = base.substr(0, 3);
            const char digit = base[3];
            if (digit >= '0' && digit <= '9' &&
                (equalsIgnoreCase(prefix, "COM") || equalsIgnoreCase(prefix, "LPT")))
                return true;
        }
        return false;
    }
}

bool ARCHIVE::validName(std::string_view name, std::string *error)
{
    const std::size_t size = name.size();
    if (size == 0)
    {
        setError(error, "name is empty");
        return false;
    }
    if (size > MAX_NAME_BYTES)
    {
        setError(error, "name is longer than 1024 bytes");
        return false;
    }

    const std::span<const byte> bytes(reinterpret_cast<const byte *>(name.data()), size);
    std::size_t badOffset = 0;
    if (!validUtf8(bytes, badOffset))
    {
        setError(error, "name is not valid UTF-8 at byte " + std::to_string(badOffset));
        return false;
    }

    for (std::size_t i = 0; i < size; ++i)
    {
        const unsigned char value = static_cast<unsigned char>(name[i]);
        if (value < 0x20 || value == 0x7F)
        {
            setError(error, "name contains a control byte at offset " + std::to_string(i));
            return false;
        }
        if (name[i] == '\\')
        {
            setError(error, "name contains a backslash");
            return false;
        }
        if (name[i] == ':')
        {
            setError(error, "name contains a colon");
            return false;
        }
    }

    std::size_t start = 0;
    for (std::size_t i = 0; i <= size; ++i)
    {
        if (i != size && name[i] != '/')
            continue;

        const std::size_t length = i - start;
        if (length == 0)
        {
            setError(error, "name contains an empty path segment");
            return false;
        }
        if (length > MAX_SEGMENT_BYTES)
        {
            setError(error, "path segment is longer than 255 bytes");
            return false;
        }

        const std::string_view segment = name.substr(start, length);
        if (segment == "." || segment == "..")
        {
            setError(error, "name contains '.' or '..'");
            return false;
        }
        if (segment.back() == ' ' || segment.back() == '.')
        {
            setError(error, "path segment ends with a space or dot");
            return false;
        }
        if (reservedDeviceName(segment))
        {
            setError(error, "name uses a reserved Windows device name");
            return false;
        }

        start = i + 1;
    }

    return true;
}

std::string ARCHIVE::foldAscii(std::string_view name)
{
    std::string folded(name);
    for (char &value : folded)
        if (value >= 'A' && value <= 'Z')
            value = static_cast<char>(value - 'A' + 'a');
    return folded;
}

bool ARCHIVE::hasModuleSuffix(std::string_view name)
{
    const std::size_t slash = name.find_last_of('/');
    const std::size_t start = slash == std::string_view::npos ? 0 : slash + 1;
    const std::string_view segment = name.substr(start);
    if (segment.size() <= 4)
        return false;
    return equalsIgnoreCase(segment.substr(segment.size() - 4), ".mbc");
}
