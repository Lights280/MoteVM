// Windows implementation header. Included only by builder.cpp after the common
// helpers it uses; it is not a standalone translation unit.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
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
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                                   static_cast<int>(text.size()), nullptr, 0);
            if (length <= 0)
                return std::wstring();
            std::wstring result(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                result.data(), length);
            return result;
        }

        inline std::string fromWide(const std::wstring &text)
        {
            if (text.empty())
                return std::string();
            const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                                   static_cast<int>(text.size()), nullptr, 0,
                                                   nullptr, nullptr);
            if (length <= 0)
                return std::string();
            std::string result(static_cast<std::size_t>(length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                result.data(), length, nullptr, nullptr);
            return result;
        }

        inline std::string pathText(const Path &path)
        {
            return fromWide(path.wstring());
        }

        inline Path pathFromText(const std::string &text)
        {
            return Path(toWide(text));
        }

        inline std::wstring quoteWindowsArg(const std::wstring &argument)
        {
            if (argument.empty())
                return L"\"\"";
            bool needsQuotes = false;
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

        inline ProcessResult spawnProcess(const std::vector<std::string> &args,
                                         const Path &logPath, const Path &cwd)
        {
            ProcessResult result;
            SECURITY_ATTRIBUTES security{};
            security.nLength = static_cast<DWORD>(sizeof(security));
            security.bInheritHandle = TRUE;
            HANDLE logHandle = CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE,
                                           FILE_SHARE_READ, &security, CREATE_ALWAYS,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (logHandle == INVALID_HANDLE_VALUE)
            {
                result.spawnError = "cannot create log file '" + pathText(logPath) + "'";
                return result;
            }
            HANDLE errorHandle = INVALID_HANDLE_VALUE;
            if (!DuplicateHandle(GetCurrentProcess(), logHandle, GetCurrentProcess(), &errorHandle,
                                 0, TRUE, DUPLICATE_SAME_ACCESS))
            {
                CloseHandle(logHandle);
                result.spawnError = "cannot duplicate the log handle";
                return result;
            }
            STARTUPINFOW startup{};
            startup.cb = static_cast<DWORD>(sizeof(startup));
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = logHandle;
            startup.hStdError = errorHandle;
            HANDLE inputHandle = CreateFileW(L"NUL", GENERIC_READ,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            startup.hStdInput = inputHandle == INVALID_HANDLE_VALUE
                                    ? GetStdHandle(STD_INPUT_HANDLE)
                                    : inputHandle;
            PROCESS_INFORMATION process{};
            std::wstring commandLine;
            for (const std::string &argument : args)
            {
                if (!commandLine.empty())
                    commandLine.push_back(L' ');
                commandLine += quoteWindowsArg(toWide(argument));
            }
            std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
            mutableLine.push_back(L'\0');
            const std::wstring application = toWide(args[0]);
            const std::wstring working = cwd.wstring();
            const BOOL created = CreateProcessW(application.c_str(), mutableLine.data(), nullptr,
                                                nullptr, TRUE, 0, nullptr, working.c_str(),
                                                &startup, &process);
            if (!created)
            {
                const DWORD code = GetLastError();
                result.spawnError = "cannot start '" + args[0] + "' (Windows error " +
                                    std::to_string(static_cast<unsigned long>(code)) + ")";
                if (inputHandle != INVALID_HANDLE_VALUE)
                    CloseHandle(inputHandle);
                CloseHandle(errorHandle);
                CloseHandle(logHandle);
                return result;
            }
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(process.hProcess, &exitCode))
                exitCode = 1;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            if (inputHandle != INVALID_HANDLE_VALUE)
                CloseHandle(inputHandle);
            CloseHandle(errorHandle);
            CloseHandle(logHandle);
            result.started = true;
            result.exitCode = static_cast<int>(exitCode);
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
            for (const std::string &argument : args)
            {
                if (!commandLine.empty())
                    commandLine.push_back(L' ');
                commandLine += quoteWindowsArg(toWide(argument));
            }
            std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
            mutableLine.push_back(L'\0');
            STARTUPINFOW startup{};
            startup.cb = static_cast<DWORD>(sizeof(startup));
            PROCESS_INFORMATION process{};
            const std::wstring application = toWide(args[0]);
            const std::wstring working = cwd.wstring();
            const BOOL created = CreateProcessW(
                application.c_str(), mutableLine.data(), nullptr, nullptr, TRUE, 0, nullptr,
                working.empty() ? nullptr : working.c_str(), &startup, &process);
            if (!created)
            {
                result.spawnError = "cannot start '" + args[0] + "'";
                return result;
            }
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(process.hProcess, &exitCode))
                exitCode = 1;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            result.started = true;
            result.exitCode = static_cast<int>(exitCode);
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
            if (left.size() != right.size())
                return false;
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                char a = left[i];
                char b = right[i];
                if (a >= 'A' && a <= 'Z')
                    a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z')
                    b = static_cast<char>(b - 'A' + 'a');
                if (a != b)
                    return false;
            }
            return true;
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

        inline void setTempEnvironment(const Path &tempDir)
        {
            const std::wstring wide = tempDir.wstring();
            SetEnvironmentVariableW(L"TMP", wide.c_str());
            SetEnvironmentVariableW(L"TEMP", wide.c_str());
            SetEnvironmentVariableW(L"TMPDIR", wide.c_str());
        }

        inline std::optional<Path> findVsDevCmd(const Path &logsDir, const Path &root)
        {
            const char *environmentNames[] = {"VSINSTALLDIR", "VS2026INSTALLDIR",
                                              "VS2022INSTALLDIR", "VS2019INSTALLDIR",
                                              "VS2017INSTALLDIR"};
            for (const char *name : environmentNames)
            {
                const char *value = std::getenv(name);
                if (value == nullptr || *value == '\0')
                    continue;
                const Path candidate = pathFromText(value) / "Common7" / "Tools" /
                                       "VsDevCmd.bat";
                if (fileExists(candidate))
                    return candidate;
            }
            std::vector<Path> vswhereCandidates;
            const char *programFiles = std::getenv("ProgramFiles(x86)");
            if (programFiles != nullptr && *programFiles != '\0')
                vswhereCandidates.push_back(pathFromText(programFiles) /
                                            "Microsoft Visual Studio" / "Installer" /
                                            "vswhere.exe");
            const std::optional<Path> onPath = findExecutable("vswhere");
            if (onPath)
                vswhereCandidates.push_back(*onPath);
            for (const Path &vswhere : vswhereCandidates)
            {
                if (!fileExists(vswhere))
                    continue;
                const std::optional<std::string> output = runCapture(
                    {pathText(vswhere), "-latest", "-products", "*", "-property",
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
            if (std::getenv(REEXEC_ENV) != nullptr)
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
            std::ostringstream command;
            command << "@echo off\r\n";
            command << "call \"" << pathText(*devCmd) << "\" -arch=" << archName(arch)
                    << " -host_arch=" << archName(hostArch()) << " -no_logo\r\n";
            command << "if errorlevel 1 exit /b 1\r\n";
            command << "set \"" << REEXEC_ENV << "=1\"\r\n";
            command << "\"" << pathText(options.currentExe) << "\"";
            std::vector<std::string> forwarded;
            for (int i = 1; i < argc; ++i)
                forwarded.push_back(argv[i] == nullptr ? std::string() : std::string(argv[i]));
            if (!forwarded.empty())
                command << " " << formatCommand(forwarded);
            command << "\r\nexit /b %ERRORLEVEL%\r\n";
            if (!writeTextFileAtomic(script, command.str(), error))
                return false;
            std::cerr << "note: re-executing in VS DevShell (" << archName(arch) << ") via "
                      << pathText(*devCmd) << "\n";

            std::optional<Path> commandProcessor = findExecutable("cmd.exe");
            if (!commandProcessor)
            {
                const char *comspec = std::getenv("ComSpec");
                if (comspec != nullptr && *comspec != '\0')
                {
                    const Path candidate = pathFromText(comspec);
                    if (fileExists(candidate))
                        commandProcessor = candidate;
                }
            }
            if (!commandProcessor)
            {
                error = "cmd.exe was not found";
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
