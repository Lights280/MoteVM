// MoteVM.cpp : the entry point for MoteVM application.

#include <iostream>

#include "common/cli.hpp"
#include "cli.hpp"

int main(int argc, char *argv[])
{
    return TOOL::runMain(argc, argv, std::cout, std::cerr, &CLI::parse,
                         &CLI::printUsage, &CLI::printVersion, &CLI::run);
}
