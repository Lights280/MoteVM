#pragma once
// Small, type-agnostic command-line helpers shared by the tool executables.
//
// This header intentionally does NOT define options, flags, exit codes or
// help text. Each module keeps its own namespace CLI / Options / parse / run;
// only the mechanical argv handling and version/usage headers live here.
// Keep it header-only: motevm links only motevm_core and builder must remain
// buildable as a single translation unit.

#include <exception>
#include <new>
#include <ostream>
#include <string>
#include <string_view>

#include "args.hpp"
#include "../define.hpp"

namespace TOOL
{
    // Token starts an option (but a bare "-" is the stdio marker, not an option).
    inline bool isOptionToken(std::string_view argument)
    {
        return !argument.empty() && argument.front() == '-' && argument != "-";
    }

    inline bool isEndOfOptions(std::string_view argument)
    {
        return argument == "--";
    }

    inline bool isStdioToken(std::string_view argument)
    {
        return argument == "-";
    }

    // Exact "name" or "name=value" (no short-option attached-value magic).
    inline bool matchesOption(std::string_view argument, std::string_view name)
    {
        if (argument == name)
            return true;
        return argument.size() > name.size() &&
               argument.compare(0, name.size(), name) == 0 &&
               argument[name.size()] == '=';
    }

    // Exact "name" consumes the next argv token via takeValue(); "name=value"
    // takes the suffix. Missing/empty (or non-matching) returns false.
    // Callers must guard with matchesOption() before treating false as a
    // malformed value; this mirrors the old per-module isOption()+optionValue().
    inline bool optionValue(std::string_view argument, std::string_view name,
                            int argc, char *argv[], int &index, std::string &value)
    {
        if (argument == name)
            return takeValue(argc, argv, index, value);
        if (!matchesOption(argument, name))
            return false;

        const std::string_view suffix = argument.substr(name.size() + 1);
        if (suffix.empty())
        {
            value.clear();
            return false;
        }
        value.assign(suffix.data(), suffix.size());
        return true;
    }

    // "<tool> <MOTEVM_VERSION> (<format> <formatVersion>)\n".
    inline void printVersionLine(std::ostream &out, std::string_view tool,
                                 std::string_view format, std::string_view formatVersion)
    {
        out << tool << ' ' << MOTEVM_VERSION << " (" << format << ' '
            << formatVersion << ")\n";
    }

    // Version line followed by a blank line and "usage:\n".
    inline void printUsageHeader(std::ostream &out, std::string_view tool,
                                 std::string_view format, std::string_view formatVersion)
    {
        printVersionLine(out, tool, format, formatVersion);
        out << "\nusage:\n";
    }

    // Shared main() shell for tools whose Options have help/version fields and
    // whose parse/usage/version/run signatures match the four non-builder
    // modules. Function pointers avoid two-phase lookup of a module-local
    // namespace CLI from this header.
    template <typename Options>
    int runMain(int argc, char *argv[], std::ostream &out, std::ostream &err,
                int (*parse)(int, char **, Options &),
                void (*usage)(std::ostream &),
                void (*version)(std::ostream &),
                int (*run)(const Options &, std::ostream &, std::ostream &))
    {
        Options options;
        if (parse(argc, argv, options) != 0)
        {
            usage(err);
            return 1;
        }
        if (options.help)
        {
            usage(out);
            return 0;
        }
        if (options.version)
        {
            version(out);
            return 0;
        }
        try
        {
            return run(options, out, err);
        }
        catch (const std::bad_alloc &)
        {
            err << "error: out of memory\n";
            return 4;
        }
        catch (const std::exception &error)
        {
            err << "error: " << error.what() << "\n";
            return 4;
        }
        catch (...)
        {
            err << "error: unexpected internal failure\n";
            return 4;
        }
    }
}
