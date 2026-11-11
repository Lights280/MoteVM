// MoteASM — .mta source to MOTEBC 1.0 (.mbc).
// The command line is a thin shell; the assembler lives in src/include/asm.

#include <iostream>

#include "common/cli.hpp"
#include "cli.hpp"

int main(int argc, char *argv[])
{
    return TOOL::runMain(argc, argv, std::cout, std::cerr, &CLI::parse,
                         &CLI::printUsage, &CLI::printVersion, &CLI::run);
}
