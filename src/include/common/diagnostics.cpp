#include "common/diagnostics.hpp"

#include <algorithm>
#include <sstream>
#include <tuple>

#include "common/text.hpp"

namespace
{
    bool sameDiagnostic(const TOOL::Diagnostic &left, const TOOL::Diagnostic &right)
    {
        return left.severity == right.severity && left.code == right.code &&
               left.position.line == right.position.line &&
               left.position.column == right.position.column &&
               left.message == right.message;
    }

    const char *severityName(TOOL::Severity severity)
    {
        return severity == TOOL::Severity::Error ? "error" : "warning";
    }
}

void TOOL::Diagnostics::error(const std::string &code, SourcePosition position, std::string message)
{
    ++errors;
    if (storedErrors >= cap())
        return;
    ++storedErrors;
    records.push_back({Severity::Error, code, position, std::move(message)});
}

void TOOL::Diagnostics::warning(const std::string &code, SourcePosition position,
                                std::string message)
{
    ++warnings;
    if (storedWarnings >= cap())
        return;
    ++storedWarnings;
    records.push_back({Severity::Warning, code, position, std::move(message)});
}

void TOOL::Diagnostics::sortAndDedup(void)
{
    std::sort(records.begin(), records.end(), [](const Diagnostic &left, const Diagnostic &right) {
        return std::tie(left.position.line, left.position.column, left.code, left.message,
                        left.severity) <
               std::tie(right.position.line, right.position.column, right.code, right.message,
                        right.severity);
    });
    records.erase(std::unique(records.begin(), records.end(), sameDiagnostic), records.end());
}

std::string TOOL::Diagnostics::render(const std::string &tool, const std::string &path) const
{
    std::string text;
    for (const Diagnostic &item : records)
    {
        text += tool;
        text += ": ";
        if (item.position.line > 0)
        {
            text += path;
            text += ':';
            text += std::to_string(item.position.line);
            text += ':';
            text += std::to_string(item.position.column);
            text += ": ";
        }
        text += severityName(item.severity);
        text += ": ";
        text += item.code;
        text += ": ";
        text += item.message;
        text += '\n';
    }
    if (errors != 0 || warnings != 0)
    {
        text += tool;
        text += ": ";
        text += std::to_string(errors);
        text += " error(s), ";
        text += std::to_string(warnings);
        text += " warning(s)\n";
    }
    return text;
}

std::string TOOL::firstDifference(std::span<const byte> expected, std::span<const byte> actual)
{
    const std::size_t limit = std::min(expected.size(), actual.size());
    std::size_t offset = 0;
    while (offset < limit && expected[offset] == actual[offset])
        ++offset;
    std::ostringstream out;
    out << "first difference at byte " << TOOL::hexFixed(offset, 8);
    if (offset < expected.size())
        out << " expected " << TOOL::hexFixed(expected[offset], 2);
    else
        out << " expected <eof>";
    if (offset < actual.size())
        out << " got " << TOOL::hexFixed(actual[offset], 2);
    else
        out << " got <eof>";
    return out.str();
}

