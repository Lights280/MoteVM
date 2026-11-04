#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "core.hpp"
#include "memory.hpp"
#include "parse.hpp"

namespace HOST
{
    const int OK = 0;
    const int NO_MODULE = 100;       // host-only codes start at 100
    const int NO_MEMORY = 101;
    const int UNLINKED_IMPORT = 102; // no registered function matches (name, arity)
    const int BAD_ARGUMENT = 103;    // invalid registration request

    using Function = dword (*)(void *user, const dword *args, int arity);

    // Entry of the host function registry; matched at load time (ADR-018).
    struct Registered
    {
        std::string name;
        int arity = 0;
        Function function = nullptr;
        void *user = nullptr;
    };

    struct Stats
    {
        qword modules = 0;
        qword constants = 0;
        qword constantBytes = 0; // bytes reserved for the constant pool
        qword runs = 0;
        qword instructions = 0;
        std::size_t maxDepth = 0; // peak call depth over all runs
        dword lastPc = 0;
        int trapCode = 0;
        int result = OK;
    };
}

// Minimal driver: load modules, seed memory with the constant pool, run entries.
class Host
{
private:
    std::vector<Parse> modules;                   // owns the parsed images (spans stay valid)
    std::vector<std::string> modulePaths;         // parallel to modules
    std::vector<std::vector<dword>> constantAddr;     // [module][constant] -> memory address
    std::vector<std::vector<CORE::HostSlot>> imports; // [module][import] -> bound host slot
    std::vector<HOST::Registered> registry;           // registered host functions
    std::unique_ptr<Memory> mem;                      // Memory is large: keep it on the heap
    std::unique_ptr<Core> lastCore;                   // execution site kept for -d / debuggers
    int lastError = HOST::OK;
    HOST::Stats hostStats;
    dword lastUnlinkedIndex = 0; // diagnostics for HOST::UNLINKED_IMPORT
    std::string lastUnlinkedName;
    int lastUnlinkedArity = 0;

private:
    int ensureBlocks(qword offset, qword size);
    int bindImports(const Parse &module, std::vector<CORE::HostSlot> &slots);

public:
    Host() = default;
    ~Host() = default;
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    // Registers a host function; call it before load() because imports are resolved at
    // load time (ADR-018). Duplicate names are rejected (docs/04 §7.5).
    int registerFunction(const std::string &name, int arity, HOST::Function function,
                         void *user = nullptr);
    int load(const std::string &path);
    // Loads from 'path' but reports 'displayPath' in --status/--debug output, so a
    // workspace-resolved absolute path never leaks into diagnostics (docs/self §6).
    int load(const std::string &path, const std::string &displayPath);
    int load(const std::vector<std::string> &paths);
    int initMemory(void);
    int run(void);                    // every loaded module entry, in load order
    int runModule(std::size_t index); // one module entry (index < moduleCount())
    // Same, with an optional debugger hook (plan/self/debug.md). The Core that ran
    // stays alive so -d can render the stop context through core().
    int runModule(std::size_t index, CORE::DebugHook hook, void *user);

    std::size_t moduleCount(void) const { return modules.size(); }
    const Parse &module(std::size_t index) const { return modules.at(index); }
    const std::string &path(std::size_t index) const { return modulePaths.at(index); }
    const Core *core(void) const { return lastCore.get(); }
    Memory &memory(void) { return *mem; }
    const Memory &memory(void) const { return *mem; }
    bool hasMemory(void) const { return mem != nullptr; }
    dword constantAddress(dword module, dword index) const;
    const HOST::Stats &stats(void) const { return hostStats; }
    int error(void) const { return lastError; }

    // Details of the last HOST::UNLINKED_IMPORT failure (for CLI diagnostics).
    dword unlinkedImportIndex(void) const { return lastUnlinkedIndex; }
    const std::string &unlinkedImportName(void) const { return lastUnlinkedName; }
    int unlinkedImportArity(void) const { return lastUnlinkedArity; }
};
