#pragma once
#include <span>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace TOOL
{
    struct Diagnostic
    {
        Severity severity = Severity::Error;
        std::string code; // E001.. or W001..
        SourcePosition position;
        std::string message;
    };

    // Deterministic diagnostic collector (plan/05 §6.6).
    class Diagnostics
    {
    private:
        std::vector<Diagnostic> records;
        int limit = 20; // --max-errors; 0 = unlimited (capped at 1000 records)
        int errors = 0;
        int warnings = 0;
        int storedErrors = 0;
        int storedWarnings = 0;
        bool warnAsError = false;

        int cap(void) const { return limit == 0 ? 1000 : limit; }

    public:
        Diagnostics() = default;

        void setLimit(int value) { limit = value < 0 ? 0 : value; }
        void setWarnAsError(bool value) { warnAsError = value; }

        void error(const std::string &code, SourcePosition position, std::string message);
        void warning(const std::string &code, SourcePosition position, std::string message);

        bool failed(void) const { return errors != 0 || (warnAsError && warnings != 0); }
        int errorCount(void) const { return errors; }
        int warningCount(void) const { return warnings; }
        bool empty(void) const { return records.empty(); }
        const std::vector<Diagnostic> &items(void) const { return records; }

        // Sorts by (line, column, code, message) and removes exact duplicates.
        void sortAndDedup(void);

        // One line per diagnostic plus the summary line; empty when no diagnostics.
        std::string render(const std::string &tool, const std::string &path) const;
    };

    // First differing byte (or length mismatch) rendered for RT-1 failures.
    std::string firstDifference(std::span<const byte> expected, std::span<const byte> actual);
}
