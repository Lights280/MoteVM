// ============================================================================
// src/builder/builder.cpp — MoteVM project build system (single translation unit)
//
// This file is a self-contained build driver for this repository. It does not
// read CMakeLists.txt: it knows the project layout, warning set and output
// paths directly.
//
// Everything deliberately stays in one translation unit so `builder self` can
// rebuild it with one compiler invocation and no extra objects. Platform
// specific code lives in include/{posix,win32}.hpp, shared declarations in
// include/{types,cli,platform}.hpp, and the command line is parsed by
// CommandLineParser.
//
// Bootstrap (one time):
//   Windows: clang++ -std=c++20 -O2 -o build\bin\builder.exe src\builder\builder.cpp
//   POSIX  : clang++ -std=c++20 -O2 -o build/bin/builder src/builder/builder.cpp
//
// Usage:
//   build\bin\builder.exe [build] [options] [target...]
//   build\bin\builder.exe clean | rebuild | self | doctor | targets | help
//
// Layout:
//   src/<module>/<module>.cpp   module entry (one executable target)
//   src/<module>/include/       module headers (optional)
//   src/include/                shared code + headers (core/archive/tools)
//   src/builder/include/        builder-only platform/CLI headers
//
// Targets: all (every module discovered under src/), <module>, builder
// Shared groups: motevm_core (parse/verifier/disasm), motevm_tools (tooling);
// modules link them via Target::links and they are compiled once per build.
// Compilers: clang++ (default), g++, MSVC cl; flags: --compiler, --arch, --release
//
// Environment:
//   MOTEVM_CXX, MOTEVM_BUILD_DIR, MOTEVM_JOBS
// Child compilers run with TMP/TEMP/TMPDIR=<build>/.builder/tmp so temporary
// files stay inside the build tree.
//
// Output layout (config/arch/toolchain isolated):
//   build/<toolchain>-<arch>-<config>/bin/<module>[.exe]
//   build/<toolchain>-<arch>-<config>/.builder/{obj,dep,state,logs}/
//   build/bin/builder[.exe]     self/bootstrap
// "clean" removes only those generated directories, never CMake files.
// ============================================================================

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "include/types.hpp"
#include "include/platform.hpp"
#include "include/cli.hpp"

// ============================================================================
// Text, path, file and hashing utilities
// ============================================================================

namespace BUILD
{
    namespace
    {
        using platform::pathFromText;
        using platform::pathText;

        std::string lowerAscii(std::string text)
        {
            for (char &c : text)
            {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            return text;
        }

        std::string trimText(const std::string &text)
        {
            std::size_t begin = 0;
            std::size_t end = text.size();
            while (begin < end)
            {
                const char c = text[begin];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                    break;
                ++begin;
            }
            while (end > begin)
            {
                const char c = text[end - 1];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                    break;
                --end;
            }
            return text.substr(begin, end - begin);
        }

        std::string firstLine(const std::string &text)
        {
            const std::size_t pos = text.find_first_of("\r\n");
            return trimText(text.substr(0, pos));
        }

        bool fileExists(const Path &path)
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(path, ec);
        }

        bool directoryExists(const Path &path)
        {
            std::error_code ec;
            return std::filesystem::is_directory(path, ec);
        }

        bool ensureDirectory(const Path &path, std::string &error)
        {
            if (directoryExists(path))
                return true;
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            if (ec)
            {
                error = "cannot create '" + pathText(path) + "': " + ec.message();
                return false;
            }
            return true;
        }
        bool readTextFile(const Path &path, std::string &text)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            std::ostringstream buffer;
            buffer << input.rdbuf();
            if (input.bad())
                return false;
            text = buffer.str();
            return true;
        }

