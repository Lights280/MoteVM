// POSIX implementation header. Included only by builder.cpp after the common
// helpers it uses; it is not a standalone translation unit.
#pragma once

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "platform.hpp"

namespace BUILD
{
    namespace platform
    {
        inline std::string pathText(const Path &path)
        {
            return path.string();
        }

        inline Path pathFromText(const std::string &text)
        {
            return Path(text);
        }

        inline ProcessResult spawnProcess(const std::vector<std::string> &args,
                                         const Path &logPath, const Path &cwd)
        {
            ProcessResult result;
            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (const std::string &argument : args)
                argv.push_back(const_cast<char *>(argument.c_str()));
            argv.push_back(nullptr);
            const pid_t pid = fork();
            if (pid < 0)
            {
                result.spawnError = "fork failed";
                return result;
            }
            if (pid == 0)
            {
                const int fd = open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (fd < 0)
                    _exit(127);
                if (dup2(fd, STDOUT_FILENO) < 0)
                    _exit(127);
                if (dup2(fd, STDERR_FILENO) < 0)
                    _exit(127);
                close(fd);
                if (!cwd.empty() && chdir(cwd.c_str()) != 0)
                    _exit(126);
                execvp(argv[0], argv.data());
                _exit(127);
            }
            int status = 0;
            while (waitpid(pid, &status, 0) < 0)
            {
                if (errno != EINTR)
                {
                    result.spawnError = "waitpid failed";
                    return result;
                }
            }
            result.started = true;
            if (WIFEXITED(status))
                result.exitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                result.exitCode = 128 + WTERMSIG(status);
            else
                result.exitCode = 1;
            return result;
        }

        inline ProcessResult runProcessInherited(const std::vector<std::string> &args,
                                                  const Path &cwd)
        {
            ProcessResult result;
            if (args.empty())
            {
                result.spawnError = "empty command";
                return result;
            }
            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (const std::string &argument : args)
                argv.push_back(const_cast<char *>(argument.c_str()));
            argv.push_back(nullptr);
            const pid_t pid = fork();
            if (pid < 0)
            {
                result.spawnError = "fork failed";
                return result;
            }
            if (pid == 0)
            {
                if (!cwd.empty() && chdir(cwd.c_str()) != 0)
                    _exit(126);
                execvp(argv[0], argv.data());
                _exit(127);
            }
            int status = 0;
            while (waitpid(pid, &status, 0) < 0)
            {
                if (errno != EINTR)
                {
                    result.spawnError = "waitpid failed";
                    return result;
                }
            }
            result.started = true;
            if (WIFEXITED(status))
                result.exitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                result.exitCode = 128 + WTERMSIG(status);
            else
                result.exitCode = 1;
            return result;
        }

        inline std::optional<Path> nativeExecutablePath()
        {
            std::error_code linkError;
            const Path link = std::filesystem::read_symlink("/proc/self/exe", linkError);
            if (!linkError && !link.empty())
                return link;
            return std::nullopt;
        }

        inline std::string formatCommand(const std::vector<std::string> &args)
        {
            std::string out;
            for (const std::string &argument : args)
            {
                if (!out.empty())
                    out.push_back(' ');
                bool needsQuotes = argument.empty();
                for (char c : argument)
                {
                    if (c == ' ' || c == '\t' || c == '\'' || c == '"' || c == '\\')
                    {
                        needsQuotes = true;
                        break;
                    }
                }
                if (!needsQuotes)
                {
                    out += argument;
                    continue;
                }
                out.push_back('\'');
                for (char c : argument)
                {
                    if (c == '\'')
                        out += "'\\''";
                    else
                        out.push_back(c);
                }
                out.push_back('\'');
            }
            return out;
        }

        inline bool pathTextEquals(const std::string &left, const std::string &right)
        {
            return left == right;
        }

        inline std::string executableSuffix()
        {
            return std::string();
        }

        inline char pathListSeparator()
        {
            return ':';
        }

        inline std::vector<std::string> executableNames(const std::string &name,
                                                         bool hasExtension)
        {
            (void)hasExtension;
            std::vector<std::string> names;
            names.push_back(name);
            return names;
        }

        inline void setTempEnvironment(const Path &tempDir)
        {
            const std::string text = pathText(tempDir);
            setenv("TMPDIR", text.c_str(), 1);
            setenv("TMP", text.c_str(), 1);
            setenv("TEMP", text.c_str(), 1);
        }

        inline bool reexecInDevShell(const Options &options, int argc, char *argv[], Arch arch,
                                     const Path &logsDir, int &exitCode, std::string &error)
        {
            (void)options;
            (void)argc;
            (void)argv;
            (void)arch;
            (void)logsDir;
            exitCode = 0;
            error = "MSVC is only supported on Windows";
            return false;
        }
    }
}
