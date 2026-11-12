#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace BUILD
{
    inline constexpr const char *BUILDER_VERSION = "1.0";

    inline constexpr int OK = 0;
    inline constexpr int BUILD_FAILED = 1;
    inline constexpr int USAGE = 2;
    inline constexpr int ENVIRONMENT = 3;

    inline constexpr std::uint64_t FNV_OFFSET = 1469598103934665603ull;
    inline constexpr std::uint64_t FNV_PRIME = 1099511628211ull;

    inline constexpr const char *TARGET_ALL = "all";
    inline constexpr const char *TARGET_BUILDER = "builder";
    inline constexpr const char *TARGET_CORE = "motevm_core";
    inline constexpr const char *TARGET_ARCHIVE = "motevm_archive";
    inline constexpr const char *TARGET_TOOLS = "motevm_tools";
    inline constexpr const char *OBJECT_STATE_HEADER = "builder-state 1";
    inline constexpr const char *STEP_STATE_HEADER = "builder-step-state 1";
    inline constexpr const char *REEXEC_ENV = "MOTEVM_REEXEC";

    using Path = std::filesystem::path;

    enum class Command
    {
        Build,
        Clean,
        Rebuild,
        Self,
        Doctor,
        Targets,
        Help
    };

    enum class Config
    {
        Debug,
        Release
    };

    enum class Arch
    {
        Host,
        X86,
        X64
    };

    enum class Family
    {
        Clang,
        Gcc,
        Msvc,
        Unknown
    };

    enum class CompilerChoice
    {
        Auto,
        Clang,
        Gcc,
        Msvc
    };

    struct Options
    {
        Command command = Command::Build;
        Config config = Config::Release;
        CompilerChoice compiler = CompilerChoice::Auto;
        Arch arch = Arch::Host;
        Path root;
        Path buildDir;
        Path cxx;
        Path currentExe;
        int jobs = 0;
        bool werror = false;
        bool sanitize = false;
        bool force = false;
        bool dryRun = false;
        bool verbose = false;
        bool showVersion = false;
        std::vector<std::string> targets;
    };

    struct Toolchain
    {
        Path cxx;
        Path linker; // MSVC link.exe; GNU/Clang link through cxx
        Path lldLink;
        Family family = Family::Unknown;
        Arch arch = Arch::X64;
        std::string versionLine;
        std::string triple;
        std::string devShellArch;
        bool msvcAbi = false;
        bool depFiles = true;
    };

    struct Target
    {
        std::string name;
        std::vector<Path> sources;
        std::vector<Path> includes;
        Path mainFile;
        Path output;
        std::vector<std::string> links; // shared source groups this target links
        bool internal = false;          // shared group, not a user-selectable target
    };

    struct Project
    {
        Path root;
        Path buildDir;
        std::string version = "unknown";
        std::string formatVersion = "unknown";
        std::vector<Target> targets;
    };

    struct ProcessResult
    {
        bool started = false;
        int exitCode = 0;
        std::string spawnError;
    };

    struct FileDigest
    {
        bool ok = false;
        std::uint64_t hash = 0;
        std::uint64_t size = 0;
    };

    struct DepDigest
    {
        Path path;
        std::uint64_t hash = 0;
        std::uint64_t size = 0;
    };

    struct ObjectState
    {
        bool valid = false;
        bool depsKnown = false;
        std::uint64_t cxxHash = 0;
        std::uint64_t flagsHash = 0;
        std::uint64_t sourceHash = 0;
        std::uint64_t sourceSize = 0;
        std::vector<DepDigest> deps;
    };

    struct StepState
    {
        bool valid = false;
        std::uint64_t argsHash = 0;
        std::uint64_t outputHash = 0;
        std::uint64_t outputSize = 0;
        std::vector<DepDigest> inputs;
    };

    struct CompileUnit
    {
        const Target *target = nullptr;
        Path source;
        Path object;
        Path depFile;
        Path stateFile;
        Path logFile;
        std::string display;
        std::vector<std::string> args;
        std::uint64_t cxxHash = 0;
        std::uint64_t flagsHash = 0;
        bool needsBuild = false;
        bool built = false;
    };

    struct BuildStep
    {
        std::string label;
        std::string display;
        std::vector<std::string> args;
        std::vector<Path> inputs;
        Path output;
        Path finalOutput;
        Path stateFile;
        Path logFile;
        std::uint64_t argsHash = 0;
        bool needsBuild = false;
        bool replaceSelf = false;
    };
}
