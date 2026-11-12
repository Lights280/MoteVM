#pragma once
// MTR manifest: a small TLV list (key u16, length u16, payload, 8-byte pad).
// The same shape as the MoteBC META section, but top-level and package-wide.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "define.hpp"

namespace ARCHIVE
{
    inline constexpr word META_PACKAGE_NAME = 1;
    inline constexpr word META_PACKAGE_VERSION = 2;
    inline constexpr word META_ENTRY_MODULE = 3;
    inline constexpr word META_DESCRIPTION = 4;
    inline constexpr word META_AUTHOR = 5;
    inline constexpr word META_LICENSE = 6;
    inline constexpr word META_BUILD_ID = 7;
    inline constexpr word META_CREATED_BY = 8;

    struct ManifestItem
    {
        word key = 0;
        std::vector<byte> value;
    };

    class Manifest
    {
    private:
        std::vector<ManifestItem> records; // strictly ascending by key

    public:
        void clear(void)
        {
            records.clear();
        }

        // Replaces an existing key or inserts it in ascending order.
        bool set(word key, std::string_view value, std::string &error);
        bool setBytes(word key, std::span<const byte> value, std::string &error);

        const ManifestItem *find(word key) const;
        std::string text(word key) const;
        const std::vector<ManifestItem> &items(void) const
        {
            return records;
        }

        // Encodes records in ascending key order with zero padding.
        bool encode(std::vector<byte> &out, std::string &error) const;

        // Parses a complete manifest byte range. Rejects duplicates, unsorted
        // keys, non-zero padding, invalid known-key payloads and malformed TLV.
        static bool decode(std::span<const byte> data, Manifest &out, std::string &error);
    };
}
