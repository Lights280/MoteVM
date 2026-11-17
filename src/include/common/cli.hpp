#pragma once
// Small, type-agnostic command-line helpers shared by the tool executables.
//
// This header intentionally does NOT define options, flags, exit codes or
// help text. Each module keeps its own namespace CLI / Options / parse / run;
// only the mechanical argv handling and version/usage headers live here.
// Keep it header-only: motevm links only motevm_core and builder must remain
// buildable as a single translation unit.

#include <algorithm>
#include <cstddef>
#include <deque>
#include <exception>
#include <initializer_list>
#include <new>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

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

    // Walks argv and expands clustered short options ("-dsi" => -d -s -i).
    //
    // shortOptions lists every accepted short-option letter; valueOptions lists
    // the subset that takes a value. A value-taking option consumes the rest of
    // its token ("-wDIR", "-w=DIR") or the next raw argv token ("-w DIR").
    // Tokens that are not a cluster of known short options are returned
    // verbatim, so "unknown option '-debug'" can still suggest "--debug".
    // Long options, "-", "--" and tokens after disableExpansion() pass through
    // unchanged; takeValue() never expands a token.
    class ArgReader
    {
    public:
        ArgReader(int argc, char *argv[], std::string_view shortOptions,
                  std::string_view valueOptions, int start = 1)
            : argc_(argc), argv_(argv), shortOptions_(shortOptions),
              valueOptions_(valueOptions), index_(start)
        {
        }

        bool next(std::string &argument)
        {
            if (!pending_.empty())
            {
                argument = std::move(pending_.front());
                pending_.pop_front();
                return true;
            }
            if (index_ >= argc_)
                return false;
            hasAttached_ = false;
            attached_.clear();
            const char *raw = argv_[index_++];
            argument = raw == nullptr ? std::string() : std::string(raw);
            if (expand_ && isCluster(argument) && knownCluster(argument))
            {
                split(argument);
                argument = std::move(pending_.front());
                pending_.pop_front();
            }
            return true;
        }

        bool takeValue(std::string &value)
        {
            if (hasAttached_)
            {
                value = std::move(attached_);
                hasAttached_ = false;
                attached_.clear();
                return !value.empty();
            }
            if (index_ >= argc_ || argv_[index_] == nullptr)
                return false;
            value = argv_[index_++];
            return !value.empty();
        }

        void disableExpansion() { expand_ = false; }

    private:
        static bool isCluster(std::string_view argument)
        {
            return argument.size() > 2 && argument[0] == '-' && argument[1] != '-';
        }

        bool knownCluster(std::string_view argument) const
        {
            for (std::size_t i = 1; i < argument.size(); ++i)
            {
                if (shortOptions_.find(argument[i]) == std::string_view::npos)
                    return false;
                if (valueOptions_.find(argument[i]) != std::string_view::npos)
                    return true; // the remaining characters are its value
            }
            return true;
        }

        void split(const std::string &argument)
        {
            for (std::size_t i = 1; i < argument.size(); ++i)
            {
                std::string option(1, '-');
                option.push_back(argument[i]);
                if (valueOptions_.find(argument[i]) == std::string_view::npos)
                {
                    pending_.push_back(std::move(option));
                    continue;
                }
                pending_.push_back(std::move(option));
                const std::string remainder = argument.substr(i + 1);
                if (!remainder.empty())
                {
                    // "-o=FILE" and "-oFILE" attach the value; "-o=" is an
                    // explicit empty value and must not consume the next token.
                    hasAttached_ = true;
                    attached_ = remainder.front() == '=' ? remainder.substr(1) : remainder;
                }
                return;
            }
        }

        int argc_;
        char **argv_;
        // Owned copies: callers must not have to keep the option tables alive.
        std::string shortOptions_;
        std::string valueOptions_;
        int index_;
        bool expand_ = true;
        bool hasAttached_ = false;
        std::string attached_;
        std::deque<std::string> pending_;
    };

    // Exact "name" consumes the next token via takeValue(); "name=value" takes
    // the suffix. Missing/empty (or non-matching) returns false. Callers must
    // guard with matchesOption() before treating false as a malformed value.
    inline bool optionValue(std::string_view argument, std::string_view name,
                            ArgReader &args, std::string &value)
    {
        if (argument == name)
            return args.takeValue(value);
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

    inline std::size_t editDistance(std::string_view left, std::string_view right)
    {
        std::vector<std::size_t> previous(right.size() + 1);
        std::vector<std::size_t> current(right.size() + 1);
        for (std::size_t j = 0; j <= right.size(); ++j)
            previous[j] = j;
        for (std::size_t i = 1; i <= left.size(); ++i)
        {
            current[0] = i;
            for (std::size_t j = 1; j <= right.size(); ++j)
            {
                const std::size_t substitution =
                    previous[j - 1] + (left[i - 1] == right[j - 1] ? 0 : 1);
                current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
            }
            previous.swap(current);
        }
        return previous[right.size()];
    }

    // Closest known name within two edits, as a " (did you mean 'X'?)" suffix.
    inline std::string closestName(std::string_view value,
                                   std::initializer_list<std::string_view> known)
    {
        if (value.size() < 3) // too ambiguous
            return std::string();
        std::string best;
        std::size_t bestDistance = 3;
        for (std::string_view candidate : known)
        {
            const std::size_t distance = editDistance(value, candidate);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best.assign(candidate.data(), candidate.size());
            }
        }
        if (best.empty())
            return std::string();
        return " (did you mean '" + best + "'?)";
    }

    // Option-specific hint: long options use edit distance; a single-dash long
    // spelling ("-debug") is matched against "--debug". Short typos are not
    // suggested because a one-letter edit is not meaningful.
    inline std::string optionSuggestion(std::string_view value,
                                        std::initializer_list<std::string_view> known)
    {
        const std::string_view probe = value.substr(0, value.find('='));
        const bool longForm = probe.size() > 1 && probe[1] == '-';
        if (!longForm)
        {
            if (probe.size() <= 2)
                return std::string();
            const std::string withDash = "-" + std::string(probe);
            for (std::string_view candidate : known)
                if (candidate == withDash)
                    return " (did you mean '" + withDash + "'?)";
            return std::string();
        }

        std::string best;
        std::size_t bestDistance = 3;
        for (std::string_view candidate : known)
        {
            if (candidate.size() < 2 || candidate[1] != '-')
                continue;
            const std::size_t distance = editDistance(probe, candidate);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best.assign(candidate.data(), candidate.size());
            }
        }
        if (best.empty())
            return std::string();
        return " (did you mean '" + best + "'?)";
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
                int (*parse)(int, char **, Options &, std::string &),
                void (*usage)(std::ostream &),
                void (*version)(std::ostream &),
                int (*run)(const Options &, std::ostream &, std::ostream &))
    {
        Options options;
        std::string error;
        if (parse(argc, argv, options, error) != 0)
        {
            if (!error.empty())
                err << "error: " << error << "\n\n";
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
        catch (const std::exception &failure)
        {
            err << "error: " << failure.what() << "\n";
            return 4;
        }
        catch (...)
        {
            err << "error: unexpected internal failure\n";
            return 4;
        }
    }
}
