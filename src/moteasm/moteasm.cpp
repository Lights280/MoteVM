// MoteASM — .mta source to MOTEBC 1.0 (.mbc).
// The command line is a thin shell; the assembler lives in src/include/asm.

#include <iostream>

#include "cli.hpp"

int main(int argc, char *argv[])
{
    CLI::Options options;
    if (CLI::parse(argc, argv, options) != 0)
    {
        CLI::printUsage(std::cerr);
        return 1;
    }
    if (options.help)
    {
        CLI::printUsage(std::cout);
        return 0;
    }
    if (options.version)
    {
        CLI::printVersion(std::cout);
        return 0;
    }
    return CLI::run(options, std::cout, std::cerr);
}
