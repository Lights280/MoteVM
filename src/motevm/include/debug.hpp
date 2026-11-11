#pragma once
// Debugger support: DEBUG-section source mapping, instruction trace and the
// interactive REPL used by `motevm -d/--break/--step/--trace` (plan/self/debug.md).
//
// Layering: this module only observes Core through its read-only accessors and
// never re-enters it (ADR-020). The REPL runs inside CORE::DebugHook, i.e. while
// the VM is paused before an instr.

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core.hpp"
#include "define.hpp"
#include "memory.hpp"
#include "parse.hpp"

namespace DEBUG
{
    struct SourceLocation
    {
        bool known = false;
        dword function = NO_INDEX; // FUNCS index
        dword relativePc = 0;      // instruction offset inside the function
        dword line = 0;
        dword column = 0;
        std::string moduleName;  // META key 1
        std::string sourceFile;  // META key 2
        std::string functionName;
    };

    struct LocalValue
    {
        std::string name;
        word slot = 0;
        dword value = 0;
        bool parameter = false;
    };

    enum class TraceLevel
    {
        off = 0,
        call = 1,
        exec = 2,
        full = 3
    };

    // DEBUG-section helpers (never throw, never print).
    dword functionAt(const Parse &module, dword instructionIndex);
    std::string constantText(const Parse &module, dword constantIndex);
    std::string functionName(const Parse &module, dword function);
    SourceLocation locate(const Parse &module, dword instructionIndex);
    std::vector<LocalValue> localsAt(const Parse &module, const Core &core, dword instructionIndex);
    std::string instructionText(const Parse &module, dword instructionIndex);
    dword codeWord(const Parse &module, dword instructionIndex);
    dword codeCount(const Parse &module);

    // plan/05 §9 trace stream (deterministic, no timestamps/addresses).
    class TraceWriter
    {
    public:
        TraceWriter(const Parse &module, TraceLevel level, std::ostream &out, dword limit = 0);

        void instruction(const Core &core, dword pc, dword word);
        void snapshot(const Core &core, dword pc); // S line at a debugger stop
        void stopped(const Core &core, int error); // X line (plus a final S line)

        bool limited(void) const { return limited_; }
        dword lines(void) const { return count_; }

    private:
        void emit(const std::string &line);
        std::string functionField(dword instructionIndex) const;

        const Parse &module_;
        TraceLevel level_;
        std::ostream &out_;
        dword limit_ = 0;
        dword count_ = 0;
        bool headerWritten_ = false;
        bool limited_ = false;
    };

    // Interactive debugger. Feed it scripted input in tests; the CLI feeds std::cin.
    class Session
    {
    public:
        Session(const Parse &module, Memory &memory, std::istream &in, std::ostream &diag);
        ~Session() = default;
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        static bool hook(void *user, const Core &core, dword pc, dword word);

        bool addBreakpoint(const std::string &spec, std::string &error);
        void setInteractive(bool enabled) { interactive_ = enabled; }
        void setPrompt(bool enabled) { prompt_ = enabled; }
        void setStartStopped(bool enabled) { startStopped_ = enabled; }
        void setTrace(TraceLevel level, std::ostream &out, dword limit = 0);
        void finishTrace(const Core &core, int error)
        {
            if (trace_ != nullptr)
                trace_->stopped(core, error);
        }

        bool interactive(void) const { return interactive_; }
        bool quitRequested(void) const { return quit_; }
        bool abortRequested(void) const { return abort_; }
        dword stopPc(void) const { return stopPc_; }
        const std::string &lastError(void) const { return lastError_; }

    private:
        enum class Mode
        {
            Continue,
            Step,
            Next,
            Finish
        };

        struct Breakpoint
        {
            dword id = 0;
            dword pc = 0;
        };

        bool onInstruction(const Core &core, dword pc, dword word);
        bool stopAt(const Core &core, dword pc);
        void repl(const Core &core, dword pc);
        bool dispatch(const Core &core, std::istringstream &line, dword pc);

        void printHelp(std::ostream &out) const;
        void printLocation(const Core &core, dword pc, std::ostream &out) const;
        void printBacktrace(const Core &core, dword pc, std::ostream &out) const;
        void printRegisters(const Core &core, dword pc, dword start, dword count,
                            std::ostream &out) const;
        void printMemory(dword address, dword length, std::ostream &out) const;
        void printDisassembly(dword pc, dword start, dword count, std::ostream &out) const;
        void printLocals(const Core &core, dword pc, std::ostream &out) const;
        void printBreakpoints(std::ostream &out) const;
        bool deleteBreakpoint(dword id, std::ostream &out);

        dword currentRegisterCount(const Core &core, dword pc) const;

        const Parse &module_;
        Memory &memory_;
        std::istream &in_;
        std::ostream &diag_;
        std::unique_ptr<TraceWriter> trace_;

        std::vector<Breakpoint> breaks_; // insertion order, ids are stable
        std::unordered_set<dword> breakSet_;
        dword nextBreakId_ = 1;

        Mode mode_ = Mode::Continue;
        bool interactive_ = false;
        bool prompt_ = false;
        bool startStopped_ = false;
        bool entered_ = false;
        bool quit_ = false;
        bool abort_ = false;
        dword stopDepth_ = 0;
        dword stepSkips_ = 0;
        dword skipPc_ = NO_INDEX;
        dword stopPc_ = NO_INDEX;
        std::string lastError_;
    };
}
