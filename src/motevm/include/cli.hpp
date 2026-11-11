#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "define.hpp"

namespace CLI
{
    const int OK = 0;    // arguments parsed
    const int USAGE = 1; // usage error

    struct Options
    {
        bool version = false;             // -v / --version
        bool help = false;                // -h / --help
        bool debug = false;               // -d / --debug  (default: release)
        bool status = false;              // -s / --status
        bool interactive = false;         // -i / --interactive (stop at entry, enter REPL)
        bool step = false;                // --step (stop before the first instruction)
        int trace = 0;                    // --trace: 0 off, 1 call, 2 exec, 3 full
        std::string traceFile;            // --trace-file (empty: stderr, "-": stdout)
        dword traceLimit = 0;             // --trace-limit (0: unlimited)
        std::vector<std::string> breakpoints; // --break, repeatable
        std::string workspace;            // -w / --workspace DIR (empty: stdio only)
        std::string entry;                // entry module (first non-option token)
        std::vector<std::string> modules; // external modules (tokens ending with .mbc)
        std::vector<std::string> intern;  // intern parameters (reserved, currently ignored)

        std::vector<std::string> files(void) const; // entry + modules, in load order
        bool tracing(void) const { return trace != 0; }
        bool debugging(void) const { return interactive || step || !breakpoints.empty() || trace != 0; }
    };

    // motevm [extern_parameters] <entry_mbc_module> [ext_mbc_modules] [intern_parameters]
    int parse(int argc, char *argv[], Options &options);
    void printUsage(std::ostream &out);
    void printVersion(std::ostream &out);

    // Returns a process exit code: 0 ok / 1 usage / 2 input failure / 3 trap / 4 internal.
    int run(const Options &options, std::ostream &out, std::ostream &err);
}