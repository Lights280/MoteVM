// Windows implementation header. Included only by builder.cpp after the common
// helpers it uses; it is not a standalone translation unit.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // PROC_THREAD_ATTRIBUTE_HANDLE_LIST requires Vista+
#endif

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

#include "platform.hpp"

namespace BUILD
{
    namespace platform
    {
        inline std::wstring toWide(const std::string &text)
        {
            if (text.empty())
                return std::wstring();
            const int length = MultiByteToWideChar(
                CP_UTF8, static_cast<DWORD>(MB_ERR_INVALID_CHARS), text.c_str(),
                static_cast<int>(text.size()), nullptr, 0);
            if (length <= 0)
                return std::wstring();
            std::wstring result(static_cast<std::size_t>(length), L'\0');
            if (MultiByteToWideChar(CP_UTF8, static_cast<DWORD>(MB_ERR_INVALID_CHARS),
                                    text.c_str(), static_cast<int>(text.size()),
                                    result.data(), length) != length)
                return std::wstring();
            return result;
        }

        inline std::string fromWide(const std::wstring &text)
        {
            if (text.empty())
                return std::string();
            const int length = WideCharToMultiByte(
                CP_UTF8, static_cast<DWORD>(WC_ERR_INVALID_CHARS), text.c_str(),
                static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (length <= 0)
                return std::string();
            std::string result(static_cast<std::size_t>(length), '\0');
            if (WideCharToMultiByte(CP_UTF8, static_cast<DWORD>(WC_ERR_INVALID_CHARS),
                                    text.c_str(), static_cast<int>(text.size()),
                                    result.data(), length, nullptr, nullptr) != length)
                return std::string();
            return result;
        }

        inline std::string windowsErrorText(DWORD code)
        {
            return "(Windows error " +
                   std::to_string(static_cast<unsigned long>(code)) + ")";
        }

        inline std::string pathText(const Path &path)
        {
            return fromWide(path.wstring());
        }

        inline Path pathFromText(const std::string &text)
        {
            return Path(toWide(text));
        }

        inline std::optional<std::string> environmentValue(const char *name)
        {
            const std::wstring wideName = toWide(name);
            if (wideName.empty())
                return std::nullopt;
            SetLastError(ERROR_SUCCESS);
            const DWORD needed = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
            if (needed == 0)
            {
                if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                    return std::nullopt;
                return std::string();
            }
            std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
            const DWORD written =
                GetEnvironmentVariableW(wideName.c_str(), buffer.data(), needed);
            if (written >= needed)
                return std::nullopt; // the value grew between the two calls
            buffer.resize(static_cast<std::size_t>(written));
            return fromWide(buffer);
        }

        inline std::wstring quoteWindowsArg(const std::wstring &argument,
                                            bool forceQuotes = false)
        {
            if (argument.empty())
                return L"\"\"";
            bool needsQuotes = forceQuotes;
            for (wchar_t c : argument)
            {
                if (c == L' ' || c == L'\t' || c == L'"')
                {
                    needsQuotes = true;
                    break;
                }
            }
            if (!needsQuotes)
                return argument;
            std::wstring out;
            out.push_back(L'"');
            std::size_t backslashes = 0;
            for (wchar_t c : argument)
            {
                if (c == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == L'"')
                {
                    out.append(backslashes * 2 + 1, L'\\');
                    out.push_back(L'"');
                    backslashes = 0;
                    continue;
                }
                out.append(backslashes, L'\\');
                backslashes = 0;
                out.push_back(c);
            }
            out.append(backslashes * 2, L'\\');
            out.push_back(L'"');
            return out;
        }

        // Command-line quoting for a line that cmd.exe passes through to a
        // CRT child. Metacharacters force quotes even when there is no space;
        // embedded quotes are rejected by the re-exec caller.
        inline std::wstring quoteBatchArg(const std::wstring &argument)
        {
            bool forceQuotes = false;
            for (wchar_t c : argument)
            {
                if (c == L'&' || c == L'|' || c == L'<' || c == L'>' || c == L'^' ||
                    c == L'(' || c == L')' || c == L'%' || c == L'!')
                {
                    forceQuotes = true;
                    break;
                }
            }
            return quoteWindowsArg(argument, forceQuotes);
        }

        inline bool appendCommandLine(const std::vector<std::string> &args, bool cmdQuoting,
                                      std::wstring &commandLine, std::string &error)
        {
            for (const std::string &argument : args)
            {
                const std::wstring wide = toWide(argument);
                if (wide.empty() && !argument.empty())
                {
                    error = "cannot convert a command-line argument to UTF-16";
                    return false;
                }
                if (!commandLine.empty())
                    commandLine.push_back(L' ');
                commandLine += cmdQuoting ? quoteBatchArg(wide) : quoteWindowsArg(wide);
            }
            return true;
        }

        // cmd.exe expands %NAME% even inside double quotes. Escape only text
        // that must reach the child literally; %ERRORLEVEL% in the script is
        // intentionally left unescaped.
        inline std::wstring escapeBatchPercent(const std::wstring &text)
        {
            std::wstring out;
            out.reserve(text.size());
            for (wchar_t c : text)
            {
                if (c == L'%')
                    out += L"%%";
                else
                    out.push_back(c);
            }
            return out;
        }

        // Batch files are read by cmd.exe using the console input code page
        // (chcp normally changes both, but they are independently settable).
        inline bool toConsoleEncoding(const std::wstring &text, std::string &out)
        {
            const UINT codePage = GetConsoleCP() == 0 ? CP_OEMCP : GetConsoleCP();
            BOOL usedDefault = FALSE;
            const int needed = WideCharToMultiByte(
                codePage, static_cast<DWORD>(WC_NO_BEST_FIT_CHARS), text.c_str(),
                static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (needed <= 0)
                return false;
            std::string buffer(static_cast<std::size_t>(needed), '\0');
            if (WideCharToMultiByte(codePage, static_cast<DWORD>(WC_NO_BEST_FIT_CHARS),
                                    text.c_str(), static_cast<int>(text.size()),
                                    buffer.data(), needed, nullptr, &usedDefault) != needed ||
                usedDefault)
                return false;
            out = std::move(buffer);
            return true;
        }

        // RAII wrapper for Win32 handles. INVALID_HANDLE_VALUE and nullptr are
        // treated as empty; CloseHandle is called exactly once.
        class UniqueHandle
        {
        public:
            UniqueHandle() = default;
            explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
            ~UniqueHandle() { reset(); }
            UniqueHandle(const UniqueHandle &) = delete;
            UniqueHandle &operator=(const UniqueHandle &) = delete;
            UniqueHandle(UniqueHandle &&other) noexcept : handle_(other.release()) {}
            UniqueHandle &operator=(UniqueHandle &&other) noexcept
            {
                if (this != &other)
                    reset(other.release());
                return *this;
            }

            HANDLE get() const { return handle_; }
            HANDLE release() { return std::exchange(handle_, INVALID_HANDLE_VALUE); }
            void reset(HANDLE handle = INVALID_HANDLE_VALUE)
            {
                if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr)
                    CloseHandle(handle_);
                handle_ = handle;
            }
            explicit operator bool() const
            {
                return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
            }

        private:
            HANDLE handle_ = INVALID_HANDLE_VALUE;
        };

        inline bool waitForProcess(HANDLE process, const std::string &executable,
                                   ProcessResult &result)
        {
            const DWORD wait = WaitForSingleObject(process, INFINITE);
            if (wait == WAIT_FAILED)
            {
                const DWORD code = GetLastError();
                result.spawnError =
                    "cannot wait for '" + executable + "' " + windowsErrorText(code);
                return false;
            }
            // WaitForSingleObject already confirmed termination. At this point
            // STILL_ACTIVE (259) is a legitimate exit code, not "still running";
            // only a failing GetExitCodeProcess is an error.
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(process, &exitCode))
            {
                const DWORD code = GetLastError();
                result.spawnError = "cannot read the exit code of '" + executable + "' " +
                                    windowsErrorText(code);
                return false;
            }
            result.started = true;
            result.exitCode = static_cast<int>(exitCode);
            return true;
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

            SECURITY_ATTRIBUTES security{};
            security.nLength = static_cast<DWORD>(sizeof(security));
            security.bInheritHandle = TRUE;

            UniqueHandle logHandle(CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE,
                                               FILE_SHARE_READ, &security, CREATE_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!logHandle)
            {
                result.spawnError = "cannot create log file '" + pathText(logPath) + "'";
                return result;
            }

            HANDLE duplicated = INVALID_HANDLE_VALUE;
            if (!DuplicateHandle(GetCurrentProcess(), logHandle.get(), GetCurrentProcess(),
                                 &duplicated, 0, TRUE, DUPLICATE_SAME_ACCESS))
            {
                const DWORD code = GetLastError();
                result.spawnError = "cannot duplicate the log handle " + windowsErrorText(code);
                return result;
            }
            UniqueHandle errorHandle(duplicated);

            UniqueHandle inputHandle(CreateFileW(L"NUL", GENERIC_READ,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!inputHandle)
            {
                const DWORD code = GetLastError();
                result.spawnError =
                    "cannot open NUL for the child's stdin " + windowsErrorText(code);
                return result;
            }

            std::wstring commandLine;
            if (!appendCommandLine(args, false, commandLine, result.spawnError))
                return result;
            std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
            mutableLine.push_back(L'\0');
            const std::wstring application = toWide(args[0]);
            if (application.empty() && !args[0].empty())
            {
                result.spawnError = "cannot convert the executable path to UTF-16";
                return result;
            }
            const std::wstring working = cwd.wstring();

            SIZE_T attributeSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1u, 0u, &attributeSize);
            if (attributeSize == 0)
            {
                result.spawnError = "cannot size the process attribute list";
                return result;
            }
            // The attribute-list API writes into an opaque, max-aligned buffer.
            std::vector<std::max_align_t> attributeStorage(
                (attributeSize + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
            auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                attributeStorage.data());
            if (!InitializeProcThreadAttributeList(attributes, 1u, 0u, &attributeSize))
            {
                const DWORD code = GetLastError();
                result.spawnError =
                    "cannot create the process attribute list " + windowsErrorText(code);
                return result;
            }
            HANDLE inherited[] = {inputHandle.get(), logHandle.get(), errorHandle.get()};
            if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                           inherited, sizeof(inherited), nullptr, nullptr))
            {
                const DWORD code = GetLastError();
                DeleteProcThreadAttributeList(attributes);
                result.spawnError =
                    "cannot restrict inherited handles " + windowsErrorText(code);
                return result;
            }

            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = static_cast<DWORD>(sizeof(startup));
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = inputHandle.get();
            startup.StartupInfo.hStdOutput = logHandle.get();
            startup.StartupInfo.hStdError = errorHandle.get();
            startup.lpAttributeList = attributes;

