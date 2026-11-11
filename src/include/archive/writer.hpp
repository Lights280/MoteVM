#pragma once
// Deterministic MTR 1.0 writer (plan/archieve/mtr-format.md §9).

#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "define.hpp"
#include "archive/archive.hpp"

namespace ARCHIVE
{
    class Writer
    {
    private:
        struct InputEntry
        {
            std::string name;
            EntryKind kind = EntryKind::resource;
            std::vector<byte> data;
        };

        std::vector<InputEntry> entryList;
        std::unordered_set<std::string> exactNames;
        std::unordered_set<std::string> foldedNames;
        Manifest metadata;

    private:
        bool nameTaken(std::string_view name) const;

    public:
        void clear(void);

        // Validates the name, the .mbc suffix and the module with the MoteBC
        // parser/verifier, then copies it into the writer.
        bool addModule(std::string_view name, std::span<const byte> data, std::string &error);
        bool addResource(std::string_view name, std::span<const byte> data, std::string &error);

        bool setMetadata(word key, std::string_view value, std::string &error);

        std::size_t moduleCount(void) const;
        std::size_t resourceCount(void) const;

        // Builds the canonical archive bytes. If the manifest has no entry
        // module and the package has exactly one module, key 3 is filled in.
        bool build(std::vector<byte> &out, std::string &error) const;
    };
}
