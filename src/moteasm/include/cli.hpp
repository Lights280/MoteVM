#pragma once
#include <ostream>
#include <string>

namespace CLI
{
    struct Options
    {
        bool help = false;
        bool version = false;
        bool debug = false;    // -g / --debug (force DEBUG section)
        bool noDebug = false;  // --no-debug
        bool listing = false;
        std::string listingPath;
        int maxErrors = 20;
        bool warnAsError = false;
        bool checkRoundtrip = false;
        bool outputSet = false;
        std::string output;
        std::string input;
    };

    int parse(int argc, char *argv[], Options &options);
    void printUsage(std::ostream &out);
    void printVersion(std::ostream &out);
    int run(const Options &options, std::ostream &out, std::ostream &err);
}
