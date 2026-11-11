// MoteAR — pack, inspect, verify and unpack MTR 1.0 (.mtr) archives.
// The format logic lives in src/include/archive; this file only drives the CLI.

#include <iostream>

#include "common/cli.hpp"
#include "cli.hpp"

int main(int argc, char *argv[])
{
    return TOOL::runMain(argc, argv, std::cout, std::cerr, &CLI::parse,
                         &CLI::printUsage, &CLI::printVersion, &CLI::run);
}
