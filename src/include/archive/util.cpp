#include "archive/util.hpp"

namespace
{
    bool continuation(byte value)
    {
        return value >= 0x80 && value <= 0xBF;
    }
}

bool ARCHIVE::validUtf8(std::span<const byte> data, std::size_t &badOffset)
{
    std::size_t i = 0;
    while (i < data.size())
    {
        const byte lead = data[i];
        if (lead <= 0x7F)
        {
            ++i;
            continue;
        }

        if (lead >= 0xC2 && lead <= 0xDF)
        {
            if (i + 1 >= data.size() || !continuation(data[i + 1]))
            {
                badOffset = i;
                return false;
            }
            i += 2;
            continue;
        }

        if (lead >= 0xE0 && lead <= 0xEF)
        {
            if (i + 2 >= data.size())
            {
                badOffset = i;
                return false;
            }
            const byte second = data[i + 1];
            const byte third = data[i + 2];
            bool ok = continuation(third);
            if (lead == 0xE0)
                ok = ok && second >= 0xA0 && second <= 0xBF;
            else if (lead == 0xED)
                ok = ok && second >= 0x80 && second <= 0x9F; // exclude surrogates
            else
                ok = ok && continuation(second);
            if (!ok)
            {
                badOffset = i;
                return false;
            }
            i += 3;
            continue;
        }

        if (lead >= 0xF0 && lead <= 0xF4)
        {
            if (i + 3 >= data.size())
            {
                badOffset = i;
                return false;
            }
            const byte second = data[i + 1];
            const byte third = data[i + 2];
            const byte fourth = data[i + 3];
            bool ok = continuation(third) && continuation(fourth);
            if (lead == 0xF0)
                ok = ok && second >= 0x90 && second <= 0xBF;
            else if (lead == 0xF4)
                ok = ok && second >= 0x80 && second <= 0x8F; // <= U+10FFFF
            else
                ok = ok && continuation(second);
            if (!ok)
            {
                badOffset = i;
                return false;
            }
            i += 4;
            continue;
        }

        badOffset = i;
        return false;
    }
    return true;
}

bool ARCHIVE::allZero(std::span<const byte> data, qword begin, qword end)
{
    if (begin > end || end > static_cast<qword>(data.size()))
        return false;
    for (qword i = begin; i < end; ++i)
        if (data[indexOf(i)] != 0)
            return false;
    return true;
}

bool ARCHIVE::validText(std::span<const byte> data, std::size_t &badOffset)
{
    if (!validUtf8(data, badOffset))
        return false;
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        if (data[i] < 0x20 || data[i] == 0x7F)
        {
            badOffset = i;
            return false;
        }
    }
    return true;
}
