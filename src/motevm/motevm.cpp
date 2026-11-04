// MoteVM.cpp : the entry point for MoteVM application.

#include <iostream>

#include "cli.hpp"

int main(int argc, char *argv[])
{
    CLI::Options options;
    if (CLI::parse(argc, argv, options) != CLI::OK)
    {
        CLI::printUsage(std::cerr);
        return CLI::USAGE;
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