            PROCESS_INFORMATION process{};
            const BOOL created = CreateProcessW(
                application.c_str(), mutableLine.data(), nullptr, nullptr, TRUE,
                static_cast<DWORD>(EXTENDED_STARTUPINFO_PRESENT), nullptr,
                working.empty() ? nullptr : working.c_str(), &startup.StartupInfo, &process);
            const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
            DeleteProcThreadAttributeList(attributes);
            if (!created)
            {
                result.spawnError =
                    "cannot start '" + args[0] + "' " + windowsErrorText(createError);
                return result;
            }
            UniqueHandle processHandle(process.hProcess);
            UniqueHandle threadHandle(process.hThread);
            (void)threadHandle; // RAII only: closes the thread handle on scope exit.

            waitForProcess(processHandle.get(), args[0], result);
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

            std::wstring commandLine;
            if (!appendCommandLine(args, true, commandLine, result.spawnError))
                return result;
            std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
            mutableLine.push_back(L'\0');

            STARTUPINFOW startup{};
            startup.cb = static_cast<DWORD>(sizeof(startup));
            PROCESS_INFORMATION process{};
            const std::wstring application = toWide(args[0]);
            if (application.empty() && !args[0].empty())
            {
                result.spawnError = "cannot convert the executable path to UTF-16";
                return result;
            }
            const std::wstring working = cwd.wstring();
            const BOOL created = CreateProcessW(
                application.c_str(), mutableLine.data(), nullptr, nullptr, TRUE, 0, nullptr,
                working.empty() ? nullptr : working.c_str(), &startup, &process);
            if (!created)
            {
                const DWORD code = GetLastError();
                result.spawnError =
                    "cannot start '" + args[0] + "' " + windowsErrorText(code);
                return result;
            }
            UniqueHandle processHandle(process.hProcess);
            UniqueHandle threadHandle(process.hThread);
            (void)threadHandle; // RAII only: closes the thread handle on scope exit.

