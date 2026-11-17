#include "cli.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "asm/assembler.hpp"
#include "define.hpp"
#include "dis/disassembler.hpp"
#include "common/cli.hpp"
#include "common/diagnostics.hpp"
#include "common/io.hpp"
#include "common/path.hpp"
#include "common/text.hpp"
#include "parse.hpp"

int CLI::parse(int argc, char *argv[], Options &options, std::string &error)
{
    TOOL::ArgReader args(argc, argv, "hvo", "o");
    bool endOfOptions = false;
    std::string argument;
    while (args.next(argument))
    {
        if (!endOfOptions && TOOL::isEndOfOptions(argument))
        {
            endOfOptions = true;
            args.disableExpansion();
            continue;
        }
        if (!endOfOptions && (argument == "-h" || argument == "--help"))
        {
            options.help = true;
            return 0;
        }
        if (!endOfOptions && (argument == "-v" || argument == "--version"))
        {
            options.version = true;
            return 0;
        }
        if (!endOfOptions && argument == "--no-debug")
        {
            options.noDebug = true;
            continue;
        }
        if (!endOfOptions && argument == "--annotate")
        {
            options.annotate = true;
            continue;
        }
        if (!endOfOptions && argument == "--check-roundtrip")
        {
            options.checkRoundtrip = true;
            continue;
        }
        if (!endOfOptions && (argument == "-o" || TOOL::matchesOption(argument, "--output")))
        {
            const std::string_view name = argument == "-o" ? "-o" : "--output";
            if (!TOOL::optionValue(argument, name, args, options.output))
            {
                error = "option '" + std::string(name) +
                        "' needs a value (use '" + std::string(name) +
                        " FILE' or '" + std::string(name) + "=FILE')";
                return 1;
            }
            options.outputSet = true;
            continue;
        }
        if (!endOfOptions && TOOL::isOptionToken(argument))
        {
            error = "unknown option '" + argument + "'" +
                    TOOL::optionSuggestion(argument,
                                           {"-h", "--help", "-v", "--version", "-o",
                                            "--output", "--no-debug", "--annotate",
                                            "--check-roundtrip"});
            return 1;
        }
        if (options.input.empty())
            options.input = argument;
        else
        {
            error = "unexpected argument '" + argument +
                    "' (only one input file is accepted)";
            return 1;
        }
    }

    if (options.help || options.version)
        return 0;
    if (options.input.empty())
    {
        error = "missing input file (usage: motedis [options] <input.mbc>)";
        return 1;
    }
    // RT-1 compares the whole module; --annotate and --no-debug both change
    // the text that is re-assembled, so neither can be combined with it.
    if (options.annotate && options.checkRoundtrip)
    {
        error = "--annotate cannot be combined with --check-roundtrip";
        return 1;
    }
    if (options.noDebug && options.checkRoundtrip)
    {
        error = "--no-debug cannot be combined with --check-roundtrip";
        return 1;
    }
    if (options.outputSet && !TOOL::isStdioToken(options.output) &&
        !TOOL::isStdioToken(options.input) &&
        TOOL::samePath(options.input, options.output))
    {
        error = "input and output are the same file '" + options.input + "'";
        return 1;
    }
    return 0;
}

void CLI::printVersion(std::ostream &out)
{
    TOOL::printVersionLine(out, "motedis", "MOTEBC", MOTEBC_VERSION);
}

void CLI::printUsage(std::ostream &out)
{
    TOOL::printUsageHeader(out, "motedis", "MOTEBC", MOTEBC_VERSION);
    out << "  motedis [options] <input.mbc>\n\n";
    out << "options:\n";
    out << "  -o, --output FILE  canonical text output (default: stdout; '-' = stdout)\n";
    out << "      --no-debug     omit .loc/.local\n";
    out << "      --annotate     append '; pc=...' to instructions (non-canonical)\n";
    out << "      --check-roundtrip  re-assemble and compare bytes\n";
    out << "      --version | --help\n\n";
    out << "exit codes:\n";
    out << "  0 success   1 usage   2 input/verify error   4 internal or I/O error\n";
}

int CLI::run(const Options &options, std::ostream &out, std::ostream &err)
{
    if (!TOOL::setBinaryStdio())
    {
        err << "motedis: error: cannot switch stdio to binary mode\n";
        return TOOL::INTERNAL;
    }
    (void)out;

    const std::string display = options.input == "-" ? "<stdin>" : options.input;
    std::vector<byte> module;
    std::string error;
    const bool read =
        options.input == "-"
            ? TOOL::readStdin(module, error, PARSE::MAX_MODULE_BYTES)
            : TOOL::readFile(options.input, module, error, PARSE::MAX_MODULE_BYTES);
    if (!read)
    {
        err << "motedis: " << display << ": error: " << error << "\n";
        return TOOL::INPUT;
    }

    const DISASM::TextResult text =
        DISASM::renderCanonical(module, !options.noDebug, options.annotate);
    if (text.status != TOOL::OK)
    {
        err << "motedis: " << display << ": error: " << text.error << "\n";
        return text.status;
    }
    for (const std::string &warning : text.warnings)
        err << "motedis: warning: " << warning << "\n";

    if (options.checkRoundtrip)
    {
        const std::vector<byte> source(text.text.begin(), text.text.end());
        TOOL::Diagnostics diagnostics;
        ASM::Options assembly;
        assembly.noDebug = options.noDebug;
        // A module may carry an empty DEBUG section (moteasm -g with no
        // .loc/.local). canonical text cannot express the empty section, so
        // force the header back when the input flag is set.
        if (!options.noDebug && module.size() >= 12 && (module[8] & 0x01u) != 0 &&
            text.text.find(".loc ") == std::string::npos &&
            text.text.find(".local ") == std::string::npos)
            assembly.forceDebug = true;
        const ASM::AssembleResult second = ASM::assemble(source, assembly, diagnostics);
        diagnostics.sortAndDedup();
        if (second.status != TOOL::OK || diagnostics.failed())
        {
            if (!diagnostics.empty())
                err << diagnostics.render("motedis", "<canonical>");
            err << "motedis: error: round-trip re-assembly failed\n";
            return TOOL::INTERNAL;
        }
        if (second.image != module)
        {
            err << "motedis: error: round-trip byte comparison failed: "
                << TOOL::firstDifference(module, second.image) << "\n";
            return TOOL::INTERNAL;
        }
    }

    const std::span<const byte> bytes(reinterpret_cast<const byte *>(text.text.data()),
                                      text.text.size());
    if (!options.outputSet || options.output == "-")
    {
        if (!TOOL::writeStdout(bytes, error))
        {
            err << "motedis: error: " << error << "\n";
            return TOOL::INTERNAL;
        }
    }
    else if (!TOOL::writeFileAtomic(options.output, bytes, error))
    {
        err << "motedis: error: " << error << "\n";
        return TOOL::INTERNAL;
    }
    return TOOL::OK;
}

