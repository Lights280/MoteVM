// POSIX implementation header. Included only by builder.cpp after the common
// helpers it uses; it is not a standalone translation unit.
#pragma once

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "platform.hpp"

namespace BUILD
{
    namespace platform
    {
        inline std::string processErrorText(const char *what, int code)
        {
            return std::string(what) + ": " +
                   std::error_code(code, std::generic_category()).message();
        }

        // Move a descriptor above stderr so dup2() in the child cannot clobber it.
        inline bool raiseFd(int &fd)
        {
            if (fd > STDERR_FILENO)
                return true;
            const int high = ::fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
            if (high < 0)
                return false;
            ::close(fd);
            fd = high;
            return true;
        }

        // Reports a pre-exec failure to the parent through a CLOEXEC pipe: the
        // child writes errno, the parent turns it into spawnError. Without this,
        // an exec failure is indistinguishable from a compiler exit code.
        inline bool makeErrorPipe(int (&pipeFds)[2], std::string &error)
        {
            if (::pipe(pipeFds) != 0)
            {
                error = processErrorText("pipe failed", errno);
                return false;
            }
            // Only the write end needs to survive the child's dup2() calls; the
            // child closes the read end before redirecting stdio.
            if (!raiseFd(pipeFds[1]))
            {
                const int code = errno;
                ::close(pipeFds[0]);
                ::close(pipeFds[1]);
                error = processErrorText("cannot prepare the error pipe", code);
                return false;
            }
            const int flags = ::fcntl(pipeFds[1], F_GETFD);
            if (flags < 0 || ::fcntl(pipeFds[1], F_SETFD, flags | FD_CLOEXEC) != 0)
            {
                const int code = errno;
                ::close(pipeFds[0]);
                ::close(pipeFds[1]);
                error = processErrorText("cannot set close-on-exec", code);
                return false;
            }
            return true;
        }

