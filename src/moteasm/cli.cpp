#include "cli.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "asm/assembler.hpp"
#include "define.hpp"
#include "dis/disassembler.hpp"
#include "common/args.hpp"
#include "common/cli.hpp"
#include "common/diagnostics.hpp"
#include "common/io.hpp"
#include "common/path.hpp"
#include "common/text.hpp"

namespace
{
    std::string defaultOutput(const std::string &input)
    {
        std::filesystem::path path(input);
        path.replace_extension(".mbc");
        return path.string();
    }

    std::string listingText(const ASM::AssembleResult &result)
    {
        std::ostringstream out;
        out << "; moteasm listing v1  module=\"" << TOOL::escapeBytes(result.moduleName)
            << "\"  mbc=1.0\n";
        out << "; func  idx  pc   word        source\n";
        for (const ASM::ListingEntry &entry : result.listing)
        {
            out << entry.function << "  " << entry.funcIndex << "  " << entry.pc << "  "
                << TOOL::hexFixed(entry.word, 8) << "  " << entry.source << "\n";
        }
        out << "; consts=" << result.constantCount << " funcs=" << result.functionCount
            << " globals=" << result.globalCount << " imports=" << result.importCount
            << " code_instrs=" << result.instructionCount << "\n";
        return out.str();
    }

}

int CLI::parse(int argc, char *argv[], Options &options)
{
    bool endOfOptions = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index] == nullptr ? std::string() : argv[index];
        if (!endOfOptions && TOOL::isEndOfOptions(argument))
        {
            endOfOptions = true;
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
        if (!endOfOptions && (argument == "-g" || argument == "--debug"))
        {
            options.debug = true;
            continue;
        }
        if (!endOfOptions && argument == "--no-debug")
        {
            options.noDebug = true;
            continue;
        }
        if (!endOfOptions && argument == "--warn-as-error")
        {
            options.warnAsError = true;
            continue;
        }
        if (!endOfOptions && argument == "--check-roundtrip")
        {
            options.checkRoundtrip = true;
            continue;
        }
        if (!endOfOptions && (argument == "-o" || TOOL::matchesOption(argument, "--output")))
        {
            const bool ok =
                argument == "-o"
                    ? TOOL::takeValue(argc, argv, index, options.output)
                    : TOOL::optionValue(argument, "--output", argc, argv, index,
                                        options.output);
            if (!ok)
                return 1;
            options.outputSet = true;
            continue;
        }
        if (!endOfOptions && TOOL::matchesOption(argument, "--listing"))
        {
            if (!TOOL::optionValue(argument, "--listing", argc, argv, index,
                                   options.listingPath))
                return 1;
            options.listing = true;
            continue;
        }
        if (!endOfOptions && TOOL::matchesOption(argument, "--max-errors"))
        {
            std::string value;
            if (!TOOL::optionValue(argument, "--max-errors", argc, argv, index, value))
                return 1;
            std::int64_t parsed = 0;
            if (!TOOL::parseInt64(value, parsed) || parsed < 0 || parsed > 100000)
                return 1;
            options.maxErrors = static_cast<int>(parsed);
            continue;
        }
        if (!endOfOptions && TOOL::isOptionToken(argument))
            return 1;
        if (options.input.empty())
            options.input = argument;
        else
            return 1;
    }

    if (options.help || options.version)
        return 0;
    if (options.input.empty())
        return 1;
    if (options.debug && options.noDebug)
        return 1;
    if (!options.outputSet && TOOL::isStdioToken(options.input))
        return 1;
    if (options.outputSet && !TOOL::isStdioToken(options.output) &&
        !TOOL::isStdioToken(options.input) &&
        TOOL::samePath(options.input, options.output))
        return 1;
    return 0;
}

void CLI::printVersion(std::ostream &out)
{
    TOOL::printVersionLine(out, "moteasm", "MOTEBC", MOTEBC_VERSION);
}

