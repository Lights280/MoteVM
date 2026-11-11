#pragma once
// MTR 1.0 reader and validator (plan/archieve/mtr-format.md).
//
// The archive owns its file bytes; Entry::data are read-only views into that
// buffer. parse() is fail-closed: any malformed field, non-canonical layout or
// invalid module rejects the whole archive.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "define.hpp"
#include "archive/manifest.hpp"

namespace ARCHIVE
{
    const int OK = 0;
    const int FAIL_OPEN = 1;
    const int EMPTY = 2;
    const int TRUNC = 3;
    const int TOO_LARGE = 4;
    const int BAD_HEADER = 5;
    const int BAD_CHECKSUM = 6;
    const int BAD_TOC = 7;
    const int BAD_NAME = 8;
    const int DUPLICATE = 9;
    const int BAD_MANIFEST = 10;
    const int BAD_MODULE = 11;
    const int BAD_DATA = 12;
    const int BAD_ENTRY = 13;

    const char *errorText(int code);

    enum class EntryKind : byte
    {
        module = 1,
        resource = 2
    };

    struct Header
    {
        word versionMajor = 0;
        word versionMinor = 0;
        dword flags = 0;
        dword entryCount = 0;
        dword tocEntrySize = 0;
        qword tocOffset = 0;
        qword tocSize = 0;
        qword stringsOffset = 0;
        qword stringsSize = 0;
        qword manifestOffset = 0;
        qword manifestSize = 0;
        qword dataOffset = 0;
        qword fileSize = 0;
        dword checksum = 0;
        dword reserved0 = 0;
    };

    struct Entry
    {
        std::string name;
        EntryKind kind = EntryKind::resource;
        dword crc32c = 0;
        qword offset = 0;
        qword size = 0;
        std::span<const byte> data; // view into the Archive buffer
    };

    class Archive
    {
    private:
        std::vector<byte> buffer;
        Header headerData{};
        std::vector<Entry> entryList;
        Manifest manifestData;
        bool parsed = false;
        int lastError = OK;
        std::string lastDetail;

    private:
        void resetImage(void);
        int fail(int code, std::string detail);

    public:
        Archive() = default;
        ~Archive() = default;
        Archive(const Archive &) = delete; // owns spans into buffer
        Archive &operator=(const Archive &) = delete;
        Archive(Archive &&) noexcept = default;
        Archive &operator=(Archive &&) noexcept = default;

        // Whole-file read; enforces MAX_ARCHIVE_BYTES before allocating.
        int load(const std::string &path);
        // Copies bytes; call parse() afterwards.
        int load(std::span<const byte> bytes);
        // Verifies header, CRC, canonical layout, manifest, names and (by
        // default) every module with the MoteBC verifier.
        int parse(bool verifyModules = true);

        bool valid(void) const
        {
            return parsed;
        }
        const Header &header(void) const
        {
            return headerData;
        }
        const std::vector<Entry> &entries(void) const
        {
            return entryList;
        }
        const Manifest &manifest(void) const
        {
            return manifestData;
        }
        const Entry *find(std::string_view name) const;

        // Manifest entry (key 3), or the only module when key 3 is absent.
        // Empty when there is no unambiguous module entry.
        std::string entryModuleName(void) const;
        // Same, with a reason when no unique executable entry can be selected.
        bool resolveEntry(std::string &name, std::string &error) const;

        int error(void) const
        {
            return lastError;
        }
        const std::string &detail(void) const
        {
            return lastDetail;
        }
    };
}
