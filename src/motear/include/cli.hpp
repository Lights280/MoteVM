#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "define.hpp"

namespace CLI
{
    enum class Command
    {
        none,
        pack,
        list,
        info,
        verify,
        unpack
    };

    struct FileSpec
    {
        std::string path;
        std::string name; // empty: basename of path
    };

    struct Options
    {
        Command command = Command::none;
        bool help = false;
        bool version = false;

        std::string input;     // list/info/verify/unpack
        std::string output;    // pack
        std::string directory; // unpack -C

        bool verbose = false;
        bool verify = false;
        bool strict = false;
        bool noModules = false;
        bool json = false;
        bool force = false;

        std::vector<FileSpec> modules;
        std::vector<FileSpec> resources;

        std::vector<std::string> moduleFilter;
        std::vector<std::string> resourceFilter;

        std::string entry;
        std::string packageName;
        std::string packageVersion;
        std::string description;
        std::string author;
        std::string license;
        std::string buildId;
    };

    int parse(int argc, char *argv[], Options &options);
    void printUsage(std::ostream &out);
    void printVersion(std::ostream &out);

    // Returns 0 ok, 1 usage, 2 input/verify, 3 I/O, 4 internal.
    int run(const Options &options, std::ostream &out, std::ostream &err);
}