void CLI::printUsage(std::ostream &out)
{
    TOOL::printUsageHeader(out, "moteasm", "MOTEBC", MOTEBC_VERSION);
    out << "  moteasm [options] <input.mta>\n\n";
    out << "options:\n";
    out << "  -o, --output FILE   output .mbc (default: input with .mbc; '-' = stdout)\n";
    out << "  -g, --debug         force a DEBUG section\n";
    out << "      --no-debug      omit the DEBUG section\n";
    out << "      --listing FILE  write a listing\n";
    out << "      --max-errors N  stop storing after N errors (0 = unlimited, default 20)\n";
    out << "      --warn-as-error warnings are failures\n";
    out << "      --check-roundtrip  re-assemble the canonical text and compare bytes\n";
    out << "      --version | --help\n\n";
    out << "exit codes:\n";
    out << "  0 success   1 usage   2 input/source error   4 internal or I/O error\n";
}


int CLI::run(const Options &options, std::ostream &out, std::ostream &err)
{
    TOOL::setBinaryStdio();
    (void)out;

    const std::string display = options.input == "-" ? "<stdin>" : options.input;
    std::vector<byte> source;
    std::string error;
    const bool read = options.input == "-" ? TOOL::readStdin(source, error)
                                           : TOOL::readFile(options.input, source, error);
    if (!read)
    {
        err << "moteasm: " << display << ": error: " << error << "\n";
        return TOOL::INPUT;
    }

    TOOL::Diagnostics diagnostics;
    diagnostics.setLimit(options.maxErrors);
    diagnostics.setWarnAsError(options.warnAsError);

    ASM::Options assembly;
    assembly.noDebug = options.noDebug;
    assembly.forceDebug = options.debug;
    ASM::AssembleResult result = ASM::assemble(source, assembly, diagnostics);
    diagnostics.sortAndDedup();
    if (!diagnostics.empty())
        err << diagnostics.render("moteasm", display);
    if (result.status != TOOL::OK || diagnostics.failed())
        return result.status == TOOL::OK ? TOOL::INPUT : result.status;

    if (options.checkRoundtrip)
    {
        const DISASM::TextResult text =
            DISASM::renderCanonical(result.image, !options.noDebug, false);
        if (text.status != TOOL::OK)
        {
            err << "moteasm: internal error: cannot disassemble the assembled module: "
                << text.error << "\n";
            return TOOL::INTERNAL;
        }
        const std::vector<byte> secondSource(text.text.begin(), text.text.end());
        TOOL::Diagnostics secondDiagnostics;
        secondDiagnostics.setLimit(options.maxErrors);
        ASM::Options secondOptions;
        secondOptions.noDebug = options.noDebug;
        secondOptions.forceDebug = options.debug;
        const ASM::AssembleResult second =
            ASM::assemble(secondSource, secondOptions, secondDiagnostics);
        secondDiagnostics.sortAndDedup();
        if (second.status != TOOL::OK || secondDiagnostics.failed())
        {
            if (!secondDiagnostics.empty())
                err << secondDiagnostics.render("moteasm", "<canonical>");
            err << "moteasm: error: round-trip re-assembly failed\n";
            return TOOL::INTERNAL;
        }
        if (second.image != result.image)
        {
            err << "moteasm: error: round-trip byte comparison failed: "
                << TOOL::firstDifference(result.image, second.image) << "\n";
            return TOOL::INTERNAL;
        }
    }

    const std::string outputPath =
        options.outputSet ? options.output : defaultOutput(options.input);
    if (outputPath == "-")
    {
        if (!TOOL::writeStdout(result.image, error))
        {
            err << "moteasm: error: " << error << "\n";
            return TOOL::INTERNAL;
        }
    }
    else if (!TOOL::writeFileAtomic(outputPath, result.image, error))
    {
        err << "moteasm: error: " << error << "\n";
        return TOOL::INTERNAL;
    }

    if (options.listing)
    {
        const std::string text = listingText(result);
        const std::span<const byte> bytes(reinterpret_cast<const byte *>(text.data()),
                                          text.size());
        if (!TOOL::writeFileAtomic(options.listingPath, bytes, error))
        {
            err << "moteasm: error: " << error << "\n";
            return TOOL::INTERNAL;
        }
    }
    return TOOL::OK;
}