        bool writeTextFileAtomic(const Path &path, const std::string &text, std::string &error)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec)
            {
                error = "cannot create '" + pathText(path.parent_path()) + "': " + ec.message();
                return false;
            }
            Path temporary = path;
            temporary += pathFromText(".tmp");
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    error = "cannot write '" + pathText(temporary) + "'";
                    return false;
                }
                output.write(text.data(), static_cast<std::streamsize>(text.size()));
                output.close();
                if (!output)
                {
                    error = "cannot write '" + pathText(temporary) + "'";
                    std::filesystem::remove(temporary, ec);
                    return false;
                }
            }
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(temporary, path, ec);
            if (ec)
            {
                error = "cannot install '" + pathText(path) + "': " + ec.message();
                std::filesystem::remove(temporary, ec);
                return false;
            }
            return true;
        }

        void removeQuiet(const Path &path)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        FileDigest digestFile(const Path &path)
        {
            FileDigest result;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return result;
            std::vector<char> buffer(64 * 1024);
            std::uint64_t hash = FNV_OFFSET;
            std::uint64_t size = 0;
            for (;;)
            {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize got = input.gcount();
                if (got > 0)
                {
                    const std::size_t count = static_cast<std::size_t>(got);
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(buffer[i]));
                        hash *= FNV_PRIME;
                    }
                    size += static_cast<std::uint64_t>(count);
                }
                if (got < static_cast<std::streamsize>(buffer.size()))
                    break;
            }
            if (input.bad())
                return result;
            result.ok = true;
            result.hash = hash;
            result.size = size;
            return result;
        }

        std::uint64_t hashText(std::string_view text)
        {
            std::uint64_t hash = FNV_OFFSET;
            for (char c : text)
            {
                hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
                hash *= FNV_PRIME;
            }
            return hash;
        }

        std::uint64_t hashArgs(const std::vector<std::string> &args)
        {
            std::uint64_t hash = FNV_OFFSET;
            for (const std::string &argument : args)
            {
                for (char c : argument)
                {
                    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
                    hash *= FNV_PRIME;
                }
                hash ^= 0x1Fu;
                hash *= FNV_PRIME;
            }
            return hash;
        }

        std::string hex64(std::uint64_t value)
        {
            static const char digits[] = "0123456789abcdef";
            std::string out(16, '0');
            for (int i = 15; i >= 0; --i)
            {
                out[static_cast<std::size_t>(i)] = digits[static_cast<std::size_t>(value & 0xFu)];
                value >>= 4;
            }
            return out;
        }

        bool parseHex64(const std::string &text, std::uint64_t &value)
        {
            if (text.empty())
                return false;
            try
            {
                std::size_t consumed = 0;
                const unsigned long long parsed = std::stoull(text, &consumed, 16);
                if (consumed != text.size())
                    return false;
                value = static_cast<std::uint64_t>(parsed);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        std::string displayPath(const Path &path, const Path &base)
        {
            std::error_code ec;
            const Path relative = std::filesystem::relative(path, base, ec);
            if (!ec && !relative.empty())
                return pathText(relative);
            return pathText(path);
        }

        std::string sanitizeForFile(const std::string &text)
        {
            std::string out;
            out.reserve(text.size());
            for (char c : text)
            {
                const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
                out.push_back(keep ? c : '_');
            }
            return out;
        }

        // ============================================================================
        // Child process execution
        // ============================================================================

        ProcessResult runProcess(const std::vector<std::string> &args, const Path &logPath,
                                 const Path &cwd)
        {
            if (args.empty())
            {
                ProcessResult result;
                result.spawnError = "empty command";
                return result;
            }
            std::string directoryError;
            if (!ensureDirectory(logPath.parent_path(), directoryError))
            {
                ProcessResult result;
                result.spawnError = directoryError;
                return result;
            }
            return platform::spawnProcess(args, logPath, cwd);
        }

        std::optional<Path> findExecutable(const std::string &name)
        {
            if (name.empty())
                return std::nullopt;
            const Path direct = pathFromText(name);
            if (direct.has_parent_path() || direct.is_absolute())
            {
                std::error_code ec;
                const Path absolute = std::filesystem::absolute(direct, ec);
                if (!ec && fileExists(absolute))
                    return absolute;
                return std::nullopt;
            }
            const char *pathEnv = std::getenv("PATH");
            if (pathEnv == nullptr)
                return std::nullopt;
            const std::string pathValue(pathEnv);
            const char separator = platform::pathListSeparator();
            const std::vector<std::string> names =
                platform::executableNames(name, !direct.extension().empty());
            std::size_t begin = 0;
            while (begin <= pathValue.size())
            {
                const std::size_t end = pathValue.find(separator, begin);
                std::string directory =
                    pathValue.substr(begin, end == std::string::npos ? std::string::npos
                                                                     : end - begin);
                directory = trimText(directory);
                if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"')
                    directory = directory.substr(1, directory.size() - 2);
                if (!directory.empty())
                {
                    for (const std::string &candidateName : names)
                    {
                        const Path candidate =
                            pathFromText(directory) / pathFromText(candidateName);
                        if (!fileExists(candidate))
                            continue;
                        std::error_code ec;
                        const Path absolute = std::filesystem::absolute(candidate, ec);
                        if (!ec)
                            return absolute;
                        return candidate;
                    }
                }
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            return std::nullopt;
        }

        Path currentExecutablePath(const char *argv0)
        {
            if (const std::optional<Path> native = platform::nativeExecutablePath())
                return *native;
            std::error_code ec;
            const Path fallback =
                std::filesystem::absolute(pathFromText(argv0 == nullptr ? "" : argv0), ec);
            if (!ec)
                return fallback;
            return Path();
        }

        std::optional<std::string> runCapture(const std::vector<std::string> &args,
                                              const Path &logPath, const Path &cwd)
        {
            const ProcessResult result = runProcess(args, logPath, cwd);
            std::string output;
            readTextFile(logPath, output);
            removeQuiet(logPath);
            if (!result.started || result.exitCode != 0)
                return std::nullopt;
            return output;
        }

        std::optional<std::string> runCaptureAny(const std::vector<std::string> &args,
                                                 const Path &logPath, const Path &cwd)
        {
            const ProcessResult result = runProcess(args, logPath, cwd);
            std::string output;
            readTextFile(logPath, output);
            removeQuiet(logPath);
            if (!result.started)
                return std::nullopt;
            if (output.empty() && result.exitCode != 0)
                return std::nullopt;
            return output;
        }

        bool samePath(const Path &left, const Path &right)
        {
            if (left.empty() || right.empty())
                return false;
            std::error_code ec;
            if (fileExists(left) && fileExists(right))
            {
                const bool equal = std::filesystem::equivalent(left, right, ec);
                if (!ec)
                    return equal;
            }
            return platform::pathTextEquals(pathText(left), pathText(right));
        }

        std::string exeSuffix()
        {
            return platform::executableSuffix();
        }

        Arch hostArch()
        {
            return sizeof(void *) == 8 ? Arch::X64 : Arch::X86;
        }

        Arch resolveArch(Arch requested)
        {
            return requested == Arch::Host ? hostArch() : requested;
        }

        std::string archName(Arch arch)
        {
            return arch == Arch::X86 ? "x86" : "x64";
        }

        std::string configName(Config config)
        {
            return config == Config::Debug ? "debug" : "release";
        }

        std::string toolchainName(Family family)
        {
            switch (family)
            {
            case Family::Clang:
                return "clang";
            case Family::Gcc:
                return "gcc";
            case Family::Msvc:
                return "msvc";
            default:
                return "unknown";
            }
        }

        std::string comboName(const Options &options, const Toolchain &toolchain)
        {
            return toolchainName(toolchain.family) + "-" + archName(toolchain.arch) + "-" +
                   configName(options.config);
        }

        const char *familyName(Family family)
        {
            switch (family)
            {
            case Family::Clang:
                return "clang";
            case Family::Gcc:
                return "gcc";
            case Family::Msvc:
                return "msvc";
            default:
                return "unknown";
            }
        }

        std::string readDefineValue(const Path &header, const std::string &name)
        {
            std::string text;
            if (!readTextFile(header, text))
                return "unknown";
            std::size_t position = text.find(name);
            while (position != std::string::npos)
            {
                const std::size_t equals = text.find('=', position + name.size());
                if (equals == std::string::npos)
                    break;
                const std::size_t firstQuote = text.find('"', equals + 1);
                if (firstQuote == std::string::npos)
                    break;
                const std::size_t secondQuote = text.find('"', firstQuote + 1);
                if (secondQuote == std::string::npos)
                    break;
                return text.substr(firstQuote + 1, secondQuote - firstQuote - 1);
            }
            return "unknown";
        }

        // ============================================================================
        // Toolchain discovery and compiler probing
        // ============================================================================

        std::string readDevShellArch()
        {
            const char *value = std::getenv("VSCMD_ARG_TGT_ARCH");
            if (value == nullptr || *value == '\0')
                return std::string();
            std::string arch = lowerAscii(trimText(value));
            if (arch == "amd64" || arch == "x86_64")
                arch = "x64";
            else if (arch == "win32" || arch == "i386" || arch == "i686")
                arch = "x86";
            return arch;
        }
    } // namespace
} // namespace BUILD

// ============================================================================
// Platform implementations
//
// Only one implementation header is included, exactly once, after the common
// helpers it uses. This keeps builder.cpp a single translation unit while all
// Windows/POSIX API calls live in the platform headers.
// ============================================================================

#ifdef _WIN32
#include "include/win32.hpp"
#else
#include "include/posix.hpp"
#endif

namespace BUILD
{
    namespace
    {
        using platform::formatCommand;
        using platform::pathFromText;
        using platform::pathText;
        using platform::reexecInDevShell;

        bool discoverToolchain(const Options &options, const Path &logsDir, Toolchain &toolchain,
                               std::string &error)
        {
            std::string directoryError;
            if (!ensureDirectory(logsDir, directoryError))
            {
                error = directoryError;
                return false;
            }

            std::vector<std::string> candidates;
            if (!options.cxx.empty())
            {
                candidates.push_back(pathText(options.cxx));
            }
            else
            {
                const char *environment = std::getenv("MOTEVM_CXX");
                if (environment != nullptr && *environment != '\0')
                    candidates.push_back(environment);
                environment = std::getenv("CXX");
                if (environment != nullptr && *environment != '\0')
                    candidates.push_back(environment);
                if (options.compiler == CompilerChoice::Auto ||
                    options.compiler == CompilerChoice::Clang)
                    candidates.push_back("clang++");
                if (options.compiler == CompilerChoice::Auto ||
                    options.compiler == CompilerChoice::Gcc)
                    candidates.push_back("g++");
                if (options.compiler == CompilerChoice::Auto ||
                    options.compiler == CompilerChoice::Msvc)
                    candidates.push_back("cl");
            }

            std::optional<Path> compiler;
            for (const std::string &candidate : candidates)
            {
                if (candidate.empty())
                    continue;
                const std::optional<Path> found = findExecutable(candidate);
                if (!found)
                    continue;
                const std::string base = lowerAscii(pathText(found->filename()));
                if (base == "clang-cl.exe" || base == "clang-cl")
                {
                    error = "MSVC-style driver '" + pathText(*found) +
                            "' is not supported; use clang++ or cl";
                    return false;
                }
                compiler = found;
                break;
            }
            if (!compiler)
            {
                if (options.compiler == CompilerChoice::Msvc)
                    error = "cl.exe not found; open a Visual Studio Developer PowerShell "
                            "(Enter-VsDevShell) or pass --cxx";
                else
                    error = "no C++ compiler found; use --compiler, --cxx, MOTEVM_CXX or CXX";
                return false;
            }
            toolchain.cxx = *compiler;

            const std::string baseName = lowerAscii(pathText(toolchain.cxx.filename()));
            if (baseName == "cl.exe" || baseName == "cl")
                toolchain.family = Family::Msvc;
            else if (baseName.find("clang") != std::string::npos)
                toolchain.family = Family::Clang;
            else if (baseName.find("g++") != std::string::npos ||
                     baseName.find("gcc") != std::string::npos)
                toolchain.family = Family::Gcc;

            if (toolchain.family == Family::Msvc)
            {
                const std::optional<std::string> versionText =
                    runCaptureAny({pathText(toolchain.cxx)},
                                  logsDir / "probe-msvc-version.log", options.root);
                if (!versionText || trimText(*versionText).empty())
                {
                    error = "cannot query the MSVC compiler '" + pathText(toolchain.cxx) + "'";
                    return false;
                }
                toolchain.versionLine = firstLine(*versionText);
                toolchain.msvcAbi = true;
                toolchain.depFiles = false;
                toolchain.arch = resolveArch(options.arch);
                toolchain.devShellArch = readDevShellArch();
                if (toolchain.devShellArch.empty())
                {
                    const std::string lowered = lowerAscii(toolchain.versionLine);
                    if (lowered.find("x64") != std::string::npos)
                        toolchain.devShellArch = "x64";
                    else if (lowered.find("x86") != std::string::npos)
                        toolchain.devShellArch = "x86";
                }
                toolchain.triple = toolchain.arch == Arch::X86 ? "i686-pc-windows-msvc"
                                                               : "x86_64-pc-windows-msvc";
                const std::optional<Path> linker = findExecutable("link");
                if (!linker)
                {
                    error = "link.exe not found; open a Visual Studio Developer PowerShell";
                    return false;
                }
                toolchain.linker = *linker;
                return true;
            }

            const std::optional<std::string> versionText =
                runCapture({pathText(toolchain.cxx), "--version"},
                           logsDir / "probe-cxx-version.log", options.root);
            if (!versionText)
            {
                error = "cannot query the C++ compiler '" + pathText(toolchain.cxx) + "'";
                return false;
            }
            toolchain.versionLine = firstLine(*versionText);

            if (toolchain.family == Family::Unknown)
            {
                const std::string loweredVersion = lowerAscii(*versionText);
                if (loweredVersion.find("clang") != std::string::npos)
                    toolchain.family = Family::Clang;
                else if (loweredVersion.find("gcc") != std::string::npos ||
                         loweredVersion.find("g++") != std::string::npos ||
                         loweredVersion.find("free software foundation") != std::string::npos)
                    toolchain.family = Family::Gcc;
            }
            if (toolchain.family == Family::Unknown)
            {
                error = "unsupported compiler family: '" + toolchain.versionLine + "'";
                return false;
            }

            const std::optional<std::string> tripleText =
                runCapture({pathText(toolchain.cxx), "-dumpmachine"},
                           logsDir / "probe-cxx-target.log", options.root);
            if (!tripleText)
            {
                error = "cannot query the compiler target with '" + pathText(toolchain.cxx) +
                        " -dumpmachine'";
                return false;
            }
            toolchain.triple = trimText(*tripleText);
            toolchain.msvcAbi = lowerAscii(toolchain.triple).find("msvc") != std::string::npos;
            toolchain.depFiles = true;
            toolchain.arch = resolveArch(options.arch);
            toolchain.devShellArch = readDevShellArch();
            if (toolchain.family == Family::Clang && toolchain.msvcAbi)
                toolchain.triple = toolchain.arch == Arch::X86 ? "i686-pc-windows-msvc"
                                                               : "x86_64-pc-windows-msvc";

            if (toolchain.family == Family::Gcc && toolchain.arch == Arch::X86)
            {
                const std::string loweredTripleCheck = lowerAscii(toolchain.triple);
                const bool already32 = loweredTripleCheck.find("i686") != std::string::npos ||
                                       loweredTripleCheck.find("i386") != std::string::npos ||
                                       loweredTripleCheck.find("i586") != std::string::npos;
                if (!already32)
                {
                    const std::optional<std::string> multilib =
                        runCapture({pathText(toolchain.cxx), "-print-multi-lib"},
                                   logsDir / "probe-gcc-multilib.log", options.root);
                    if (!multilib || lowerAscii(*multilib).find("32") == std::string::npos)
                    {
                        error = "this g++ build has no 32-bit multilib (-m32): '" +
                                pathText(toolchain.cxx) +
                                "'; use --cxx with a MinGW32 toolchain or --arch x64";
                        return false;
                    }
                }
            }

            if (toolchain.msvcAbi)
            {
                const std::optional<Path> lld = findExecutable("lld-link");
                if (lld)
                    toolchain.lldLink = *lld;
            }
            return true;
        }

        // ============================================================================
        // Project and module discovery
        // ============================================================================

        bool isLowerModuleName(const std::string &name)
        {
            if (name.empty() || name[0] < 'a' || name[0] > 'z')
                return false;
            for (char c : name)
            {
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
                    continue;
                return false;
            }
            return true;
        }

        void collectModuleSources(const Path &directory, const Path &moduleInclude,
                                  std::vector<Path> &sources)
        {
            std::error_code ec;
            std::vector<Path> entries;
            for (const std::filesystem::directory_entry &entry :
                 std::filesystem::directory_iterator(directory, ec))
            {
                if (ec)
                    break;
                entries.push_back(entry.path());
            }
            std::sort(entries.begin(), entries.end(),
                      [](const Path &left, const Path &right)
                      { return pathText(left) < pathText(right); });
            for (const Path &entry : entries)
            {
                if (directoryExists(entry))
                {
                    if (samePath(entry, moduleInclude))
                        continue;
                    collectModuleSources(entry, moduleInclude, sources);
                    continue;
                }
                if (lowerAscii(pathText(entry.extension())) == ".cpp")
                    sources.push_back(entry);
            }
        }

        std::string readProjectValue(const Path &root, const std::string &name)
        {
            const std::string sharedValue =
                readDefineValue(root / pathFromText("src/include/define.hpp"), name);
            if (sharedValue != "unknown")
                return sharedValue;
            return readDefineValue(root / pathFromText("src/motevm/include/define.hpp"), name);
        }

        Path selfOutputPath(const Options &options, const Path &buildDir)
        {
            std::error_code ec;
            if (!options.currentExe.empty())
            {
                const Path relative = std::filesystem::relative(options.currentExe, buildDir, ec);
                const std::string relativeText = pathText(relative);
                if (!ec && !relativeText.empty() && relativeText.rfind("..", 0) != 0)
                    return options.currentExe;
            }
            return buildDir / "bin" / pathFromText(std::string(TARGET_BUILDER) + exeSuffix());
        }

        bool makeProject(const Path &root, const Path &buildDir, const Options &options,
                         const Toolchain &toolchain, Project &project, std::string &error)
        {
            project = Project();
            project.root = root;
            project.buildDir = buildDir;
            project.version = readProjectValue(root, "MOTEVM_VERSION");
            project.formatVersion = readProjectValue(root, "MOTEBC_VERSION");

            const Path srcRoot = root / pathFromText("src");
            if (!directoryExists(srcRoot))
            {
                error = "missing source directory '" + pathText(srcRoot) + "'";
                return false;
            }
            const Path sharedInclude = srcRoot / pathFromText("include");
            std::vector<Path> entries;
            std::error_code ec;
            for (const std::filesystem::directory_entry &entry :
                 std::filesystem::directory_iterator(srcRoot, ec))
            {
                if (ec)
                    break;
                std::error_code typeError;
                if (entry.is_directory(typeError))
                    entries.push_back(entry.path());
            }
            std::sort(entries.begin(), entries.end(),
                      [](const Path &left, const Path &right)
                      { return pathText(left) < pathText(right); });

            const Path outputRoot = buildDir / pathFromText(comboName(options, toolchain));

            // Shared source groups (src/include). They are compiled once and
            // linked into the modules that need them; they are not selectable
            // user targets. Keep these lists in sync with the MOTEVM_CORE_*,
            // MOTEVM_ARCHIVE_* and MOTEVM_TOOL_* lists in the root CMakeLists.txt.
            const bool sharedReady = directoryExists(sharedInclude);
            if (sharedReady)
            {
                Target core;
                core.name = TARGET_CORE;
                core.internal = true;
                core.includes.push_back(sharedInclude);
                for (const char *file : {"parse.cpp", "verifier.cpp", "disasm.cpp"})
                {
                    const Path source = sharedInclude / pathFromText(file);
                    if (!fileExists(source))
                    {
                        error = "shared core source is missing: '" + pathText(source) + "'";
                        return false;
                    }
                    core.sources.push_back(source);
                }

                Target archive;
                archive.name = TARGET_ARCHIVE;
                archive.internal = true;
                archive.includes.push_back(sharedInclude);
                archive.links.push_back(core.name);
                for (const char *file : {"archive/archive.cpp", "archive/manifest.cpp",
                                         "archive/name.cpp", "archive/util.cpp",
                                         "archive/writer.cpp"})
                {
                    const Path source = sharedInclude / pathFromText(file);
                    if (!fileExists(source))
                    {
                        error = "shared archive source is missing: '" + pathText(source) + "'";
                        return false;
                    }
                    archive.sources.push_back(source);
                }

                Target tools;
                tools.name = TARGET_TOOLS;
                tools.internal = true;
                tools.includes.push_back(sharedInclude);
                tools.links.push_back(core.name);
                for (const char *file : {"common/diagnostics.cpp", "common/io.cpp",
                                         "common/path.cpp", "common/text.cpp",
                                         "common/trap.cpp", "asm/assembler.cpp",
                                         "asm/emitter.cpp", "asm/lexer.cpp", "asm/parser.cpp",
                                         "dis/disassembler.cpp"})
                {
                    const Path source = sharedInclude / pathFromText(file);
                    if (!fileExists(source))
                    {
                        error = "shared tool source is missing: '" + pathText(source) + "'";
                        return false;
                    }
                    tools.sources.push_back(source);
                }
                project.targets.push_back(std::move(core));
                project.targets.push_back(std::move(archive));
                project.targets.push_back(std::move(tools));
            }

            std::size_t moduleCount = 0;
            for (const Path &directory : entries)
            {
                const std::string directoryName = pathText(directory.filename());
                const std::string name = lowerAscii(directoryName);
                if (name == "include" || name == TARGET_BUILDER)
                    continue;
                if (name == TARGET_ALL || !isLowerModuleName(directoryName))
                {
                    error = "module directory '" + pathText(directory) +
                            "' must be lowercase [a-z][a-z0-9_]* and not 'all'";
                    return false;
                }
                const Path mainFile = directory / pathFromText(name + ".cpp");
                if (!fileExists(mainFile))
                {
                    error = "module '" + name + "' is missing main file '" +
                            pathText(mainFile) + "'";
                    return false;
                }
                Target target;
                target.name = name;
                target.mainFile = mainFile;
                collectModuleSources(directory, directory / pathFromText("include"),
                                     target.sources);
                if (target.sources.empty())
                {
                    error = "module '" + name + "' contains no .cpp files";
                    return false;
                }
                if (directoryExists(directory / pathFromText("include")))
                    target.includes.push_back(directory / pathFromText("include"));
                if (directoryExists(sharedInclude))
                    target.includes.push_back(sharedInclude);
                target.output = outputRoot / "bin" / pathFromText(name + exeSuffix());
                if (sharedReady)
                {
                    if (name == "motevm")
                        target.links.push_back(TARGET_CORE);
                    else if (name == "moteasm" || name == "motedis")
                        target.links.push_back(TARGET_TOOLS);
                    else if (name == "motear")
                    {
                        target.links.push_back(TARGET_ARCHIVE);
                        target.links.push_back(TARGET_TOOLS);
                    }
                }
                project.targets.push_back(std::move(target));
                ++moduleCount;
            }

            if (moduleCount == 0)
            {
                error = "no modules found under '" + pathText(srcRoot) + "'";
                return false;
            }

            Target self;
            self.name = TARGET_BUILDER;
            self.mainFile = root / pathFromText("src/builder/builder.cpp");
            if (!fileExists(self.mainFile))
            {
                error = "builder source is missing: '" + pathText(self.mainFile) + "'";
                return false;
            }
            self.sources.push_back(self.mainFile);
            if (directoryExists(sharedInclude))
                self.includes.push_back(sharedInclude);
            self.output = selfOutputPath(options, buildDir);
            project.targets.push_back(std::move(self));
            return true;
        }

        // ============================================================================
        // Compiler flags, output paths and link command construction
        // ============================================================================

        std::vector<std::string> warningFlags()
        {
            return {"-Wall", "-Wextra", "-Wpedantic",
                    "-Wconversion", "-Wsign-conversion", "-Wshadow",
                    "-Wcast-qual", "-Wundef", "-Wnon-virtual-dtor",
                    "-Wold-style-cast", "-Werror=return-type"};
        }

        std::string msvcTriple(Arch arch)
        {
            return arch == Arch::X86 ? "i686-pc-windows-msvc" : "x86_64-pc-windows-msvc";
        }

        std::vector<std::string> archFlags(const Toolchain &toolchain)
        {
            std::vector<std::string> flags;
            if (toolchain.family == Family::Msvc)
                return flags;
            if (toolchain.family == Family::Clang && toolchain.msvcAbi)
            {
                flags.push_back("--target=" + msvcTriple(toolchain.arch));
                return flags;
            }
            flags.push_back(toolchain.arch == Arch::X86 ? "-m32" : "-m64");
            return flags;
        }

        void appendGnuSanitizers(std::vector<std::string> &args)
        {
            args.push_back("-fsanitize=address,undefined");
            args.push_back("-fno-omit-frame-pointer");
            args.push_back("-fno-sanitize-recover=all");
        }

        std::vector<std::string> msvcCompileFlags(const Options &options)
        {
            std::vector<std::string> flags = {"/nologo", "/c", "/std:c++20", "/EHsc",
                                              "/permissive-", "/utf-8", "/Zc:__cplusplus",
                                              "/W4"};
            if (options.config == Config::Debug)
            {
                flags.push_back("/D_DEBUG");
                flags.push_back("/MDd");
                flags.push_back("/Od");
                flags.push_back("/Z7");
            }
            else
            {
                flags.push_back("/DNDEBUG");
                flags.push_back("/MD");
                flags.push_back("/O2");
            }
            if (options.werror)
                flags.push_back("/WX");
            if (options.sanitize)
                flags.push_back("/fsanitize=address");
            return flags;
        }

        std::vector<std::string> gnuCompileFlags(const Options &options,
                                                 const Toolchain &toolchain)
        {
            std::vector<std::string> flags;
            flags.push_back("-std=c++20");
            const std::vector<std::string> arch = archFlags(toolchain);
            flags.insert(flags.end(), arch.begin(), arch.end());
            if (options.config == Config::Debug)
            {
                flags.push_back("-O0");
                flags.push_back("-g");
                if (toolchain.msvcAbi)
                {
                    flags.push_back("-Xclang");
                    flags.push_back("-gcodeview");
                    flags.push_back("-D_DEBUG");
                }
            }
            else
            {
                flags.push_back("-O2");
                flags.push_back("-DNDEBUG");
            }
            if (toolchain.msvcAbi)
            {
                flags.push_back("-D_DLL");
                flags.push_back("-D_MT");
                flags.push_back("-Xclang");
                flags.push_back(options.config == Config::Debug ? "--dependent-lib=msvcrtd"
                                                                : "--dependent-lib=msvcrt");
            }
            const std::vector<std::string> warnings = warningFlags();
            flags.insert(flags.end(), warnings.begin(), warnings.end());
            if (options.werror)
                flags.push_back("-Werror");
            if (options.sanitize)
                appendGnuSanitizers(flags);
            return flags;
        }

        std::vector<std::string> compileFlags(const Options &options, const Toolchain &toolchain)
        {
            return toolchain.family == Family::Msvc ? msvcCompileFlags(options)
                                                    : gnuCompileFlags(options, toolchain);
        }

        void appendObjects(std::vector<std::string> &args, const std::vector<Path> &objects)
        {
            for (const Path &object : objects)
                args.push_back(pathText(object));
        }

        std::vector<std::string> msvcLinkArguments(const Options &options,
                                                   const Toolchain &toolchain,
                                                   const Path &output,
                                                   const std::vector<Path> &objects)
        {
            std::vector<std::string> args;
            const char *machine =
                toolchain.arch == Arch::X86 ? "/MACHINE:X86" : "/MACHINE:X64";
            if (options.sanitize)
            {
                args.push_back(pathText(toolchain.cxx));
                args.push_back("/nologo");
                args.push_back("/Fe" + pathText(output));
                if (options.config == Config::Debug)
                    args.push_back("/DEBUG");
                args.push_back("/fsanitize=address");
                appendObjects(args, objects);
                args.push_back("/link");
                args.push_back("/SUBSYSTEM:CONSOLE");
                args.push_back(machine);
                return args;
            }

            args.push_back(pathText(toolchain.linker));
            args.push_back("/NOLOGO");
            args.push_back("/INCREMENTAL:NO");
            args.push_back("/OUT:" + pathText(output));
            args.push_back("/SUBSYSTEM:CONSOLE");
            args.push_back(machine);
            if (options.config == Config::Debug)
            {
                args.push_back("/DEBUG");
                Path pdb = output;
                pdb += pathFromText(".pdb");
                args.push_back("/PDB:" + pathText(pdb));
            }
            appendObjects(args, objects);
            return args;
        }

        std::vector<std::string> gnuLinkArguments(const Options &options,
                                                  const Toolchain &toolchain,
                                                  const Path &output,
                                                  const std::vector<Path> &objects)
        {
            std::vector<std::string> args;
            args.push_back(pathText(toolchain.cxx));
            const std::vector<std::string> arch = archFlags(toolchain);
            args.insert(args.end(), arch.begin(), arch.end());
            if (options.config == Config::Debug)
                args.push_back("-g");
            if (options.sanitize)
                appendGnuSanitizers(args);
            if (toolchain.msvcAbi && !toolchain.lldLink.empty())
                args.push_back("-fuse-ld=lld");
            args.push_back("-o");
            args.push_back(pathText(output));
            appendObjects(args, objects);
            return args;
        }

        std::vector<std::string> linkArguments(const Options &options, const Toolchain &toolchain,
                                               const Path &output,
                                               const std::vector<Path> &objects)
        {
            return toolchain.family == Family::Msvc
                       ? msvcLinkArguments(options, toolchain, output, objects)
                       : gnuLinkArguments(options, toolchain, output, objects);
        }

        Path buildRoot(const Options &options, const Toolchain &toolchain)
        {
            return options.buildDir / pathFromText(comboName(options, toolchain));
        }

        Path privateRoot(const Options &options, const Toolchain &toolchain)
        {
            return buildRoot(options, toolchain) / ".builder";
        }

        Path projectRelative(const Path &root, const Path &path)
        {
            std::error_code ec;
            const Path relative = std::filesystem::relative(path, root, ec);
            if (ec || relative.empty())
                return path.filename();
            return relative;
        }

        Path objectPath(const Options &options, const Toolchain &toolchain, const Target &target,
                        const Path &root, const Path &source)
        {
            Path object = privateRoot(options, toolchain) / "obj" / pathFromText(target.name) /
                          projectRelative(root, source);
            object.replace_extension(".obj");
            return object;
        }

        Path depPath(const Options &options, const Toolchain &toolchain, const Target &target,
                     const Path &root, const Path &source)
        {
            Path dep = privateRoot(options, toolchain) / "dep" / pathFromText(target.name) /
                       projectRelative(root, source);
            dep.replace_extension(".d");
            return dep;
        }

        Path statePath(const Options &options, const Toolchain &toolchain, const Target &target,
                       const Path &root, const Path &source)
        {
            Path state = privateRoot(options, toolchain) / "state" / pathFromText(target.name) /
                         projectRelative(root, source);
            state.replace_extension(".state");
            return state;
        }

        // ============================================================================
        // Persistent build state (object and link-step fingerprints)
        // ============================================================================

        bool readDigestLine(std::istringstream &fields, DepDigest &digest)
        {
            std::string token;
            if (!(fields >> token >> digest.size) || !parseHex64(token, digest.hash))
                return false;
            std::string rest;
            std::getline(fields, rest);
            if (!rest.empty() && rest.front() == ' ')
                rest.erase(rest.begin());
            if (rest.empty())
                return false;
            digest.path = pathFromText(rest);
            return true;
        }

        template <typename Handler>
        bool readStateFile(const Path &file, const char *header, Handler handler)
        {
            std::string text;
            if (!readTextFile(file, text))
                return false;
            std::istringstream input(text);
            std::string line;
            bool headerSeen = false;
            while (std::getline(input, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;
                if (!headerSeen)
                {
                    if (line != header)
                        return false;
                    headerSeen = true;
                    continue;
                }
                std::istringstream fields(line);
                std::string key;
                fields >> key;
                if (!key.empty())
                    handler(key, fields);
            }
            return headerSeen;
        }

        ObjectState loadObjectState(const Path &file)
        {
            ObjectState state;
            bool seenCxx = false;
            bool seenFlags = false;
            bool seenSource = false;
            bool seenDepsKnown = false;
            const bool header = readStateFile(
                file, OBJECT_STATE_HEADER,
                [&](const std::string &key, std::istringstream &fields)
                {
                    if (key == "cxx")
                    {
                        std::string token;
                        if (fields >> token && parseHex64(token, state.cxxHash))
                            seenCxx = true;
                    }
                    else if (key == "flags")
                    {
                        std::string token;
                        if (fields >> token && parseHex64(token, state.flagsHash))
                            seenFlags = true;
                    }
                    else if (key == "source")
                    {
                        std::string token;
                        if (fields >> token >> state.sourceSize &&
                            parseHex64(token, state.sourceHash))
                            seenSource = true;
                    }
                    else if (key == "deps-known")
                    {
                        std::string value;
                        if (fields >> value)
                        {
                            state.depsKnown = value == "yes";
                            seenDepsKnown = true;
                        }
                    }
                    else if (key == "dep")
                    {
                        DepDigest dep;
                        if (readDigestLine(fields, dep))
                            state.deps.push_back(std::move(dep));
                    }
                });
            state.valid = header && seenCxx && seenFlags && seenSource && seenDepsKnown;
            return state;
        }

        bool saveObjectState(const Path &file, const ObjectState &state, std::string &error)
        {
            std::vector<DepDigest> deps = state.deps;
            std::sort(deps.begin(), deps.end(),
                      [](const DepDigest &left, const DepDigest &right)
                      { return pathText(left.path) < pathText(right.path); });
            std::ostringstream out;
            out << OBJECT_STATE_HEADER << "\n";
            out << "cxx " << hex64(state.cxxHash) << "\n";
            out << "flags " << hex64(state.flagsHash) << "\n";
            out << "source " << hex64(state.sourceHash) << ' ' << state.sourceSize << "\n";
            out << "deps-known " << (state.depsKnown ? "yes" : "no") << "\n";
            for (const DepDigest &dep : deps)
                out << "dep " << hex64(dep.hash) << ' ' << dep.size << ' ' << pathText(dep.path)
                    << "\n";
            return writeTextFileAtomic(file, out.str(), error);
        }
        StepState loadStepState(const Path &file)
        {
            StepState state;
            bool seenArgs = false;
            bool seenOutput = false;
            const bool header = readStateFile(
                file, STEP_STATE_HEADER,
                [&](const std::string &key, std::istringstream &fields)
                {
                    if (key == "args")
                    {
                        std::string token;
                        if (fields >> token && parseHex64(token, state.argsHash))
                            seenArgs = true;
                    }
                    else if (key == "output")
                    {
                        std::string token;
                        if (fields >> token >> state.outputSize &&
                            parseHex64(token, state.outputHash))
                            seenOutput = true;
                    }
                    else if (key == "in")
                    {
                        DepDigest inputFile;
                        if (readDigestLine(fields, inputFile))
                            state.inputs.push_back(std::move(inputFile));
                    }
                });
            state.valid = header && seenArgs && seenOutput;
            return state;
        }

        bool saveStepState(const Path &file, std::uint64_t argsHash, const Path &output,
                           const std::vector<Path> &inputs, std::string &error)
        {
            std::ostringstream out;
            out << STEP_STATE_HEADER << "\n";
            out << "args " << hex64(argsHash) << "\n";
            const FileDigest outputDigest = digestFile(output);
            if (outputDigest.ok)
                out << "output " << hex64(outputDigest.hash) << ' ' << outputDigest.size << "\n";
            for (const Path &input : inputs)
            {
                const FileDigest digest = digestFile(input);
                if (!digest.ok)
                    continue;
                out << "in " << hex64(digest.hash) << ' ' << digest.size << ' '
                    << pathText(input) << "\n";
            }
            return writeTextFileAtomic(file, out.str(), error);
        }

        // ============================================================================
        // Dependency scanning and incremental up-to-date checks
        // ============================================================================

        std::vector<Path> parseDepFile(const Path &file, const Path &root, bool &known)
        {
            std::vector<Path> dependencies;
            known = false;
            std::string text;
            if (!readTextFile(file, text) || text.empty())
                return dependencies;

            std::string joined;
            joined.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '\\' && i + 1 < text.size() &&
                    (text[i + 1] == '\n' || text[i + 1] == '\r'))
                {
                    if (text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n')
                        ++i;
                    ++i;
                    continue;
                }
                joined.push_back(text[i]);
            }

            std::vector<std::string> tokens;
            std::string current;
            for (std::size_t i = 0; i < joined.size(); ++i)
            {
                const char c = joined[i];
                if (c == '\\' && i + 1 < joined.size() &&
                    (joined[i + 1] == ' ' || joined[i + 1] == '#'))
                {
                    current.push_back(joined[i + 1]);
                    ++i;
                    continue;
                }
                if (c == '\\' && i + 1 < joined.size() && joined[i + 1] == '\\')
                {
                    current.push_back('\\');
                    ++i;
                    continue;
                }
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                    if (!current.empty())
                    {
                        tokens.push_back(current);
                        current.clear();
                    }
                    continue;
                }
                current.push_back(c);
            }
            if (!current.empty())
                tokens.push_back(current);
            if (tokens.empty())
                return dependencies;

            known = true;
            for (std::size_t i = 1; i < tokens.size(); ++i)
            {
                std::string token = tokens[i];
                if (!token.empty() && token.back() == ':')
                    token.pop_back();
                if (token.empty())
                    continue;
                Path path = pathFromText(token);
                if (path.is_relative())
                    path = root / path;
                dependencies.push_back(path);
            }
            return dependencies;
        }
        bool scanIncludes(const Path &file, const std::vector<Path> &includes,
                          std::unordered_set<std::string> &visited, std::vector<Path> &out)
        {
            std::error_code canonicalError;
            const Path canonical = std::filesystem::weakly_canonical(file, canonicalError);
            const std::string key = pathText(canonicalError ? file : canonical);
            if (!visited.insert(key).second)
                return true;

            std::string text;
            if (!readTextFile(file, text))
                return false;

            bool complete = true;
            bool inBlockComment = false;
            std::istringstream input(text);
            std::string line;
            while (std::getline(input, line))
            {
                std::string cleaned;
                cleaned.reserve(line.size());
                for (std::size_t i = 0; i < line.size(); ++i)
                {
                    const char c = line[i];
                    if (inBlockComment)
                    {
                        if (c == '*' && i + 1 < line.size() && line[i + 1] == '/')
                        {
                            inBlockComment = false;
                            ++i;
                        }
                        continue;
                    }
                    if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
                        break;
                    if (c == '/' && i + 1 < line.size() && line[i + 1] == '*')
                    {
                        inBlockComment = true;
                        ++i;
                        continue;
                    }
                    cleaned.push_back(c);
                }

                const std::size_t marker = cleaned.find('#');
                if (marker == std::string::npos)
                    continue;
                std::size_t cursor = marker + 1;
                while (cursor < cleaned.size() &&
                       (cleaned[cursor] == ' ' || cleaned[cursor] == '\t'))
                    ++cursor;
                if (cleaned.compare(cursor, 7, "include") != 0)
                    continue;
                cursor += 7;
                while (cursor < cleaned.size() &&
                       (cleaned[cursor] == ' ' || cleaned[cursor] == '\t'))
                    ++cursor;
                if (cursor >= cleaned.size())
                    continue;
                const char opener = cleaned[cursor];
                if (opener != '"' && opener != '<')
                    continue;
                const char closer = opener == '"' ? '"' : '>';
                const std::size_t first = cursor + 1;
                const std::size_t last = cleaned.find(closer, first);
                if (last == std::string::npos)
                    continue;
                const std::string name = cleaned.substr(first, last - first);
                const bool angleInclude = opener == '<';

                Path resolved;
                if (!angleInclude)
                {
                    const Path local = file.parent_path() / pathFromText(name);
                    if (fileExists(local))
                        resolved = local;
                }
                if (resolved.empty())
                {
                    for (const Path &include : includes)
                    {
                        const Path candidate = include / pathFromText(name);
                        if (fileExists(candidate))
                        {
                            resolved = candidate;
                            break;
                        }
                    }
                }
                if (resolved.empty())
                {
                    // Unresolved <...> is assumed to be a system header. Unresolved "..."
                    // means the dependency set is incomplete and the object must not be
                    // treated as up to date.
                    if (!angleInclude)
                        complete = false;
                    continue;
                }
                out.push_back(resolved);
                if (!scanIncludes(resolved, includes, visited, out))
                    complete = false;
            }
            return complete;
        }

        void collectDependencies(const CompileUnit &unit, const Path &root, bool &known,
                                 std::vector<Path> &dependencies)
        {
            known = false;
            if (!unit.depFile.empty())
                dependencies = parseDepFile(unit.depFile, root, known);
            if (known)
                return;
            std::unordered_set<std::string> visited;
            std::vector<Path> scanned;
            known = scanIncludes(unit.source, unit.target->includes, visited, scanned);
            dependencies = std::move(scanned);
        }
        bool makeObjectState(const CompileUnit &unit, const Path &root, ObjectState &state,
                             std::string &error)
        {
            state = ObjectState();
            state.cxxHash = unit.cxxHash;
            state.flagsHash = unit.flagsHash;
            const FileDigest source = digestFile(unit.source);
            if (!source.ok)
            {
                error = "cannot hash '" + pathText(unit.source) + "'";
                return false;
            }
            state.sourceHash = source.hash;
            state.sourceSize = source.size;

            bool known = false;
            std::vector<Path> dependencies;
            collectDependencies(unit, root, known, dependencies);
            std::sort(dependencies.begin(), dependencies.end(),
                      [](const Path &left, const Path &right)
                      { return pathText(left) < pathText(right); });
            dependencies.erase(std::unique(dependencies.begin(), dependencies.end(),
                                           [](const Path &left, const Path &right)
                                           { return pathText(left) == pathText(right); }),
                               dependencies.end());
            state.depsKnown = known;
            for (const Path &dependency : dependencies)
            {
                const FileDigest digest = digestFile(dependency);
                if (!digest.ok)
                {
                    state.depsKnown = false;
                    continue;
                }
                state.deps.push_back(DepDigest{dependency, digest.hash, digest.size});
            }
            return true;
        }

        bool objectUpToDate(const CompileUnit &unit, std::string &reason)
        {
            if (!fileExists(unit.object))
            {
                reason = "object missing";
                return false;
            }
            const ObjectState state = loadObjectState(unit.stateFile);
            if (!state.valid)
            {
                reason = "no usable state file";
                return false;
            }
            if (!state.depsKnown)
            {
                reason = "dependencies unknown";
                return false;
            }
            if (state.cxxHash != unit.cxxHash)
            {
                reason = "compiler changed";
                return false;
            }
            if (state.flagsHash != unit.flagsHash)
            {
                reason = "flags changed";
                return false;
            }
            const FileDigest source = digestFile(unit.source);
            if (!source.ok || source.hash != state.sourceHash || source.size != state.sourceSize)
            {
                reason = "source changed";
                return false;
            }
            for (const DepDigest &dependency : state.deps)
            {
                const FileDigest digest = digestFile(dependency.path);
                if (!digest.ok || digest.hash != dependency.hash ||
                    digest.size != dependency.size)
                {
                    reason = "dependency changed";
                    return false;
                }
            }
            return true;
        }

        bool stepUpToDate(const Path &output, const Path &stateFile, std::uint64_t argsHash,
                          const std::vector<Path> &inputs, std::string &reason)
        {
            if (!fileExists(output))
            {
                reason = "output missing";
                return false;
            }
            const StepState state = loadStepState(stateFile);
            if (!state.valid)
            {
                reason = "no usable state file";
                return false;
            }
            if (state.argsHash != argsHash)
            {
                reason = "command line changed";
                return false;
            }
            const FileDigest outputDigest = digestFile(output);
            if (!outputDigest.ok || outputDigest.hash != state.outputHash ||
                outputDigest.size != state.outputSize)
            {
                reason = "output changed";
                return false;
            }
            if (state.inputs.size() != inputs.size())
            {
                reason = "input set changed";
                return false;
            }
            std::unordered_map<std::string, DepDigest> recorded;
            for (const DepDigest &inputFile : state.inputs)
                recorded[pathText(inputFile.path)] = inputFile;
            for (const Path &input : inputs)
            {
                const auto found = recorded.find(pathText(input));
                if (found == recorded.end())
                {
                    reason = "input set changed";
                    return false;
                }
                const FileDigest digest = digestFile(input);
                if (!digest.ok || digest.hash != found->second.hash ||
                    digest.size != found->second.size)
                {
                    reason = "input changed";
                    return false;
                }
            }
            return true;
        }

        // ============================================================================
        // Build engine: target selection, preparation and task execution
        // ============================================================================

        const Target *findTargetByName(const Project &project, const std::string &name)
        {
            for (const Target &target : project.targets)
                if (target.name == name)
                    return &target;
            return nullptr;
        }

        bool addTargetByName(const Project &project, const std::string &name,
                             std::vector<const Target *> &selected)
        {
            const Target *found = nullptr;
            for (const Target &target : project.targets)
            {
                if (!target.internal && target.name == name)
                {
                    found = &target;
                    break;
                }
            }
            if (found == nullptr)
                return false;
            for (const Target *existing : selected)
            {
                if (existing == found)
                    return true;
            }
            selected.push_back(found);
            return true;
        }

        bool addTargetClosure(const Project &project, const Target &target,
                              std::vector<const Target *> &targets, std::string &error)
        {
            for (const Target *existing : targets)
                if (existing->name == target.name)
                    return true;
            targets.push_back(&target);
            for (const std::string &link : target.links)
            {
                const Target *dependency = findTargetByName(project, link);
                if (dependency == nullptr)
                {
                    error = "target '" + target.name + "' links unknown target '" + link + "'";
                    return false;
                }
                if (!addTargetClosure(project, *dependency, targets, error))
                    return false;
            }
            return true;
        }

        std::vector<std::string> compileArguments(const Options &options,
                                                  const Toolchain &toolchain,
                                                  const Target &target, const CompileUnit &unit)
        {
            std::vector<std::string> args;
            args.push_back(pathText(toolchain.cxx));
            const std::vector<std::string> flags = compileFlags(options, toolchain);
            args.insert(args.end(), flags.begin(), flags.end());
            if (toolchain.family == Family::Msvc)
            {
                for (const Path &include : target.includes)
                    args.push_back("/I" + pathText(include));
                args.push_back("/Fo" + pathText(unit.object));
                args.push_back(pathText(unit.source));
                return args;
            }
            for (const Path &include : target.includes)
                args.push_back("-I" + pathText(include));
            if (toolchain.depFiles && !unit.depFile.empty())
            {
                args.push_back("-MMD");
                args.push_back("-MF");
                args.push_back(pathText(unit.depFile));
            }
            args.push_back("-c");
            args.push_back(pathText(unit.source));
            args.push_back("-o");
            args.push_back(pathText(unit.object));
            return args;
        }

        bool prepareCompileUnit(const Options &options, const Toolchain &toolchain,
                                const Project &project, const Target &target, const Path &source,
                                CompileUnit &unit, std::string &error)
        {
            unit.target = &target;
            unit.source = source;
            unit.object = objectPath(options, toolchain, target, project.root, source);
            unit.depFile = toolchain.depFiles
                               ? depPath(options, toolchain, target, project.root, source)
                               : Path();
            unit.stateFile = statePath(options, toolchain, target, project.root, source);
            unit.display = displayPath(source, project.root);
            unit.logFile =
                privateRoot(options, toolchain) / "logs" /
                pathFromText("compile-" + sanitizeForFile(target.name + "-" + unit.display) +
                             ".log");
            if (!ensureDirectory(unit.object.parent_path(), error) ||
                !ensureDirectory(unit.stateFile.parent_path(), error) ||
                !ensureDirectory(unit.logFile.parent_path(), error))
                return false;
            if (!unit.depFile.empty() && !ensureDirectory(unit.depFile.parent_path(), error))
                return false;
            unit.args = compileArguments(options, toolchain, target, unit);
            unit.cxxHash = hashText(pathText(toolchain.cxx) + "\n" + toolchain.versionLine + "\n" +
                                    toolchain.triple + "\n" + archName(toolchain.arch));
            std::vector<std::string> signature = compileFlags(options, toolchain);
            for (const Path &include : target.includes)
                signature.push_back("-I" + pathText(include));
            unit.flagsHash = hashArgs(signature);
            return true;
        }

        int selectTargets(const Project &project, const Options &options,
                          std::vector<const Target *> &selected)
        {
            std::vector<std::string> requested = options.targets;
            if (options.command == Command::Self)
                requested = {TARGET_BUILDER};
            if (requested.empty())
                requested = {TARGET_ALL};

            for (const std::string &name : requested)
            {
                if (name == TARGET_ALL)
                {
                    for (const Target &target : project.targets)
                    {
                        if (!target.internal && target.name != TARGET_BUILDER)
                            addTargetByName(project, target.name, selected);
                    }
                    continue;
                }
                if (!addTargetByName(project, name, selected))
                {
                    std::cerr << "error: unknown target '" << name
                              << "' (try 'builder targets')\n";
                    return USAGE;
                }
            }
            return OK;
        }

        int prepareCompileUnits(const Options &options, const Toolchain &toolchain,
                                const Project &project, const std::vector<const Target *> &targets,
                                std::vector<CompileUnit> &units)
        {
            for (const Target *target : targets)
            {
                for (const Path &source : target->sources)
                {
                    if (!fileExists(source))
                    {
                        std::cerr << "error: source file is missing: '" << pathText(source)
                                  << "'\n";
                        return BUILD_FAILED;
                    }
                    CompileUnit unit;
                    std::string error;
                    if (!prepareCompileUnit(options, toolchain, project, *target, source, unit,
                                            error))
                    {
                        std::cerr << "error: " << error << "\n";
                        return ENVIRONMENT;
                    }
                    units.push_back(std::move(unit));
                }
            }
            return OK;
        }

        int markCompileUnits(std::vector<CompileUnit> &units, const Options &options)
        {
            int workCount = 0;
            for (CompileUnit &unit : units)
            {
                std::string reason;
                unit.needsBuild = options.force || !objectUpToDate(unit, reason);
                if (unit.needsBuild)
                {
                    ++workCount;
                    if (options.verbose)
                        std::cout << "note: " << unit.display << ": " << reason << "\n";
                }
                else if (options.verbose)
                {
                    std::cout << "note: " << unit.display << ": cached\n";
                }
            }
            return workCount;
        }

        void collectLinkObjects(const Options &options, const Toolchain &toolchain,
                                const Project &project, const Target &target,
                                std::vector<Path> &objects, std::vector<std::string> &seen)
        {
            for (const std::string &name : seen)
                if (name == target.name)
                    return;
            seen.push_back(target.name);
            for (const Path &source : target.sources)
                objects.push_back(objectPath(options, toolchain, target, project.root, source));
            for (const std::string &link : target.links)
            {
                const Target *dependency = findTargetByName(project, link);
                if (dependency != nullptr)
                    collectLinkObjects(options, toolchain, project, *dependency, objects, seen);
            }
        }

        bool replaceRunningExecutable(const Path &fresh, const Path &destination,
                                      std::string &error);

        int prepareLinkSteps(const Options &options, const Toolchain &toolchain,
                             const Project &project, const std::vector<const Target *> &selected,
                             const Path &logsDir, std::vector<BuildStep> &steps,
                             int &workCount)
        {
            const Path stateDir = privateRoot(options, toolchain) / "state";
            for (const Target *target : selected)
            {
                BuildStep step;
                step.label = "LINK";
                step.finalOutput = target->output;
                step.output = target->output;
                step.replaceSelf = target->name == TARGET_BUILDER;
                if (step.replaceSelf)
                    step.output += pathFromText(".new");
                step.display = displayPath(step.finalOutput, options.buildDir);

                std::vector<Path> objects;
                std::vector<std::string> seenTargets;
                collectLinkObjects(options, toolchain, project, *target, objects, seenTargets);
                step.inputs = objects;
                step.args = linkArguments(options, toolchain, step.output, objects);
                step.argsHash = hashArgs(step.args);
                step.stateFile = stateDir / pathFromText(target->name + ".link.state");
                step.logFile = logsDir / pathFromText(target->name + "-link.log");

                std::string error;
                if (!ensureDirectory(step.output.parent_path(), error))
                {
                    std::cerr << "error: " << error << "\n";
                    return ENVIRONMENT;
                }
                std::string reason;
                const Path &checkedOutput = step.replaceSelf ? step.finalOutput : step.output;
                step.needsBuild =
                    options.force ||
                    !stepUpToDate(checkedOutput, step.stateFile, step.argsHash, step.inputs,
                                  reason);
                if (step.needsBuild)
                    ++workCount;
                steps.push_back(std::move(step));
            }
            return OK;
        }

        void propagateRebuiltInputs(const Options &options,
                                    const std::vector<CompileUnit> &units,
                                    std::vector<BuildStep> &steps, int &workCount)
        {
            std::unordered_set<std::string> rebuilt;
            for (const CompileUnit &unit : units)
            {
                if (unit.built)
                    rebuilt.insert(pathText(unit.object));
            }
            for (BuildStep &step : steps)
            {
                if (!step.needsBuild)
                {
                    for (const Path &input : step.inputs)
                    {
                        if (rebuilt.find(pathText(input)) != rebuilt.end())
                        {
                            step.needsBuild = true;
                            ++workCount;
                            break;
                        }
                    }
                }
                if (step.needsBuild)
                    rebuilt.insert(pathText(step.finalOutput));
            }
            if (options.verbose)
            {
                for (const BuildStep &step : steps)
                {
                    if (!step.needsBuild)
                        std::cout << "note: " << step.label << " " << step.display
                                  << ": cached\n";
                }
            }
        }
        bool runCompilePhase(const Options &options, const Project &project,
                             std::vector<CompileUnit> &units, int workCount,
                             std::atomic<int> &completed, std::string &firstError)
        {
            if (units.empty())
                return true;

            std::atomic<std::size_t> nextUnit{0};
            std::atomic<bool> failed{false};
            std::mutex outputMutex;

            const auto worker = [&]()
            {
                for (;;)
                {
                    if (failed.load())
                        return;
                    const std::size_t index = nextUnit.fetch_add(1);
                    if (index >= units.size())
                        return;
                    CompileUnit &unit = units[index];
                    if (!unit.needsBuild)
                        continue;

                    if (options.dryRun)
                    {
                        std::lock_guard<std::mutex> lock(outputMutex);
                        unit.built = true;
                        const int doneIndex = completed.fetch_add(1) + 1;
                        std::cout << "[" << doneIndex << "/" << workCount << "] CXX   "
                                  << unit.display << "\n";
                        std::cout << "        " << formatCommand(unit.args) << "\n";
                        std::cout.flush();
                        continue;
                    }

                    const ProcessResult result =
                        runProcess(unit.args, unit.logFile, project.root);
                    if (!result.started || result.exitCode != 0)
                    {
                        removeQuiet(unit.object);
                        removeQuiet(unit.stateFile);
                        std::string log;
                        readTextFile(unit.logFile, log);
                        std::lock_guard<std::mutex> lock(outputMutex);
                        if (!failed.exchange(true))
                        {
                            std::ostringstream report;
                            report << "error: compiling '" << unit.display << "' failed";
                            if (!result.started)
                                report << ": " << result.spawnError;
                            else
                                report << " (exit " << result.exitCode << ")";
                            report << "\n";
                            if (!log.empty())
                                report << log << (log.back() == '\n' ? "" : "\n");
                            firstError = report.str();
                        }
                        return;
                    }

                    ObjectState state;
                    std::string stateError;
                    bool stateOk =
                        makeObjectState(unit, project.root, state, stateError);
                    if (stateOk)
                        stateOk = saveObjectState(unit.stateFile, state, stateError);
                    if (!stateOk)
                    {
                        removeQuiet(unit.stateFile);
                        std::lock_guard<std::mutex> lock(outputMutex);
                        std::cerr << "warning: " << stateError << "\n";
                    }
                    unit.built = true;

                    std::lock_guard<std::mutex> lock(outputMutex);
                    const int doneIndex = completed.fetch_add(1) + 1;
                    std::cout << "[" << doneIndex << "/" << workCount << "] CXX   "
                              << unit.display << "\n";
                    std::cout.flush();
                }
            };

            int threadCount = options.jobs > 0 ? options.jobs : 1;
            if (static_cast<std::size_t>(threadCount) > units.size())
                threadCount = static_cast<int>(units.size());
            if (threadCount <= 1)
            {
                worker();
            }
            else
            {
                std::vector<std::thread> threads;
                threads.reserve(static_cast<std::size_t>(threadCount));
                for (int i = 0; i < threadCount; ++i)
                    threads.emplace_back(worker);
                for (std::thread &thread : threads)
                    thread.join();
            }
            return !failed.load();
        }

        bool runBuildSteps(const Options &options, const Project &project,
                           std::vector<BuildStep> &steps, std::atomic<int> &completed,
                           int workCount)
        {
            for (BuildStep &step : steps)
            {
                if (!step.needsBuild)
                    continue;

                const int doneIndex = completed.fetch_add(1) + 1;
                std::string prefix = step.label;
                while (prefix.size() < 6)
                    prefix.push_back(' ');
                std::cout << "[" << doneIndex << "/" << workCount << "] " << prefix
                          << step.display << "\n";
                if (options.dryRun)
                {
                    std::cout << "        " << formatCommand(step.args) << "\n";
                    std::cout.flush();
                    continue;
                }
                std::cout.flush();

                const ProcessResult result =
                    runProcess(step.args, step.logFile, project.root);
                if (!result.started || result.exitCode != 0)
                {
                    std::string log;
                    readTextFile(step.logFile, log);
                    std::cerr << "error: linking '" << step.display << "' failed";
                    if (!result.started)
                        std::cerr << ": " << result.spawnError;
                    else
                        std::cerr << " (exit " << result.exitCode << ")";
                    std::cerr << "\n";
                    if (!log.empty())
                        std::cerr << log << (log.back() == '\n' ? "" : "\n");
                    return false;
                }

                if (step.replaceSelf)
                {
                    std::string replaceError;
                    if (!replaceRunningExecutable(step.output, step.finalOutput, replaceError))
                    {
                        std::cerr << "error: " << replaceError << "\n";
                        std::cerr << "note: the new binary is available at '"
                                  << pathText(step.output) << "'\n";
                        return false;
                    }
                }

                const Path &stateOutput = step.replaceSelf ? step.finalOutput : step.output;
                std::string stateError;
                if (!saveStepState(step.stateFile, step.argsHash, stateOutput, step.inputs,
                                   stateError))
                {
                    std::cerr << "warning: " << stateError << "\n";
                }
            }
            return true;
        }

        void printBuildBanner(const Options &options, const Toolchain &toolchain)
        {
            std::cout << "builder " << BUILDER_VERSION << " | "
                      << toolchainName(toolchain.family) << " " << archName(toolchain.arch)
                      << " " << configName(options.config) << " | " << toolchain.versionLine
                      << " | jobs=" << options.jobs
                      << (options.dryRun ? " | dry-run" : "") << "\n";
        }

        void printBuildSummary(const Options &options, int workCount,
                               const std::chrono::steady_clock::time_point &startTime)
        {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime)
                    .count();
            std::cout << (options.dryRun ? "dry-run: " : "build ok: ") << workCount
                      << " task(s)";
            if (!options.dryRun)
                std::cout << " in " << std::fixed << std::setprecision(2) << elapsed << "s";
            std::cout << "\n";
        }

        int runBuild(const Options &options, const Toolchain &toolchain)
        {
            Project project;
            std::string projectError;
            if (!makeProject(options.root, options.buildDir, options, toolchain, project,
                             projectError))
            {
                std::cerr << "error: " << projectError << "\n";
                return BUILD_FAILED;
            }

            std::vector<const Target *> selected;
            const int selectResult = selectTargets(project, options, selected);
            if (selectResult != OK)
                return selectResult;

            std::vector<const Target *> buildTargets;
            for (const Target *target : selected)
            {
                std::string closureError;
                if (!addTargetClosure(project, *target, buildTargets, closureError))
                {
                    std::cerr << "error: " << closureError << "\n";
                    return BUILD_FAILED;
                }
            }

            const Path logsDir = privateRoot(options, toolchain) / "logs";
            std::string setupError;
            if (!ensureDirectory(logsDir, setupError) ||
                !ensureDirectory(buildRoot(options, toolchain) / "bin", setupError))
            {
                std::cerr << "error: " << setupError << "\n";
                return ENVIRONMENT;
            }

            std::vector<CompileUnit> units;
            const int unitResult =
                prepareCompileUnits(options, toolchain, project, buildTargets, units);
            if (unitResult != OK)
                return unitResult;

            int workCount = markCompileUnits(units, options);

            std::vector<BuildStep> steps;
            const int stepResult =
                prepareLinkSteps(options, toolchain, project, selected, logsDir, steps, workCount);
            if (stepResult != OK)
                return stepResult;

            printBuildBanner(options, toolchain);
            if (workCount == 0)
            {
                std::cout << "build up to date (nothing to do)\n";
                return OK;
            }
            const auto startTime = std::chrono::steady_clock::now();
            std::atomic<int> completed{0};
            std::string compileError;
            if (!runCompilePhase(options, project, units, workCount, completed, compileError))
            {
                if (!compileError.empty())
                    std::cerr << compileError;
                return BUILD_FAILED;
            }
            propagateRebuiltInputs(options, units, steps, workCount);
            if (!runBuildSteps(options, project, steps, completed, workCount))
                return BUILD_FAILED;
            printBuildSummary(options, workCount, startTime);
            return OK;
        }

        // ============================================================================
        // Commands: rebuild/clean and self-update support
        // ============================================================================

        bool replaceRunningExecutable(const Path &fresh, const Path &destination,
                                      std::string &error)
        {
            std::error_code ec;
            if (!fileExists(fresh))
            {
                error = "new executable is missing: '" + pathText(fresh) + "'";
                return false;
            }
            Path backup = destination;
            backup += pathFromText(".old");
            std::filesystem::remove(backup, ec);
            ec.clear();
            if (fileExists(destination))
            {
                std::filesystem::rename(destination, backup, ec);
                if (ec)
                {
                    error = "cannot move the running executable aside ('" +
                            pathText(destination) + "': " + ec.message() + ")";
                    return false;
                }
            }
            std::filesystem::rename(fresh, destination, ec);
            if (ec)
            {
                std::error_code restoreError;
                if (fileExists(backup))
                    std::filesystem::rename(backup, destination, restoreError);
                error = "cannot install the new executable ('" + pathText(destination) +
                        "': " + ec.message() + ")";
                return false;
            }
            std::filesystem::remove(backup, ec);
            return true;
        }

        void configureTempDirectory(const Path &tempDir)
        {
            std::string error;
            if (!ensureDirectory(tempDir, error))
                return;
            platform::setTempEnvironment(tempDir);
        }

        bool usesMsvcCompiler(const Options &options)
        {
            if (options.compiler == CompilerChoice::Msvc)
                return true;
            if (!options.cxx.empty())
            {
                const std::string base = lowerAscii(pathText(options.cxx.filename()));
                return base == "cl" || base == "cl.exe";
            }
            return false;
        }

        int runClean(const Options &options)
        {
            int removed = 0;
            std::error_code ec;
            const auto removeTree = [&](const Path &path)
            {
                if (!directoryExists(path))
                    return;
                std::error_code treeError;
                std::filesystem::remove_all(path, treeError);
                if (!treeError)
                    ++removed;
                else
                    ec = treeError;
            };
            static const char *toolchains[] = {"clang", "gcc", "msvc"};
            static const char *architectures[] = {"x86", "x64"};
            static const char *configurations[] = {"debug", "release"};
            for (const char *toolchain : toolchains)
                for (const char *architecture : architectures)
                    for (const char *configuration : configurations)
                        removeTree(options.buildDir / pathFromText(std::string(toolchain) + "-" +
                                                                   architecture + "-" +
                                                                   configuration));
            // Legacy private tree from the previous single-target builder.
            removeTree(options.buildDir / pathFromText(TARGET_BUILDER));
            removeTree(options.buildDir / ".builder");
            removeQuiet(options.buildDir / "reexec-x86.cmd");
            removeQuiet(options.buildDir / "reexec-x64.cmd");
            if (ec)
            {
                std::cerr << "error: cannot clean the build tree: " << ec.message() << "\n";
                return BUILD_FAILED;
            }
            if (!options.currentExe.empty())
            {
                Path newExe = options.currentExe;
                newExe += pathFromText(".new");
                Path oldExe = options.currentExe;
                oldExe += pathFromText(".old");
                Path appendedPdb = options.currentExe;
                appendedPdb += pathFromText(".pdb");
                Path replacedPdb = options.currentExe;
                replacedPdb.replace_extension(".pdb");
                removeQuiet(newExe);
                removeQuiet(oldExe);
                removeQuiet(appendedPdb);
                removeQuiet(replacedPdb);
            }
            std::cout << "clean: removed " << removed << " item(s)\n";
            return OK;
        }

        // ============================================================================
        // Commands: doctor and targets
        // ============================================================================

        int runDoctor(const Options &options, const Toolchain &toolchain)
        {
            Project project;
            std::string error;
            if (!makeProject(options.root, options.buildDir, options, toolchain, project, error))
            {
                std::cerr << "error: " << error << "\n";
                return BUILD_FAILED;
            }

            std::size_t sourceCount = 0;
            for (const Target &target : project.targets)
                sourceCount += target.sources.size();

            std::cout << "builder " << BUILDER_VERSION << " (MoteVM " << project.version
                      << ", MOTEBC " << project.formatVersion << ")\n";
            std::cout << "root       " << pathText(project.root) << "\n";
            std::cout << "build      " << pathText(project.buildDir) << "\n";
            std::cout << "combo      " << comboName(options, toolchain) << "\n";
            std::cout << "output     " << pathText(buildRoot(options, toolchain)) << "\n";
            std::cout << "config     " << configName(options.config) << "\n";
            std::cout << "cxx        " << pathText(toolchain.cxx) << "\n";
            std::cout << "family     " << familyName(toolchain.family) << "\n";
            std::cout << "version    " << toolchain.versionLine << "\n";
            std::cout << "target     " << toolchain.triple << "\n";
            std::cout << "arch       " << archName(toolchain.arch) << "\n";
            std::cout << "abi        " << (toolchain.msvcAbi ? "msvc" : "gnu") << "\n";
            if (toolchain.family == Family::Msvc)
            {
                std::cout << "devshell   "
                          << (toolchain.devShellArch.empty() ? "unknown"
                                                             : toolchain.devShellArch)
                          << "\n";
                std::cout << "linker     " << pathText(toolchain.linker) << "\n";
                if (!toolchain.devShellArch.empty() &&
                    toolchain.devShellArch != archName(toolchain.arch))
                    std::cout << "note       build will re-exec via VsDevCmd.bat for "
                              << archName(toolchain.arch) << "\n";
            }
            else
            {
                std::cout << "linker     cxx driver\n";
            }
            if (toolchain.family != Family::Msvc && toolchain.msvcAbi &&
                !toolchain.devShellArch.empty() &&
                toolchain.devShellArch != archName(toolchain.arch))
                std::cout << "note       build will re-exec via VsDevCmd.bat for "
                          << archName(toolchain.arch) << "\n";
            std::cout << "jobs       " << options.jobs << "\n";
            std::cout << "targets    " << TARGET_ALL;
            for (const Target &target : project.targets)
            {
                if (!target.internal && target.name != TARGET_BUILDER)
                    std::cout << " " << target.name;
            }
            std::cout << " " << TARGET_BUILDER << "\n";
            std::cout << "shared    ";
            bool firstShared = true;
            for (const Target &target : project.targets)
            {
                if (!target.internal)
                    continue;
                if (!firstShared)
                    std::cout << " ";
                std::cout << target.name;
                firstShared = false;
            }
            std::cout << "\n";
            std::cout << "sources    " << sourceCount << "\n";
            return OK;
        }

        int runTargets(const Options &options, const Toolchain &toolchain)
        {
            Project project;
            std::string error;
            if (!makeProject(options.root, options.buildDir, options, toolchain, project, error))
            {
                std::cerr << "error: " << error << "\n";
                return BUILD_FAILED;
            }
            std::cout << TARGET_ALL << "      ";
            for (const Target &target : project.targets)
            {
                if (!target.internal && target.name != TARGET_BUILDER)
                    std::cout << target.name << " ";
            }
            std::cout << "\n";
            for (const Target &target : project.targets)
            {
                if (target.internal)
                    continue;
                std::string name = target.name;
                while (name.size() < 9)
                    name.push_back(' ');
                std::cout << name << displayPath(target.output, project.buildDir) << " ("
                          << target.sources.size() << " source(s))\n";
                std::cout << "         main: " << displayPath(target.mainFile, project.root)
                          << "\n";
                if (!target.links.empty())
                {
                    std::cout << "         links:";
                    for (const std::string &link : target.links)
                        std::cout << " " << link;
                    std::cout << "\n";
                }
            }
            std::cout << "shared:\n";
            for (const Target &target : project.targets)
            {
                if (!target.internal)
                    continue;
                std::cout << "  " << target.name << " (" << target.sources.size()
                          << " source(s))";
                if (!target.links.empty())
                {
                    std::cout << " ->";
                    for (const std::string &link : target.links)
                        std::cout << " " << link;
                }
                std::cout << "\n";
            }
            return OK;
        }

        // ============================================================================
        // CLI help and version output
        // ============================================================================

        void printVersion(const Options &options)
        {
            std::string version = "unknown";
            std::string format = "unknown";
            if (!options.root.empty())
            {
                version = readProjectValue(options.root, "MOTEVM_VERSION");
                format = readProjectValue(options.root, "MOTEBC_VERSION");
            }
            std::cout << "builder " << BUILDER_VERSION << " (MoteVM " << version << " / MOTEBC "
                      << format << ")\n";
        }

        Path searchProjectRoot(Path directory)
        {
            for (int depth = 0; depth < 32; ++depth)
            {
                if (directoryExists(directory / "src" / "motevm") &&
                    directoryExists(directory / "src" / TARGET_BUILDER))
                    return directory;
                const Path parent = directory.parent_path();
                if (parent.empty() || parent == directory)
                    break;
                directory = parent;
            }
            return Path();
        }

        Path findProjectRoot(const Path &executable)
        {
            std::error_code ec;
            const Path current = std::filesystem::current_path(ec);
            if (!ec)
            {
                const Path found = searchProjectRoot(current);
                if (!found.empty())
                    return found;
            }
            if (!executable.empty())
            {
                const Path found = searchProjectRoot(executable.parent_path());
                if (!found.empty())
                    return found;
            }
            return Path();
        }

        void printUsage(std::ostream &out)
        {
            out << "builder " << BUILDER_VERSION << " - MoteVM project build system\n\n";
            out << "usage: builder [command] [options] [target...]\n\n";
            out << "commands:\n";
            out << "  build      compile and link targets (default)\n";
            out << "  clean      remove builder outputs and state (never CMake files)\n";
            out << "  rebuild    clean, then build\n";
            out << "  self       rebuild builder itself\n";
            out << "  doctor     print toolchain and build configuration\n";
            out << "  targets    list targets and sources\n";
            out << "  help       show this help\n\n";
            out << "targets:\n";
            out << "  all        every module discovered under src/ (default)\n";
            out << "  <module>   build/<toolchain>-<arch>-<config>/bin/<module>[.exe]\n";
            out << "  builder    this tool (bin/builder[.exe])\n";
            out << "  shared     motevm_core/motevm_archive/motevm_tools are linked automatically\n\n";
            out << "options:\n";
            out << "  -j, --jobs N        parallel compile jobs (default: hardware concurrency)\n";
            out << "      --compiler X    clang|gcc|msvc (default: clang++ first)\n";
            out << "      --arch X        x86|x64 (default: host architecture)\n";
            out << "      --release       Release build (default)\n";
            out << "      --debug         Debug build\n";
            out << "      --cxx PATH      explicit C++ compiler (family detected from name)\n";
            out << "      --build-dir DIR output root (default: <root>/build)\n";
            out << "      --root DIR      project root (default: auto-detect)\n";
            out << "      --werror        treat warnings as errors\n";
            out << "      --sanitize      ASan + UBSan (MSVC: ASan only)\n";
            out << "      --force         rebuild everything\n";
            out << "  -n, --dry-run       print commands without running them\n";
            out << "  -v, --verbose       print cached tasks and full command lines\n";
            out << "      --version       print versions\n";
            out << "  -h, --help          show this help\n\n";
            out << "exit codes: 0 ok, 1 build failure, 2 usage, 3 environment/toolchain\n";
        }

        // ============================================================================
        // Option value parsing
        // ============================================================================

        bool startsWith(const std::string &text, const std::string &prefix)
        {
            return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
        }

        bool parsePositiveInt(const std::string &text, int &value)
        {
            if (text.empty())
                return false;
            std::size_t consumed = 0;
            long parsed = 0;
            try
            {
                parsed = std::stol(text, &consumed);
            }
            catch (...)
            {
                return false;
            }
            if (consumed != text.size() || parsed <= 0 || parsed > 1024)
                return false;
            value = static_cast<int>(parsed);
            return true;
        }

        std::optional<Command> commandFromWord(const std::string &word)
        {
            if (word == "build")
                return Command::Build;
            if (word == "clean")
                return Command::Clean;
            if (word == "rebuild")
                return Command::Rebuild;
            if (word == "self")
                return Command::Self;
            if (word == "doctor")
                return Command::Doctor;
            if (word == "targets")
                return Command::Targets;
            if (word == "help")
                return Command::Help;
            return std::nullopt;
        }

        bool parseCompilerChoice(const std::string &textValue, CompilerChoice &choice)
        {
            const std::string value = lowerAscii(textValue);
            if (value == "auto")
                choice = CompilerChoice::Auto;
            else if (value == "clang" || value == "clang++")
                choice = CompilerChoice::Clang;
            else if (value == "gcc" || value == "g++")
                choice = CompilerChoice::Gcc;
            else if (value == "msvc" || value == "cl")
                choice = CompilerChoice::Msvc;
            else
                return false;
            return true;
        }

        bool parseArchOption(const std::string &textValue, Arch &arch)
        {
            const std::string value = lowerAscii(textValue);
            if (value == "host" || value == "auto")
                arch = Arch::Host;
            else if (value == "x86" || value == "32" || value == "i686" || value == "win32")
                arch = Arch::X86;
            else if (value == "x64" || value == "64" || value == "x86_64" || value == "amd64")
                arch = Arch::X64;
            else
                return false;
            return true;
        }

        Path resolveAgainst(const Path &base, const Path &path)
        {
            std::error_code ec;
            const Path combined = path.is_absolute() ? path : base / path;
            const Path canonical = std::filesystem::weakly_canonical(combined, ec);
            if (!ec)
                return canonical;
            return combined;
        }

        // ============================================================================
        // Command-line parsing and entry point
        // ============================================================================

        void configureJobs(Options &options)
        {
            if (options.jobs != 0)
                return;
            const char *environment = std::getenv("MOTEVM_JOBS");
            int parsed = 0;
            if (environment != nullptr && parsePositiveInt(environment, parsed))
                options.jobs = parsed;
            if (options.jobs == 0)
            {
                const unsigned hardware = std::thread::hardware_concurrency();
                options.jobs =
                    hardware == 0 ? 4 : static_cast<int>(std::min<unsigned>(hardware, 64u));
            }
        }

        void resolveProjectPaths(Options &options, const char *argv0)
        {
            std::error_code cwdError;
            const Path currentDirectory = std::filesystem::current_path(cwdError);
            const Path base = cwdError ? Path(".") : currentDirectory;
            options.currentExe = currentExecutablePath(argv0);
            if (options.root.empty())
                options.root = findProjectRoot(options.currentExe);
            else
                options.root = resolveAgainst(base, options.root);
            if (options.root.empty())
                return;

            std::error_code rootError;
            const Path canonicalRoot =
                std::filesystem::weakly_canonical(options.root, rootError);
            if (!rootError)
                options.root = canonicalRoot;

            if (options.buildDir.empty())
            {
                const char *environment = std::getenv("MOTEVM_BUILD_DIR");
                if (environment != nullptr && *environment != '\0')
                    options.buildDir =
                        resolveAgainst(options.root, pathFromText(environment));
                else
                    options.buildDir = options.root / pathFromText("build");
            }
            else
            {
                options.buildDir = resolveAgainst(options.root, options.buildDir);
            }
            std::error_code buildError;
            const Path canonicalBuild =
                std::filesystem::weakly_canonical(options.buildDir, buildError);
            if (!buildError)
                options.buildDir = canonicalBuild;
        }

        int prepareToolchain(const Options &options, int argc, char *argv[],
                             Toolchain &toolchain, std::string &error, bool &reexecuted)
        {
            reexecuted = false;
            const Path logsDir = options.buildDir / ".builder" / "logs";
            const Arch requestedArch = resolveArch(options.arch);
            if (usesMsvcCompiler(options) && !findExecutable("cl"))
            {
                int exitCode = 0;
                std::string reexecError;
                if (reexecInDevShell(options, argc, argv, requestedArch, logsDir, exitCode,
                                     reexecError))
                {
                    reexecuted = true;
                    return exitCode;
                }
                std::cerr << "error: " << reexecError << "\n";
                return ENVIRONMENT;
            }
            configureTempDirectory(options.buildDir / ".builder" / "tmp");

            if (!discoverToolchain(options, logsDir, toolchain, error))
                return ENVIRONMENT;

            const bool msvcArchMismatch = !toolchain.devShellArch.empty() &&
                                          toolchain.devShellArch != archName(toolchain.arch);
            const bool needsDevShell =
                (toolchain.family == Family::Msvc &&
                 (toolchain.devShellArch.empty() || msvcArchMismatch)) ||
                (toolchain.family == Family::Clang && toolchain.msvcAbi && msvcArchMismatch);
            if (needsDevShell && options.command != Command::Doctor &&
                options.command != Command::Targets)
            {
                int exitCode = 0;
                std::string reexecError;
                if (reexecInDevShell(options, argc, argv, toolchain.arch, logsDir, exitCode,
                                     reexecError))
                {
                    reexecuted = true;
                    return exitCode;
                }
                std::cerr << "error: " << reexecError << "\n";
                return ENVIRONMENT;
            }
            return OK;
        }

        int runMain(int argc, char *argv[])
        {
            Options options;
            std::string error;
            CommandLineParser parser(argc, argv, options);
            if (parser.parse(error) != OK)
            {
                std::cerr << "error: " << error << "\n\n";
                printUsage(std::cerr);
                return USAGE;
            }
            if (options.command == Command::Help)
            {
                printUsage(std::cout);
                return OK;
            }

            configureJobs(options);
            resolveProjectPaths(options, argc > 0 ? argv[0] : nullptr);

            if (options.showVersion)
            {
                printVersion(options);
                return OK;
            }
            if (options.root.empty())
            {
                std::cerr << "error: cannot locate the MoteVM project root (expected "
                             "src/motevm and src/"
                          << TARGET_BUILDER << "); use --root\n";
                return ENVIRONMENT;
            }

            if (options.command == Command::Clean)
                return runClean(options);
            if (options.command == Command::Rebuild)
            {
                const int cleanResult = runClean(options);
                if (cleanResult != OK)
                    return cleanResult;
                options.command = Command::Build;
            }

            Toolchain toolchain;
            bool reexecuted = false;
            const int toolchainResult =
                prepareToolchain(options, argc, argv, toolchain, error, reexecuted);
            if (reexecuted)
                return toolchainResult;
            if (toolchainResult != OK)
            {
                if (!error.empty())
                    std::cerr << "error: " << error << "\n";
                return toolchainResult;
            }

            if (options.command == Command::Build || options.command == Command::Self)
                return runBuild(options, toolchain);
            if (options.command == Command::Doctor)
                return runDoctor(options, toolchain);
            if (options.command == Command::Targets)
                return runTargets(options, toolchain);

            printUsage(std::cout);
            return USAGE;
        }
    }
}
namespace BUILD
{
    CommandLineParser::CommandLineParser(int argc, char *argv[], Options &options)
        : argc_(argc), argv_(argv), options_(options)
    {
    }

