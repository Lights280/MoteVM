#pragma once

#include <optional>
#include <string>
#include <vector>

#include "types.hpp"

namespace BUILD
{
    namespace platform
    {
        // Path encoding and process primitives. Each platform header provides these
        // definitions and is included exactly once by builder.cpp.
        inline std::string pathText(const Path &path);
        inline Path pathFromText(const std::string &text);

        inline ProcessResult spawnProcess(const std::vector<std::string> &args,
                                          const Path &logPath, const Path &cwd);
        inline ProcessResult runProcessInherited(const std::vector<std::string> &args,
                                                 const Path &cwd);
        inline std::optional<Path> nativeExecutablePath();
        inline std::string formatCommand(const std::vector<std::string> &args);

        inline bool pathTextEquals(const std::string &left, const std::string &right);
        inline std::string executableSuffix();
        inline char pathListSeparator();
        inline std::vector<std::string> executableNames(const std::string &name,
                                                        bool hasExtension);
        inline void setTempEnvironment(const Path &tempDir);

        inline bool reexecInDevShell(const Options &options, int argc, char *argv[], Arch arch,
                                     const Path &logsDir, int &exitCode, std::string &error);
    }
}
