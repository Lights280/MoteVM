// MoteASM — .mta source to MOTEBC 1.0 (.mbc).
// The command line is a thin shell; the assembler lives in src/include/asm.

#include <iostream>

#include "common/argv.hpp"
#include "common/cli.hpp"
#include "cli.hpp"

namespace
{
    int runTool(int argc, char *argv[])
    {
        return TOOL::runMain(argc, argv, std::cout, std::cerr, &CLI::parse,
                             &CLI::printUsage, &CLI::printVersion, &CLI::run);
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t *argv[])
{
    TOOL::WideArgv arguments(argc, argv);
    return runTool(arguments.argc(), arguments.argv());
}
#else
int main(int argc, char *argv[])
{
    return runTool(argc, argv);
}
#endif