    bool CommandLineParser::takeValue(std::string &value)
    {
        if (index_ >= argc_ || argv_[index_] == nullptr)
            return false;
        value.assign(argv_[index_]);
        ++index_;
        return !value.empty();
    }

    int CommandLineParser::parse(std::string &error)
    {
        while (index_ < argc_)
        {
            const std::string argument =
                argv_[index_] == nullptr ? std::string() : std::string(argv_[index_]);
            ++index_;

            if (!endOfOptions_ && argument == "--")
            {
                endOfOptions_ = true;
                continue;
            }
            if (!endOfOptions_ && !argument.empty() && argument[0] == '-')
            {
                const OptionStatus status = parseOption(argument, error);
                if (status == OptionStatus::Failed)
                    return USAGE;
                if (status == OptionStatus::Handled)
                    continue;
                error = "unknown option '" + argument + "'";
                return USAGE;
            }
            parsePositional(argument);
        }
        return OK;
    }

    void CommandLineParser::parsePositional(const std::string &argument)
    {
        if (options_.targets.empty() && !commandSeen_ && !endOfOptions_)
        {
            const std::optional<Command> command = commandFromWord(argument);
            if (command)
            {
                options_.command = *command;
                commandSeen_ = true;
                return;
            }
        }
        options_.targets.push_back(argument);
    }

