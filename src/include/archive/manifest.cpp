#include "archive/manifest.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "archive/util.hpp"

namespace
{
    bool knownKey(word key)
    {
        return key >= ARCHIVE::META_PACKAGE_NAME && key <= ARCHIVE::META_CREATED_BY;
    }

    bool validateKnown(word key, std::span<const byte> value, std::string &error)
    {
        if (key == 0)
        {
            error = "manifest key 0 is reserved";
            return false;
        }

        std::size_t minimum = 0;
        std::size_t maximum = 65535;
        switch (key)
        {
        case ARCHIVE::META_PACKAGE_NAME:
            minimum = 1;
            maximum = 255;
            break;
        case ARCHIVE::META_PACKAGE_VERSION:
            minimum = 1;
            maximum = 64;
            break;
        case ARCHIVE::META_ENTRY_MODULE:
            minimum = 1;
            maximum = ARCHIVE::MAX_NAME_BYTES;
            break;
        case ARCHIVE::META_DESCRIPTION:
            maximum = 4096;
            break;
        case ARCHIVE::META_AUTHOR:
        case ARCHIVE::META_LICENSE:
        case ARCHIVE::META_BUILD_ID:
        case ARCHIVE::META_CREATED_BY:
            maximum = 255;
            break;
        default:
            return true; // unknown keys are opaque metadata
        }

        if (value.size() < minimum || value.size() > maximum)
        {
            error = "manifest key " + std::to_string(key) + " has invalid length " +
                    std::to_string(value.size());
            return false;
        }

        if (knownKey(key))
        {
            std::size_t badOffset = 0;
            if (!ARCHIVE::validText(value, badOffset))
            {
                error = "manifest key " + std::to_string(key) +
                        " contains invalid UTF-8 or a control byte at offset " +
                        std::to_string(badOffset);
                return false;
            }
        }
        return true;
    }
}

bool ARCHIVE::Manifest::set(word key, std::string_view value, std::string &error)
{
    return setBytes(key, std::span<const byte>(reinterpret_cast<const byte *>(value.data()), value.size()),
                    error);
}

bool ARCHIVE::Manifest::setBytes(word key, std::span<const byte> value, std::string &error)
{
    if (!validateKnown(key, value, error))
        return false;

    const auto position = std::lower_bound(
        records.begin(), records.end(), key,
        [](const ManifestItem &item, word candidate)
        { return item.key < candidate; });

    if (position != records.end() && position->key == key)
    {
        position->value.assign(value.begin(), value.end());
        return true;
    }

    ManifestItem item;
    item.key = key;
    item.value.assign(value.begin(), value.end());
    records.insert(position, std::move(item));
    return true;
}

const ARCHIVE::ManifestItem *ARCHIVE::Manifest::find(word key) const
{
    for (const ManifestItem &item : records)
    {
        if (item.key == key)
            return &item;
        if (item.key > key)
            break;
    }
    return nullptr;
}

std::string ARCHIVE::Manifest::text(word key) const
{
    const ManifestItem *item = find(key);
    if (item == nullptr || item->value.empty())
        return std::string();
    return std::string(reinterpret_cast<const char *>(item->value.data()), item->value.size());
}

bool ARCHIVE::Manifest::encode(std::vector<byte> &out, std::string &error) const
{
    out.clear();
    word previous = 0;
    bool first = true;

    for (const ManifestItem &item : records)
    {
        if (!first && item.key <= previous)
        {
            error = "manifest keys are not strictly ascending";
            return false;
        }
        first = false;
        previous = item.key;

        if (!validateKnown(item.key, item.value, error))
            return false;

        const std::size_t length = item.value.size();
        if (length > 0xFFFFu)
        {
            error = "manifest payload is larger than 65535 bytes";
            return false;
        }

        const qword recordSize = align8(static_cast<qword>(4 + length));
        const qword nextSize = static_cast<qword>(out.size()) + recordSize;
        if (nextSize > MAX_MANIFEST_BYTES)
        {
            error = "manifest is larger than 1 MiB";
            return false;
        }

        const std::size_t begin = out.size();
        out.resize(begin + static_cast<std::size_t>(recordSize), 0);
        put16(out, static_cast<qword>(begin), item.key);
        put16(out, static_cast<qword>(begin) + 2, static_cast<word>(length));
        if (length != 0)
            std::copy(item.value.begin(), item.value.end(), out.begin() + static_cast<std::ptrdiff_t>(begin + 4));
    }
    return true;
}

bool ARCHIVE::Manifest::decode(std::span<const byte> data, Manifest &out, std::string &error)
{
    if (data.size() > static_cast<std::size_t>(MAX_MANIFEST_BYTES))
    {
        error = "manifest is larger than 1 MiB";
        return false;
    }
    if ((data.size() & 7u) != 0)
    {
        error = "manifest size is not a multiple of 8";
        return false;
    }

    Manifest parsed;
    qword offset = 0;
    const qword total = static_cast<qword>(data.size());
    word previous = 0;
    bool first = true;

    while (offset < total)
    {
        if (!inBounds(offset, 4, total))
        {
            error = "truncated manifest TLV header at offset " + std::to_string(offset);
            return false;
        }

        const word key = at16(data, offset);
        const word length = at16(data, offset + 2);
        if (key == 0)
        {
            error = "manifest key 0 is reserved at offset " + std::to_string(offset);
            return false;
        }
        if (!first && key <= previous)
        {
            error = "manifest keys are not strictly ascending at offset " +
                    std::to_string(offset);
            return false;
        }
        first = false;
        previous = key;

        const qword recordSize = align8(static_cast<qword>(4) + length);
        if (!inBounds(offset, recordSize, total))
        {
            error = "manifest TLV record exceeds the manifest at offset " +
                    std::to_string(offset);
            return false;
        }

        const std::span<const byte> value =
            data.subspan(static_cast<std::size_t>(offset + 4), static_cast<std::size_t>(length));
        if (!validateKnown(key, value, error))
            return false;
        if (!allZero(data, offset + 4 + length, offset + recordSize))
        {
            error = "manifest TLV padding is not zero at offset " + std::to_string(offset);
            return false;
        }

        ManifestItem item;
        item.key = key;
        item.value.assign(value.begin(), value.end());
        parsed.records.push_back(std::move(item));

        offset += recordSize;
    }

    out.records = std::move(parsed.records);
    return true;
}
