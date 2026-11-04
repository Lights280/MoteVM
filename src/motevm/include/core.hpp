#pragma once
#include <cstddef>
#include <memory>
#include <span>
#include <vector>
#include <array>
#include "define.hpp"
#include "memory.hpp"
#include "parse.hpp"

class Core; // defined below; referenced by CORE::DebugHook

namespace CORE
{
    const int IDLE = 0;
    const int RUN = 1;
    const int INTERRUPT = 2;

    const int OK = 0;
    const int UNKNOWN_INSTRUCTION = 1;
    const int DIVIDED_BY_ZERO = 2;
    const int INTEGER_OVERFLOW = 3;
    const int STACK_OVERFLOW = 4;     // call depth exceeds MAX_CALL_DEPTH
    const int INVALID_TARGET = 5;     // pc or function entry outside text
    const int INVALID_TEXT = 6;       // text is empty or not a multiple of 4 bytes
    const int NO_ACTIVE_FRAME = 7;    // step/save/take without an active frame
    const int NO_CALLER = 8;          // takeContext popped the root frame: normal end
    const int NUMERIC_CONVERSION = 9; // f2i NaN/Inf/out-of-range
    const int INVALID_FUNCTION = 10;  // FUNCS index out of range
    const int INVALID_IMPORT = 11;    // IMPORTS index out of range / not bound
    const int FUEL_EXHAUSTED = 12;    // instruction budget exhausted
    const int INTERRUPTED = 13;       // interrupt hook requested stop
    const int TRAP = 14;              // TRAP instruction raised
    const int MEMORY_FAULT = 15;      // load/store failed (block released, occupied, out of range)
    const int DEBUG_BREAK = 16;       // debugger paused before the instruction at errorPc()

    const std::size_t MAX_CALL_DEPTH = 1024; // includes the root frame

    // Optional debugger hook (plan/self/debug.md). Called before an instruction is
    // executed, with `pc` pointing at it and `word` holding its raw encoding.
    // Returning true stops execution with CORE::DEBUG_BREAK. The hook may read the
    // core through the read-only accessors but must not re-enter it (ADR-020).
    // Only the guarded debug path calls it, so ordinary runs keep their hot loop.
    using DebugHook = bool (*)(void *user, const ::Core &core, dword pc, dword word);

    // Host function bound to an IMPORTS slot (ADR-018: binding happens at load time).
    // args points at the caller's r0..r(arity-1); the returned value is written to the
    // CALLH destination register. Implementations must not throw and must not re-enter
    // the VM (docs/01-isa.md §3.7).
    using HostFunction = dword (*)(void *user, const dword *args, int arity);

    struct HostSlot
    {
        HostFunction function = nullptr;
        void *user = nullptr;
        int arity = 0;
    };
}

struct StackFrame
{
    dword returnAddress = 0;
    byte returnRegister = 0;                            // CALL's dest register
    std::array<dword, ISA::REGISTER_COUNT> registers{}; // value-initialized
    StackFrame(dword returnAddress = 0, byte returnRegister = 0)
        : returnAddress(returnAddress), returnRegister(returnRegister) {}
};

class Core
{
private:
    Memory *memory = nullptr; // a non-owning (observer) pointer to host memory.
    std::span<const byte> text;
    std::span<const PARSE::Function> functionTable; // borrowed from Parse::Image
    std::span<const CORE::HostSlot> hostSlots;      // borrowed; bound by the loader (ADR-018)
    std::vector<StackFrame> callStack; // contiguous; back() is the active frame
    dword programCounter = 0;
    // alias
    dword &pc = programCounter;
    StackFrame *nowFrame = nullptr;
    int status = 0;
    int lastError = CORE::OK;
    dword lastErrorPc = 0; // instruction that reported lastError (debugger context)

    dword fuelLimit = 0; // 0 = unlimited
    dword fuelLeft = 0;
    dword executed = 0;
    std::size_t peakDepth = 1; // includes the root frame
    bool (*interruptHook)(void *) = nullptr;
    void *interruptUser = nullptr;
    CORE::DebugHook debugHook = nullptr;
    void *debugUser = nullptr;
    int lastTrap = 0;

private:
    int asSigned(const dword &a);
    dword asUnsigned(const int &a);
    float asFloat(const dword &a);
    dword asBits(const float &a);
    word merge(byte high, byte low);

    int saveContext(dword returnAddress, byte returnRegister);
    int takeContext(dword value);
    int step(byte instruction, byte op1, byte op2, byte op3); // process 1 instruction

public:
    Core() = default;
    explicit Core(Memory &hostMemory) : memory(&hostMemory) {}
    ~Core() = default;

public:
    int attachModule(std::span<const byte> text, std::span<const PARSE::Function> functions);
    void attachImports(std::span<const CORE::HostSlot> slots);
    int attachText(std::span<const byte> text);
    void detachText(void);
    void run(dword functionIndex = 0); // FUNCS index

    void setFuel(dword maxInstructions);                     // 0 = unlimited
    void setInterruptHook(bool (*hook)(void *), void *user); // true => stop
    // Optional debugger hook; see CORE::DebugHook. Cleared with (nullptr, nullptr).
    void setDebugHook(CORE::DebugHook hook, void *user);
    dword instructionCount(void) const { return executed; }
    std::size_t peakCallDepth(void) const { return peakDepth; }
    dword currentPc(void) const { return programCounter; }
    int trapCode(void) const { return lastTrap; }

    int error() const { return lastError; }
    // Instruction that reported error(); for CORE::DEBUG_BREAK it points at the
    // instruction that has not been executed yet.
    dword errorPc(void) const { return lastErrorPc; }
    // Read-only inspection for debuggers (frames[0] is the root, back() the active one).
    std::span<const StackFrame> stack(void) const { return callStack; }
    dword codeWord(dword address) const; // 0 when the address is outside the text
    dword textBytes(void) const { return static_cast<dword>(text.size()); }
    std::size_t callDepth() const { return callStack.size(); }
};