            waitForProcess(processHandle.get(), args[0], result);
            return result;
        }

        inline std::optional<Path> nativeExecutablePath()
        {
            std::vector<wchar_t> buffer(1024);
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                        static_cast<DWORD>(buffer.size()));
                if (length == 0)
                    break;
                if (static_cast<std::size_t>(length) < buffer.size())
                    return Path(std::wstring(buffer.data(), static_cast<std::size_t>(length)));
                buffer.resize(buffer.size() * 2);
            }
            return std::nullopt;
        }

        inline std::string formatCommand(const std::vector<std::string> &args)
        {
            std::string out;
            for (const std::string &argument : args)
            {
                if (!out.empty())
                    out.push_back(' ');
                out += fromWide(quoteWindowsArg(toWide(argument)));
            }
            return out;
        }

        inline bool pathTextEquals(const std::string &left, const std::string &right)
        {
            const std::wstring wideLeft = toWide(left);
            const std::wstring wideRight = toWide(right);
            if (wideLeft.empty() && wideRight.empty())
                return left.empty() && right.empty();
            return CompareStringOrdinal(
                       wideLeft.c_str(), static_cast<int>(wideLeft.size()),
                       wideRight.c_str(), static_cast<int>(wideRight.size()), TRUE) ==
                   CSTR_EQUAL;
        }

        inline std::string executableSuffix()
        {
            return ".exe";
        }

        inline char pathListSeparator()
        {
            return ';';
        }

        inline std::vector<std::string> executableNames(const std::string &name,
                                                         bool hasExtension)
        {
            std::vector<std::string> names;
            names.push_back(name);
            if (!hasExtension)
                names.push_back(name + ".exe");
            return names;
        }

        inline bool isExecutable(const Path &path)
        {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error);
        }

        inline bool supportsMsvc()
        {
            return true;
        }

        inline unsigned long processId()
        {
            return static_cast<unsigned long>(GetCurrentProcessId());
        }

        inline bool setTempEnvironment(const Path &tempDir, std::string &error)
        {
            const std::wstring wide = tempDir.wstring();
            for (const wchar_t *name : {L"TMP", L"TEMP", L"TMPDIR"})
            {
                if (!SetEnvironmentVariableW(name, wide.c_str()))
                {
                    const DWORD code = GetLastError();
                    error = "cannot set the compiler temporary directory " +
                            windowsErrorText(code);
                    return false;
                }
            }
            return true;
        }

        // Cross-process build-directory mutex implemented with an exclusive
        // CreateFileW handle (share mode 0), released on close/process exit.
        class BuildLock
        {
        public:
            BuildLock() = default;
            ~BuildLock()
            {
                if (handle_ != INVALID_HANDLE_VALUE)
                    CloseHandle(handle_);
            }
            BuildLock(const BuildLock &) = delete;
            BuildLock &operator=(const BuildLock &) = delete;
            BuildLock(BuildLock &&) = delete;
            BuildLock &operator=(BuildLock &&) = delete;

            bool acquire(const Path &path, std::string &error)
            {
                if (handle_ != INVALID_HANDLE_VALUE)
                    return true;
                const HANDLE handle =
                    CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                {
                    const DWORD code = GetLastError();
                    if (code == ERROR_SHARING_VIOLATION)
                        error = "another builder is already using '" +
                                pathText(path.parent_path()) + "'";
                    else
                        error = "cannot lock '" + pathText(path) + "' " +
                                windowsErrorText(code);
                    return false;
                }
                handle_ = handle;
                return true;
            }

        private:
            HANDLE handle_ = INVALID_HANDLE_VALUE;
        };

        inline std::optional<Path> findVsDevCmd(const Path &logsDir, const Path &root)
        {
            const char *environmentNames[] = {"VSINSTALLDIR", "VS2026INSTALLDIR",
                                              "VS2022INSTALLDIR", "VS2019INSTALLDIR",
                                              "VS2017INSTALLDIR"};
            for (const char *name : environmentNames)
            {
                const std::optional<std::string> value = environmentValue(name);
                if (!value || value->empty())
                    continue;
                const Path candidate = pathFromText(*value) / "Common7" / "Tools" /
                                       "VsDevCmd.bat";
                if (fileExists(candidate))
                    return candidate;
            }
            std::vector<Path> vswhereCandidates;
            const std::optional<std::string> programFiles =
                environmentValue("ProgramFiles(x86)");
            if (programFiles && !programFiles->empty())
            {
                vswhereCandidates.push_back(pathFromText(*programFiles) /
                                            "Microsoft Visual Studio" / "Installer" /
                                            "vswhere.exe");
            }
            const std::optional<Path> onPath = findExecutable("vswhere");
            if (onPath)
                vswhereCandidates.push_back(*onPath);
            for (const Path &vswhere : vswhereCandidates)
            {
                if (!fileExists(vswhere))
                    continue;
                const std::optional<std::string> output = runCapture(
                    {pathText(vswhere), "-latest", "-products", "*", "-utf8", "-property",
                     "installationPath"},
                    logsDir / "probe-vswhere.log", root);
                if (!output)
                    continue;
                const std::string install = trimText(*output);
                if (install.empty())
                    continue;
                const Path candidate =
                    pathFromText(install) / "Common7" / "Tools" / "VsDevCmd.bat";
                if (fileExists(candidate))
                    return candidate;
            }
            return std::nullopt;
        }

        inline bool reexecInDevShell(const Options &options, int argc, char *argv[], Arch arch,
                                     const Path &logsDir, int &exitCode, std::string &error)
        {
            exitCode = 0;
            const std::optional<std::string> reexecMarker = environmentValue(REEXEC_ENV);
            if (reexecMarker && *reexecMarker == "1")
            {
                error = std::string("MSVC architecture '") + archName(arch) +
                        "' is still not active after re-exec; open a Developer PowerShell "
                        "with -arch=" +
                        archName(arch);
                return false;
            }
            if (options.currentExe.empty())
            {
                error = "cannot determine the builder executable for re-exec";
                return false;
            }
            const std::optional<Path> devCmd = findVsDevCmd(logsDir, options.root);
            if (!devCmd)
            {
                error = "Visual Studio VsDevCmd.bat was not found; open a Developer PowerShell "
                        "with -arch=" +
                        archName(arch);
                return false;
            }
            const Path tempDir = options.buildDir / ".builder" / "tmp";
            std::string directoryError;
            if (!ensureDirectory(tempDir, directoryError))
            {
                error = directoryError;
                return false;
            }
            // Keep the script outside .builder: "rebuild" in the child cleans that tree.
            const Path script = options.buildDir / pathFromText("reexec-" + archName(arch) + ".cmd");
            if (pathText(script).find('%') != std::string::npos)
            {
                error = "the build directory contains '%', which cmd.exe would expand; "
                        "choose another --build-dir";
                return false;
            }

            std::vector<std::string> forwarded;
            for (int i = 1; i < argc; ++i)
                forwarded.push_back(argv[i] == nullptr ? std::string() : std::string(argv[i]));

            std::wstring command;
            command += L"@echo off\r\n";
            command += L"setlocal DisableDelayedExpansion\r\n";
            command += L"call \"" + escapeBatchPercent(devCmd->wstring()) + L"\" -arch=" +
                       toWide(archName(arch)) + L" -host_arch=" +
                       toWide(archName(hostArch())) + L" -no_logo\r\n";
            command += L"if errorlevel 1 exit /b 1\r\n";
            command += L"set \"" + toWide(REEXEC_ENV) + L"=1\"\r\n";
            command += L"\"" + escapeBatchPercent(options.currentExe.wstring()) + L"\"";
            for (const std::string &argument : forwarded)
            {
                if (argument.find('"') != std::string::npos ||
                    argument.find('\n') != std::string::npos ||
                    argument.find('\r') != std::string::npos)
                {
                    error = "cannot re-execute with an argument containing a quote or newline; "
                            "open a Developer PowerShell with -arch=" +
                            archName(arch);
                    return false;
                }
                const std::wstring wide = toWide(argument);
                if (wide.empty() && !argument.empty())
                {
                    error = "cannot convert a forwarded argument to UTF-16";
                    return false;
                }
                command += L" " + escapeBatchPercent(quoteBatchArg(wide));
            }
            command += L"\r\nexit /b %ERRORLEVEL%\r\n";

            std::string encoded;
            if (!toConsoleEncoding(command, encoded))
            {
                error = "cannot encode the re-execution script in the console code page";
                return false;
            }
            if (!writeTextFileAtomic(script, encoded, error))
                return false;
            std::cerr << "note: re-executing in VS DevShell (" << archName(arch) << ") via "
                      << pathText(*devCmd) << "\n";

            std::optional<Path> commandProcessor = findExecutable("cmd.exe");
            if (!commandProcessor)
            {
                const std::optional<std::string> comspec = environmentValue("ComSpec");
                if (comspec && !comspec->empty())
                {
                    const Path candidate = pathFromText(*comspec);
                    if (fileExists(candidate))
                        commandProcessor = candidate;
                }
            }
            if (!commandProcessor)
            {
                error = "cmd.exe was not found";
                return false;
            }
            // The child inherits the build-directory lock held by this process.
            const std::wstring lockName = toWide(REEXEC_LOCK_ENV);
            if (!SetEnvironmentVariableW(lockName.c_str(), L"1"))
            {
                const DWORD code = GetLastError();
                error = "cannot mark the re-executed builder as the lock holder " +
                        windowsErrorText(code);
                return false;
            }

            Path workingDirectory = options.root;
            std::error_code cwdError;
            const Path currentDirectory = std::filesystem::current_path(cwdError);
            if (!cwdError)
                workingDirectory = currentDirectory;
            const ProcessResult result = runProcessInherited(
                {pathText(*commandProcessor), "/d", "/c", pathText(script)}, workingDirectory);
            removeQuiet(script);
            if (!result.started)
            {
                error = result.spawnError;
                return false;
            }
            exitCode = result.exitCode;
            return true;
        }
    }
}