        inline int readChildError(int fd)
        {
            int childError = 0;
            std::size_t received = 0;
            while (received < sizeof childError)
            {
                const ssize_t count =
                    ::read(fd, reinterpret_cast<char *>(&childError) + received,
                           sizeof childError - received);
                if (count > 0)
                {
                    received += static_cast<std::size_t>(count);
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                break;
            }
            return received == sizeof childError ? childError : 0;
        }

        inline bool waitForChild(pid_t pid, int &status, std::string &error)
        {
            pid_t waited = -1;
            do
            {
                waited = ::waitpid(pid, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited >= 0)
                return true;

            const int code = errno;
            (void)::kill(pid, SIGKILL);
            (void)::waitpid(pid, &status, 0);
            error = processErrorText("waitpid failed", code);
            return false;
        }

        inline ProcessResult finishChild(pid_t pid, int (&errorPipe)[2],
                                         const std::string &executable,
                                         ProcessResult result)
        {
            ::close(errorPipe[1]);
            const int childError = readChildError(errorPipe[0]);
            ::close(errorPipe[0]);

            int status = 0;
            if (!waitForChild(pid, status, result.spawnError))
                return result;
            if (childError != 0)
            {
                result.spawnError =
                    "cannot execute '" + executable + "': " +
                    std::error_code(childError, std::generic_category()).message();
                return result;
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
            if (args.empty())
            {
                result.spawnError = "empty command";
                return result;
            }

            int errorPipe[2] = {-1, -1};
            if (!makeErrorPipe(errorPipe, result.spawnError))
                return result;

            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (const std::string &argument : args)
                argv.push_back(const_cast<char *>(argument.c_str()));
            argv.push_back(nullptr);

            const pid_t pid = ::fork();
            if (pid < 0)
            {
                const int code = errno;
                ::close(errorPipe[0]);
                ::close(errorPipe[1]);
                result.spawnError = processErrorText("fork failed", code);
                return result;
            }
            if (pid == 0)
            {
                ::close(errorPipe[0]);
                const auto fail = [&](int code)
                {
                    if (::write(errorPipe[1], &code, sizeof code) < 0)
                    {
                        // Best effort: the parent falls back to the exit status.
                    }
                    ::_exit(127);
                };

                int log = ::open(logPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (log < 0)
                    fail(errno);
                if (log <= STDERR_FILENO)
                {
                    const int high = ::fcntl(log, F_DUPFD, STDERR_FILENO + 1);
                    if (high < 0)
                        fail(errno);
                    ::close(log);
                    log = high;
                }
                if (::dup2(log, STDOUT_FILENO) < 0)
                    fail(errno);
                if (::dup2(log, STDERR_FILENO) < 0)
                    fail(errno);
                ::close(log);

                const int nullFd = ::open("/dev/null", O_RDONLY);
                if (nullFd < 0)
                    fail(errno);
                if (::dup2(nullFd, STDIN_FILENO) < 0)
                    fail(errno);
                if (nullFd != STDIN_FILENO)
                    ::close(nullFd);

                if (!cwd.empty() && ::chdir(cwd.c_str()) != 0)
                    fail(errno);
                ::execv(argv[0], argv.data());
                fail(errno);
            }

            return finishChild(pid, errorPipe, args[0], result);
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

            int errorPipe[2] = {-1, -1};
            if (!makeErrorPipe(errorPipe, result.spawnError))
                return result;

            std::vector<char *> argv;
            argv.reserve(args.size() + 1);
            for (const std::string &argument : args)
                argv.push_back(const_cast<char *>(argument.c_str()));
            argv.push_back(nullptr);

            const pid_t pid = ::fork();
            if (pid < 0)
            {
                const int code = errno;
                ::close(errorPipe[0]);
                ::close(errorPipe[1]);
                result.spawnError = processErrorText("fork failed", code);
                return result;
            }
            if (pid == 0)
            {
                ::close(errorPipe[0]);
                const auto fail = [&](int code)
                {
                    if (::write(errorPipe[1], &code, sizeof code) < 0)
                    {
                        // Best effort: the parent falls back to the exit status.
                    }
                    ::_exit(127);
                };
                if (!cwd.empty() && ::chdir(cwd.c_str()) != 0)
                    fail(errno);
                ::execv(argv[0], argv.data());
                fail(errno);
            }

            return finishChild(pid, errorPipe, args[0], result);
        }

        inline std::optional<Path> nativeExecutablePath()
        {
#if defined(__linux__)
            std::error_code linkError;
            const Path link = std::filesystem::read_symlink("/proc/self/exe", linkError);
            if (linkError || link.empty())
                return std::nullopt;
            std::string text = pathText(link);
            const std::string deleted = " (deleted)";
            if (text.size() > deleted.size() && text.ends_with(deleted))
                text.resize(text.size() - deleted.size());
            return pathFromText(text);
#elif defined(__APPLE__)
            std::uint32_t size = 0;
            if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0)
                return std::nullopt;
            std::string buffer(size, '\0');
            if (_NSGetExecutablePath(buffer.data(), &size) != 0)
                return std::nullopt;
            std::error_code canonicalError;
            const Path canonical =
                std::filesystem::weakly_canonical(pathFromText(buffer.c_str()), canonicalError);
            return canonicalError ? pathFromText(buffer.c_str()) : canonical;
#else
            return std::nullopt;
#endif
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

        inline std::optional<std::string> environmentValue(const char *name)
        {
            const char *value = std::getenv(name);
            if (value == nullptr)
                return std::nullopt;
            return std::string(value);
        }

        inline bool isExecutable(const Path &path)
        {
            return ::access(path.c_str(), X_OK) == 0;
        }

        inline bool supportsMsvc()
        {
            return false;
        }

        inline unsigned long processId()
        {
            return static_cast<unsigned long>(::getpid());
        }

        inline bool setTempEnvironment(const Path &tempDir, std::string &error)
        {
            const std::string text = pathText(tempDir);
            for (const char *name : {"TMPDIR", "TMP", "TEMP"})
            {
                if (::setenv(name, text.c_str(), 1) != 0)
                {
                    const int code = errno;
                    error = processErrorText((std::string("cannot set ") + name).c_str(), code);
                    return false;
                }
            }
            return true;
        }

        // Cross-process build-directory mutex. fcntl locks are released when the
        // descriptor is closed or the process exits.
        class BuildLock
        {
        public:
            BuildLock() = default;
            ~BuildLock()
            {
                if (fd_ >= 0)
                    ::close(fd_);
            }
            BuildLock(const BuildLock &) = delete;
            BuildLock &operator=(const BuildLock &) = delete;
            BuildLock(BuildLock &&) = delete;
            BuildLock &operator=(BuildLock &&) = delete;

            bool acquire(const Path &path, std::string &error)
            {
                if (fd_ >= 0)
                    return true;
                const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
                if (fd < 0)
                {
                    const int code = errno;
                    error = processErrorText(("cannot open '" + pathText(path) + "'").c_str(),
                                             code);
                    return false;
                }
                struct flock lock = {};
                lock.l_type = F_WRLCK;
                lock.l_whence = SEEK_SET;
                lock.l_start = 0;
                lock.l_len = 0;
                if (::fcntl(fd, F_SETLK, &lock) != 0)
                {
                    const int code = errno;
                    ::close(fd);
                    if (code == EACCES || code == EAGAIN)
                        error = "another builder is already using '" + pathText(path.parent_path()) +
                                "'";
                    else
                        error = processErrorText(("cannot lock '" + pathText(path) + "'").c_str(),
                                                 code);
                    return false;
                }
                fd_ = fd;
                return true;
            }

        private:
            int fd_ = -1;
        };

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
