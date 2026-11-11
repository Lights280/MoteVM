// MoteDIS — MOTEBC 1.0 (.mbc) to canonical .mta text.
// The renderer lives in src/include/dis; this file only handles the CLI.

#include <iostream>

#include "common/cli.hpp"
#include "cli.hpp"

int main(int argc, char *argv[])
{
    return TOOL::runMain(argc, argv, std::cout, std::cerr, &CLI::parse,
                         &CLI::printUsage, &CLI::printVersion, &CLI::run);
}