    CommandLineParser::OptionStatus
    CommandLineParser::parseOption(const std::string &argument, std::string &error)
    {
        if (argument == "-h" || argument == "--help")
        {
            options_.command = Command::Help;
            return OptionStatus::Handled;
        }
        if (argument == "--version")
        {
            options_.showVersion = true;
            return OptionStatus::Handled;
        }
        if (argument == "-v" || argument == "--verbose")
        {
            options_.verbose = true;
            return OptionStatus::Handled;
        }
        if (argument == "-n" || argument == "--dry-run")
        {
            options_.dryRun = true;
            return OptionStatus::Handled;
        }
        if (argument == "--debug")
        {
            options_.config = Config::Debug;
            return OptionStatus::Handled;
        }
        if (argument == "--release")
        {
            options_.config = Config::Release;
            return OptionStatus::Handled;
        }
        if (argument == "--werror")
        {
            options_.werror = true;
            return OptionStatus::Handled;
        }
        if (argument == "--sanitize")
        {
            options_.sanitize = true;
            return OptionStatus::Handled;
        }
        if (argument == "--force")
        {
            options_.force = true;
            return OptionStatus::Handled;
        }

        std::string value;
        if (argument == "--compiler" || argument == "--arch")
        {
            if (!takeValue(value))
            {
                error = "option '" + argument + "' needs a value";
                return OptionStatus::Failed;
            }
            if (argument == "--compiler")
            {
                if (!parseCompilerChoice(value, options_.compiler))
                {
                    error = "unknown compiler '" + value + "' (use clang, gcc or msvc)";
                    return OptionStatus::Failed;
                }
            }
            else if (!parseArchOption(value, options_.arch))
            {
                error = "unknown architecture '" + value + "' (use x86 or x64)";
                return OptionStatus::Failed;
            }
            return OptionStatus::Handled;
        }
        if (startsWith(argument, "--compiler="))
        {
            value = argument.substr(11);
            if (value.empty() || !parseCompilerChoice(value, options_.compiler))
            {
                error = "option '--compiler=' needs clang, gcc or msvc";
                return OptionStatus::Failed;
            }
            return OptionStatus::Handled;
        }
        if (startsWith(argument, "--arch="))
        {
            value = argument.substr(7);
            if (value.empty() || !parseArchOption(value, options_.arch))
            {
                error = "option '--arch=' needs x86 or x64";
                return OptionStatus::Failed;
            }
            return OptionStatus::Handled;
        }

        if (argument == "-j" || argument == "--jobs")
        {
            if (!takeValue(value) || !parsePositiveInt(value, options_.jobs))
            {
                error = "option '" + argument + "' needs a positive integer";
                return OptionStatus::Failed;
            }
            return OptionStatus::Handled;
        }
        if (startsWith(argument, "--jobs="))
        {
            if (!parsePositiveInt(argument.substr(7), options_.jobs))
            {
                error = "option '--jobs=' needs a positive integer";
                return OptionStatus::Failed;
            }
            return OptionStatus::Handled;
        }
        if (argument == "--cxx" || argument == "--build-dir" || argument == "--root")
        {
            if (!takeValue(value))
            {
                error = "option '" + argument + "' needs a value";
                return OptionStatus::Failed;
            }
            if (argument == "--cxx")
                options_.cxx = platform::pathFromText(value);
            else if (argument == "--build-dir")
                options_.buildDir = platform::pathFromText(value);
            else
                options_.root = platform::pathFromText(value);
            return OptionStatus::Handled;
        }

        static const char *pathOptions[] = {"--cxx=", "--build-dir=", "--root="};
        for (const char *prefix : pathOptions)
        {
            const std::string prefixText(prefix);
            if (!startsWith(argument, prefixText))
                continue;
            const std::string option = prefixText.substr(0, prefixText.size() - 1);
            value = argument.substr(prefixText.size());
            if (value.empty())
            {
                error = "option '" + option + "' needs a value";
                return OptionStatus::Failed;
            }
            if (option == "--cxx")
                options_.cxx = platform::pathFromText(value);
            else if (option == "--build-dir")
                options_.buildDir = platform::pathFromText(value);
            else
                options_.root = platform::pathFromText(value);
            return OptionStatus::Handled;
        }
        return OptionStatus::NotOption;
    }
}

int main(int argc, char *argv[])
{
    return BUILD::runMain(argc, argv);
}