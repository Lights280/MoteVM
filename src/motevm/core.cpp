#include "core.hpp"
#include "verifier.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
    const dword FLOAT_SIGN_BIT = 0x80000000u;
    const dword FLOAT_ABS_MASK = 0x7fffffffu;

    // Internal loop signals. They never escape Core::run(), so they stay out of
    // the public CORE:: error range.
    const int STEP_HALT = 0x100;
    const int STEP_HOSTCALL = 0x101;

    // Single 32-bit, host-endianness-independent instruction fetch.
    inline dword loadWord(const byte *p)
    {
        dword value;
        std::memcpy(&value, p, sizeof(value));
        if constexpr (std::endian::native == std::endian::big)
            value = (value >> 24) | ((value >> 8) & 0x0000ff00u) | ((value << 8) & 0x00ff0000u) |
                    (value << 24);
        return value;
    }

    inline int signExtend16(word value)
    {
        return static_cast<std::int16_t>(value);
    }

    inline int signExtend24(dword value)
    {
        const dword bits = value & 0x00FFFFFFu;
        return (bits & 0x00800000u) ? static_cast<int>(bits | 0xFF000000u) : static_cast<int>(bits);
    }
}

int Core::asSigned(const dword &a)
{
    return std::bit_cast<int>(a);
}

dword Core::asUnsigned(const int &a)
{
    return std::bit_cast<dword>(a);
}

float Core::asFloat(const dword &a)
{
    return std::bit_cast<float>(a);
}

dword Core::asBits(const float &a)
{
    return std::bit_cast<dword>(a);
}

word Core::merge(byte high, byte low)
{
    return static_cast<word>((high << 8) | low);
}

int Core::step(StackFrame *&frame, dword &pc, dword &executedLocal, dword encoded)
{
    if (frame == nullptr)
        return CORE::NO_ACTIVE_FRAME;

    const byte instruction = static_cast<byte>(encoded & ISA::OPCODE_MASK);
    const byte op1 = static_cast<byte>((encoded >> ISA::OPERAND1_SHIFT) & ISA::OPERAND_MASK);
    const byte op2 = static_cast<byte>((encoded >> ISA::OPERAND2_SHIFT) & ISA::OPERAND_MASK);
    const byte op3 = static_cast<byte>((encoded >> ISA::OPERAND3_SHIFT) & ISA::OPERAND_MASK);

    std::array<dword, ISA::REGISTER_COUNT> &reg = frame->registers;
    int result = CORE::OK;
    switch (static_cast<int>(instruction))
    {
    case ISA::zero:
        reg[op1] = 0;
        break;
    case ISA::load:
        if (memory->readAt(reg[op1], reg[op2], op3) != MEMORY::OK)
            result = CORE::MEMORY_FAULT;
        break;
    case ISA::sto:
        if (memory->writeAt(reg[op1], reg[op2], op3) != MEMORY::OK)
            result = CORE::MEMORY_FAULT;
        break;
    case ISA::lhi:
        reg[op1] = (reg[op1] & 0x0000ffffu) | (static_cast<dword>(merge(op3, op2)) << 16);
        break;
    case ISA::llo:
        reg[op1] = (reg[op1] & 0xffff0000u) | merge(op3, op2);
        break;
    case ISA::mov:
        reg[op1] = reg[op2];
        break;
    case ISA::add:
    case ISA::addu:
        // Two's-complement addition wraps modulo 2^32; unsigned arithmetic is well-defined.
        reg[op1] = reg[op2] + reg[op3];
        break;
    case ISA::sub:
    case ISA::subu:
        reg[op1] = reg[op2] - reg[op3];
        break;
    case ISA::mul:
    case ISA::mulu:
        reg[op1] = reg[op2] * reg[op3];
        break;
    case ISA::div:
    {
        if (reg[op3] == 0)
        {
            result = CORE::DIVIDED_BY_ZERO;
            break;
        }

        const int dividend = asSigned(reg[op2]);
        const int divisor = asSigned(reg[op3]);
        if (dividend == std::numeric_limits<int>::lowest() && divisor == -1)
            result = CORE::INTEGER_OVERFLOW; // INT32_MIN / -1 cannot be represented.
        else
            reg[op1] = asUnsigned(dividend / divisor);
        break;
    }
    case ISA::divu:
        if (reg[op3] == 0)
            result = CORE::DIVIDED_BY_ZERO;
        else
            reg[op1] = reg[op2] / reg[op3];
        break;
    case ISA::mod:
    {
        if (reg[op3] == 0)
        {
            result = CORE::DIVIDED_BY_ZERO;
            break;
        }
        const int dividend = asSigned(reg[op2]);
        const int divisor = asSigned(reg[op3]);
        if (dividend == std::numeric_limits<int>::lowest() && divisor == -1)
            reg[op1] = 0; // INT32_MIN % -1 == 0; avoids signed-division UB.
        else
            reg[op1] = asUnsigned(dividend % divisor);
        break;
    }
    case ISA::modu:
        if (reg[op3] == 0)
            result = CORE::DIVIDED_BY_ZERO;
        else
            reg[op1] = reg[op2] % reg[op3];
        break;
    case ISA::neg:
    {
        const int value = asSigned(reg[op2]);
        if (value == std::numeric_limits<int>::lowest())
            result = CORE::INTEGER_OVERFLOW; // -INT32_MIN cannot be represented.
        else
            reg[op1] = asUnsigned(-value);
        break;
    }
    case ISA::band:
        reg[op1] = reg[op2] & reg[op3];
        break;
    case ISA::bor:
        reg[op1] = reg[op2] | reg[op3];
        break;
    case ISA::bxor:
        reg[op1] = reg[op2] ^ reg[op3];
        break;
    case ISA::bnot:
        reg[op1] = ~reg[op2];
        break;
    case ISA::shl:
        reg[op1] = reg[op2] << (reg[op3] & 0x1f);
        break;
    case ISA::shr:
        reg[op1] = reg[op2] >> (reg[op3] & 0x1f);
        break;
    case ISA::sar:
    {
        const int value = asSigned(reg[op2]);
        reg[op1] = asUnsigned(value >> (reg[op3] & 0x1f));
        break;
    }
    case ISA::eq:
        reg[op1] = reg[op2] == reg[op3];
        break;
    case ISA::ne:
        reg[op1] = reg[op2] != reg[op3];
        break;
    case ISA::lt:
        reg[op1] = asSigned(reg[op2]) < asSigned(reg[op3]);
        break;
    case ISA::le:
        reg[op1] = asSigned(reg[op2]) <= asSigned(reg[op3]);
        break;
    case ISA::gt:
        reg[op1] = asSigned(reg[op2]) > asSigned(reg[op3]);
        break;
    case ISA::ge:
        reg[op1] = asSigned(reg[op2]) >= asSigned(reg[op3]);
        break;
    case ISA::ltu:
        reg[op1] = reg[op2] < reg[op3];
        break;
    case ISA::leu:
        reg[op1] = reg[op2] <= reg[op3];
        break;
    case ISA::gtu:
        reg[op1] = reg[op2] > reg[op3];
        break;
    case ISA::geu:
        reg[op1] = reg[op2] >= reg[op3];
        break;
    case ISA::fadd:
        reg[op1] = asBits(asFloat(reg[op2]) + asFloat(reg[op3]));
        break;
    case ISA::fsub:
        reg[op1] = asBits(asFloat(reg[op2]) - asFloat(reg[op3]));
        break;
    case ISA::fmul:
        reg[op1] = asBits(asFloat(reg[op2]) * asFloat(reg[op3]));
        break;
    case ISA::fdiv:
        // IEEE division: division by zero yields Inf/NaN, never a trap.
        reg[op1] = asBits(asFloat(reg[op2]) / asFloat(reg[op3]));
        break;
    case ISA::fneg:
        reg[op1] = reg[op2] ^ FLOAT_SIGN_BIT; // sign flip keeps NaN payload
        break;
    case ISA::fabs:
        reg[op1] = reg[op2] & FLOAT_ABS_MASK;
        break;
    case ISA::fsqrt:
        reg[op1] = asBits(std::sqrt(asFloat(reg[op2])));
        break;
    case ISA::i2f:
        reg[op1] = asBits(static_cast<float>(asSigned(reg[op2])));
        break;
    case ISA::f2i:
    {
        const float value = asFloat(reg[op2]);
        if (!(value >= -2147483648.0f && value < 2147483648.0f))
        {
            result = CORE::NUMERIC_CONVERSION;
            break;
        }
        reg[op1] = asUnsigned(static_cast<int>(value));
        break;
    }
    case ISA::feq:
        reg[op1] = asFloat(reg[op2]) == asFloat(reg[op3]);
        break;
    case ISA::fne:
        reg[op1] = asFloat(reg[op2]) != asFloat(reg[op3]);
        break;
    case ISA::flt:
        reg[op1] = asFloat(reg[op2]) < asFloat(reg[op3]);
        break;
    case ISA::fle:
        reg[op1] = asFloat(reg[op2]) <= asFloat(reg[op3]);
        break;
    case ISA::fgt:
        reg[op1] = asFloat(reg[op2]) > asFloat(reg[op3]);
        break;
    case ISA::fge:
        reg[op1] = asFloat(reg[op2]) >= asFloat(reg[op3]);
        break;
    case ISA::jmp:
    {
        const dword bits = static_cast<dword>(op1) | (static_cast<dword>(op2) << 8) |
                           (static_cast<dword>(op3) << 16);
        const int offset = signExtend24(bits);
        if (offset % ISA::INSTRUCTION_SIZE != 0)
        {
            result = CORE::INVALID_TARGET;
            break;
        }
        pc += static_cast<dword>(offset);
        break;
    }
    case ISA::jz:
    {
        const int offset = signExtend16(merge(op3, op2)); // imm16 = (c << 8) | b
        if (offset % ISA::INSTRUCTION_SIZE != 0)
        {
            result = CORE::INVALID_TARGET;
            break;
        }
        if (!reg[op1])
            pc += static_cast<dword>(offset);
        break;
    }
    case ISA::jnz:
    {
        const int offset = signExtend16(merge(op3, op2));
        if (offset % ISA::INSTRUCTION_SIZE != 0)
        {
            result = CORE::INVALID_TARGET;
            break;
        }
        if (reg[op1])
            pc += static_cast<dword>(offset);
        break;
    }
    case ISA::call:
    {
        const dword index = merge(op3, op2); // FUNCS index
        if (index >= functionTable.size())
        {
            result = CORE::INVALID_FUNCTION;
            break;
        }
        const dword target = functionTable[index].entry * ISA::INSTRUCTION_SIZE;
        if (target >= text.size() || text.size() - target < ISA::INSTRUCTION_SIZE)
        {
            result = CORE::INVALID_TARGET;
            break;
        }
        // pc already points to the next instruction.
        result = saveContext(frame, pc, op1, copyExtent[index]);
        if (result == CORE::OK)
            pc = target;
        break;
    }
    case ISA::ret:
    {
        const dword value = reg[op1];
        result = takeContext(frame, pc, value);
        if (result == CORE::NO_CALLER)
        {
            status = CORE::IDLE;
            return STEP_HALT;
        }
        break;
    }
    case ISA::halt:
        status = CORE::IDLE; // stop execution
        return STEP_HALT;
    case ISA::breakpoint:
        break; // NOP without a debugger
    case ISA::trap:
    {
        const dword bits = static_cast<dword>(op1) | (static_cast<dword>(op2) << 8) |
                           (static_cast<dword>(op3) << 16);
        lastTrap = static_cast<int>(bits);
        result = CORE::TRAP;
        break;
    }
    case ISA::hostcall:
    {
        const dword index = merge(op3, op2); // IMPORTS index
        if (index >= hostSlots.size() || hostSlots[index].function == nullptr ||
            hostSlots[index].arity < 0 || hostSlots[index].arity > ISA::REGISTER_COUNT)
        {
            result = CORE::INVALID_IMPORT;
            break;
        }
        // Make instructionCount()/currentPc() observable before entering host code.
        executed += executedLocal;
        executedLocal = 0;
        programCounter = pc;
        const CORE::HostSlot &slot = hostSlots[index];
        reg[op1] = slot.function(slot.user, reg.data(), slot.arity);
        return STEP_HOSTCALL;
    }
    default:
        result = CORE::UNKNOWN_INSTRUCTION;
        break;
    }
    return result;
}

int Core::saveContext(StackFrame *&frame, dword returnAddress, byte returnRegister, word copyCount)
{
    if (frame == nullptr)
        return CORE::NO_ACTIVE_FRAME;
    if (callStack.size() >= CORE::MAX_CALL_DEPTH)
        return CORE::STACK_OVERFLOW;

    // Grow geometrically, but always before taking a reference to the caller:
    // a reallocation moves every frame and invalidates `frame`.
    if (callStack.size() == callStack.capacity())
    {
        const std::size_t grown = std::max<std::size_t>(64, callStack.capacity() * 2);
        callStack.reserve(std::min<std::size_t>(CORE::MAX_CALL_DEPTH, grown));
        frame = &callStack.back();
    }

    callStack.emplace_back(returnAddress, returnRegister);
    StackFrame &callee = callStack.back();
    const dword count = std::min<dword>(copyCount, ISA::REGISTER_COUNT);
    if (count != 0)
        std::memcpy(callee.registers.data(), frame->registers.data(), count * sizeof(dword));
    // Slots above `count` are never addressed by any reachable function (see
    // computeCopyExtents), so leaving them indeterminate is safe.
    frame = &callee;
    if (callStack.size() > peakDepth)
        peakDepth = callStack.size();
    return CORE::OK;
}

int Core::takeContext(StackFrame *&frame, dword &pc, dword value)
{
    if (frame == nullptr || callStack.empty())
        return CORE::NO_ACTIVE_FRAME;
    const byte returnRegister = frame->returnRegister;
    const dword returnAddress = frame->returnAddress;
    callStack.pop_back();
    if (callStack.empty())
    {
        frame = nullptr;
        return CORE::NO_CALLER;
    }
    frame = &callStack.back();
    frame->registers[returnRegister] = value;
    pc = returnAddress;
    return CORE::OK;
}

int Core::attachModule(std::span<const byte> codeBytes,
                       std::span<const PARSE::Function> functions)
{
    return attachModuleInternal(codeBytes, functions, false, {});
}

int Core::attachVerifiedModule(const Parse &module)
{
    if (!module.isValid())
        return CORE::INVALID_TEXT;
    return attachModuleInternal(module.text(), module.functions(), true, module.copyExtents());
}

int Core::attachModuleInternal(std::span<const byte> codeBytes,
                               std::span<const PARSE::Function> functions, bool isVerified,
                               std::span<const word> copyExtents)
{
    if (codeBytes.empty() || codeBytes.size() % ISA::INSTRUCTION_SIZE != 0)
        return CORE::INVALID_TEXT;
    for (const PARSE::Function &fn : functions)
        if (static_cast<qword>(fn.entry) * ISA::INSTRUCTION_SIZE + ISA::INSTRUCTION_SIZE >
            codeBytes.size())
            return CORE::INVALID_TARGET;
    const int attached = attachText(codeBytes);
    if (attached != CORE::OK)
        return attached;
    functionTable = functions;
    verified = isVerified;

    // The verifier's table is trustworthy only for a verified module of the
    // same shape; anything else keeps the conservative CODE scan.
    bool reusable = isVerified && copyExtents.size() == functions.size();
    if (reusable)
        for (word extent : copyExtents)
            if (extent > ISA::REGISTER_COUNT)
            {
                reusable = false;
                break;
            }
    if (reusable)
        copyExtent.assign(copyExtents.begin(), copyExtents.end());
    else
        computeCopyExtents();
    buildCallTable();
    return CORE::OK;
}

void Core::buildCallTable(void)
{
    const std::size_t count = functionTable.size();
    callTable.resize(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        callTable[i].target = functionTable[i].entry * ISA::INSTRUCTION_SIZE;
        callTable[i].copies = i < copyExtent.size() ? copyExtent[i] : 0;
    }
}

// Calls copy the caller's whole 256-register window today, but a function's
// own operands are bounded by its declared regcnt. A callee can nevertheless
// be reached from a smaller frame and legally read slots above that frame's
// regcnt, so the copy extent must include every statically reachable callee:
//
//   copyExtent[f] = max(regcnt[f], copyExtent[g] for every CALL edge f -> g)
//
// The fixpoint reproduces the old full-window values for every reachable
// register while letting ordinary functions copy only a handful of slots.
// A function whose operands violate its declared regcnt (possible only via a
// direct, unverified attachModule) gets the full 256-slot extent, which keeps
// the previous behaviour exactly.
void Core::computeCopyExtents(void)
{
    const std::size_t count = functionTable.size();
    copyExtent.assign(count, 0);
    if (count == 0)
        return;

    const std::size_t wordCount = text.size() / ISA::INSTRUCTION_SIZE;
    std::vector<PARSE::CallEdge> edges;
    edges.reserve(count * 2);

    for (std::size_t f = 0; f < count; ++f)
    {
        const PARSE::Function &fn = functionTable[f];
        dword extent = fn.regcnt;
        if (extent == 0 || extent > ISA::REGISTER_COUNT)
            extent = ISA::REGISTER_COUNT;

        const std::size_t begin = std::min<std::size_t>(fn.entry, wordCount);
        const std::size_t end = std::min<std::size_t>(
            static_cast<std::size_t>(fn.entry) + fn.instr, wordCount);
        for (std::size_t i = begin; i < end; ++i)
        {
            const dword instruction = loadWord(text.data() + i * ISA::INSTRUCTION_SIZE);
            const byte opcode = static_cast<byte>(instruction & ISA::OPCODE_MASK);
            const PARSE::OpInfo *info = PARSE::opcodeInfo(opcode);
            if (info == nullptr)
            {
                extent = ISA::REGISTER_COUNT;
                break;
            }
            const byte a = static_cast<byte>((instruction >> ISA::OPERAND1_SHIFT) & ISA::OPERAND_MASK);
            const byte b = static_cast<byte>((instruction >> ISA::OPERAND2_SHIFT) & ISA::OPERAND_MASK);
            const byte c = static_cast<byte>((instruction >> ISA::OPERAND3_SHIFT) & ISA::OPERAND_MASK);
            if (((info->regMask & 0x1) != 0 && a >= fn.regcnt) ||
                ((info->regMask & 0x2) != 0 && b >= fn.regcnt) ||
                ((info->regMask & 0x4) != 0 && c >= fn.regcnt))
            {
                extent = ISA::REGISTER_COUNT;
                break;
            }
            if (opcode == ISA::call)
            {
                const dword target = (instruction >> 16) & 0xFFFFu;
                if (target < count)
                    edges.push_back(PARSE::CallEdge{static_cast<dword>(f), target});
            }
        }
        copyExtent[f] = static_cast<word>(extent);
    }

    PARSE::fixpointCopyExtents(copyExtent, edges);
}

void Core::attachImports(std::span<const CORE::HostSlot> slots)
{
    hostSlots = slots;
}

int Core::attachText(std::span<const byte> codeBytes)
{
    if (codeBytes.empty() || codeBytes.size() % ISA::INSTRUCTION_SIZE != 0)
        return CORE::INVALID_TEXT;
    text = codeBytes;
    functionTable = {};
    hostSlots = {};
    copyExtent.clear();
    callTable.clear();
    callStack.clear();
    verified = false;
    programCounter = 0;
    status = CORE::IDLE;
    lastError = CORE::OK;
    lastErrorPc = 0;
    return CORE::OK;
}

void Core::detachText(void)
{
    text = {};
    functionTable = {};
    hostSlots = {};
    copyExtent.clear();
    callTable.clear();
    callStack.clear();
    verified = false;
    programCounter = 0;
    status = CORE::IDLE;
    lastError = CORE::OK;
    lastErrorPc = 0;
}

// Hot loop shared by the unguarded and guarded paths. One fetch and one
// switch dispatch per instruction; pc, the register base, the instruction
// counter (and the fuel budget when enabled) stay in registers and are only
// synced at exit boundaries. Hot opcodes are executed inline; cold ones
// (div/mod/neg/float/trap/breakpoint/unknown) fall back to step() so the
// semantics still have a single source of truth.
//
// GuardMask bit 1 = fuel, bit 2 = interrupt hook, bit 4 = debug hook; 0 is the
// unguarded loop. The guard outline reproduces the ADR-020 check order exactly:
// fuel -> ++executed -> interrupt -> bounds -> fetch -> debug -> dispatch.
//
// CheckBounds is true for modules attached through the conservative
// attachModule(): every pc fetch and every computed control-flow target is
// checked exactly like the pre-hot-path interpreter. It is false only for
// attachVerifiedModule(), where PARSE::Verifier guarantees in-range, aligned
// targets. Guarded loops always keep the check (same as the old guarded loop).
template <bool CheckBounds, unsigned GuardMask>
int Core::executeHot(StackFrame *&frame, dword &pc, dword &executedLocal)
{
    constexpr bool GUARD_FUEL = (GuardMask & 1u) != 0;
    constexpr bool GUARD_INTERRUPT = (GuardMask & 2u) != 0;
    constexpr bool GUARD_DEBUG = (GuardMask & 4u) != 0;

    if (frame == nullptr)
        return CORE::NO_ACTIVE_FRAME;

    const byte *code = text.data();
    const std::size_t size = text.size();
    Memory *mem = memory;
    dword ipc = pc;
    dword count = executedLocal;
    dword *regs = frame->registers.data();
    // Guard state is stable inside a loop segment: only a hostcall or an
    // interrupt/debug hook can observe or change it, and those are handled
    // below. Fuel is local so the common check costs a register decrement.
    [[maybe_unused]] dword fuelLocal = fuelLeft;
    [[maybe_unused]] auto interruptLocal = interruptHook;
    [[maybe_unused]] void *interruptUserLocal = interruptUser;
    [[maybe_unused]] CORE::DebugHook debugLocal = debugHook;
    [[maybe_unused]] void *debugUserLocal = debugUser;
    [[maybe_unused]] const dword guardFuelLimit = fuelLimit;
    [[maybe_unused]] bool guardsDirty = false;

#define VM_A ((inst >> ISA::OPERAND1_SHIFT) & ISA::OPERAND_MASK)
#define VM_B ((inst >> ISA::OPERAND2_SHIFT) & ISA::OPERAND_MASK)
#define VM_C ((inst >> ISA::OPERAND3_SHIFT) & ISA::OPERAND_MASK)
#define VM_OFF16 (signExtend16(static_cast<::word>(merge(static_cast<byte>(VM_C), static_cast<byte>(VM_B)))))
#define VM_OFF24 (signExtend24(VM_A | (VM_B << 8) | (VM_C << 16)))
#define VM_SYNC()                     \
    do                                \
    {                                 \
        pc = ipc;                     \
        executedLocal = count;        \
        if constexpr (GUARD_FUEL)     \
            fuelLeft = fuelLocal;     \
    } while (0)
#define VM_FAULT(code_)                            \
    do                                             \
    {                                              \
        lastError = (code_);                       \
        lastErrorPc = ipc - ISA::INSTRUCTION_SIZE; \
        VM_SYNC();                                 \
        return (code_);                            \
    } while (0)
// A hook may have changed the guard configuration through its user pointer.
// The change takes effect for the next instruction, exactly like the member
// reads in the old guarded loop: finish this instruction, then let run()
// re-select the loop.
#define VM_GUARDS_DIRTY_CHECK()                                                        \
    do                                                                                 \
    {                                                                                  \
        if constexpr (GUARD_INTERRUPT || GUARD_DEBUG)                                  \
        {                                                                              \
            if (fuelLimit != guardFuelLimit || interruptHook != interruptLocal ||      \
                interruptUser != interruptUserLocal || debugHook != debugLocal ||      \
                debugUser != debugUserLocal)                                           \
                guardsDirty = true;                                                    \
        }                                                                              \
    } while (0)
// Keep conditional jumps as real branches. With the extra alignment check in
// the conservative loop, clang otherwise if-converts `if (reg) ipc += off`
// into cmove, which makes the next fetch address data-dependent. The same
// fence is applied to the guarded instances.
#if defined(_MSC_VER)
#define VM_COMPILER_BARRIER() std::atomic_signal_fence(std::memory_order_seq_cst)
#elif defined(__GNUC__) || defined(__clang__)
#define VM_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define VM_COMPILER_BARRIER() ((void)0)
#endif
#define VM_TAKEN_BARRIER()                              \
    do                                                  \
    {                                                   \
        if constexpr (CheckBounds || GuardMask != 0)    \
            VM_COMPILER_BARRIER();                      \
    } while (0)

    for (;;)
    {
        if constexpr (GUARD_FUEL)
        {
            if (fuelLocal == 0)
            {
                lastError = CORE::FUEL_EXHAUSTED;
                VM_SYNC();
                return CORE::FUEL_EXHAUSTED;
            }
            --fuelLocal;
        }

        ++count;
        if constexpr (GUARD_INTERRUPT)
        {
            executed += count;
            count = 0;
            programCounter = ipc;
            if (interruptLocal(interruptUserLocal))
            {
                lastError = CORE::INTERRUPTED;
                VM_SYNC();
                return CORE::INTERRUPTED;
            }
            VM_GUARDS_DIRTY_CHECK();
        }

        if constexpr (CheckBounds)
        {
            if (static_cast<std::size_t>(ipc) + ISA::INSTRUCTION_SIZE > size)
            {
                lastError = CORE::INVALID_TARGET;
                lastErrorPc = ipc;
                VM_SYNC();
                return CORE::INVALID_TARGET;
            }
        }

        const dword inst = loadWord(code + ipc);
        if constexpr (GUARD_DEBUG)
        {
            executed += count;
            count = 0;
            programCounter = ipc;
            if (debugLocal(debugUserLocal, *this, ipc, inst))
            {
                lastError = CORE::DEBUG_BREAK;
                lastErrorPc = ipc;
                VM_SYNC();
                return CORE::DEBUG_BREAK;
            }
            VM_GUARDS_DIRTY_CHECK();
        }
        ipc += ISA::INSTRUCTION_SIZE;

        switch (inst & ISA::OPCODE_MASK)
        {
        case ISA::zero:
            regs[VM_A] = 0;
            break;
        case ISA::load:
            if (mem->readAt(regs[VM_A], regs[VM_B], VM_C) != MEMORY::OK)
                VM_FAULT(CORE::MEMORY_FAULT);
            break;
        case ISA::sto:
            if (mem->writeAt(regs[VM_A], regs[VM_B], VM_C) != MEMORY::OK)
                VM_FAULT(CORE::MEMORY_FAULT);
            break;
        case ISA::lhi:
            regs[VM_A] = (regs[VM_A] & 0x0000ffffu) |
                         (static_cast<dword>(merge(static_cast<byte>(VM_C), static_cast<byte>(VM_B)))
                          << 16);
            break;
        case ISA::llo:
            regs[VM_A] = (regs[VM_A] & 0xffff0000u) |
                         merge(static_cast<byte>(VM_C), static_cast<byte>(VM_B));
            break;
        case ISA::mov:
            regs[VM_A] = regs[VM_B];
            break;
        case ISA::add:
        case ISA::addu:
            regs[VM_A] = regs[VM_B] + regs[VM_C];
            break;
        case ISA::sub:
        case ISA::subu:
            regs[VM_A] = regs[VM_B] - regs[VM_C];
            break;
        case ISA::mul:
        case ISA::mulu:
            regs[VM_A] = regs[VM_B] * regs[VM_C];
            break;
        case ISA::band:
            regs[VM_A] = regs[VM_B] & regs[VM_C];
            break;
        case ISA::bor:
            regs[VM_A] = regs[VM_B] | regs[VM_C];
            break;
        case ISA::bxor:
            regs[VM_A] = regs[VM_B] ^ regs[VM_C];
            break;
        case ISA::bnot:
            regs[VM_A] = ~regs[VM_B];
            break;
        case ISA::shl:
            regs[VM_A] = regs[VM_B] << (regs[VM_C] & 0x1f);
            break;
        case ISA::shr:
            regs[VM_A] = regs[VM_B] >> (regs[VM_C] & 0x1f);
            break;
        case ISA::sar:
            regs[VM_A] = asUnsigned(asSigned(regs[VM_B]) >> (regs[VM_C] & 0x1f));
            break;
        case ISA::eq:
            regs[VM_A] = regs[VM_B] == regs[VM_C];
            break;
        case ISA::ne:
            regs[VM_A] = regs[VM_B] != regs[VM_C];
            break;
        case ISA::lt:
            regs[VM_A] = asSigned(regs[VM_B]) < asSigned(regs[VM_C]);
            break;
        case ISA::le:
            regs[VM_A] = asSigned(regs[VM_B]) <= asSigned(regs[VM_C]);
            break;
        case ISA::gt:
            regs[VM_A] = asSigned(regs[VM_B]) > asSigned(regs[VM_C]);
            break;
        case ISA::ge:
            regs[VM_A] = asSigned(regs[VM_B]) >= asSigned(regs[VM_C]);
            break;
        case ISA::ltu:
            regs[VM_A] = regs[VM_B] < regs[VM_C];
            break;
        case ISA::leu:
            regs[VM_A] = regs[VM_B] <= regs[VM_C];
            break;
        case ISA::gtu:
            regs[VM_A] = regs[VM_B] > regs[VM_C];
            break;
        case ISA::geu:
            regs[VM_A] = regs[VM_B] >= regs[VM_C];
            break;
        case ISA::jmp:
        {
            const int offset = VM_OFF24;
            if constexpr (CheckBounds)
            {
                if (offset % ISA::INSTRUCTION_SIZE != 0)
                    VM_FAULT(CORE::INVALID_TARGET);
            }
            ipc += static_cast<dword>(offset);
            break;
        }
        case ISA::jz:
        {
            const int offset = VM_OFF16;
            if constexpr (CheckBounds)
            {
                if (offset % ISA::INSTRUCTION_SIZE != 0)
                    VM_FAULT(CORE::INVALID_TARGET);
            }
            if (!regs[VM_A])
            {
                ipc += static_cast<dword>(offset);
                VM_TAKEN_BARRIER();
            }
            break;
        }
        case ISA::jnz:
        {
            const int offset = VM_OFF16;
            if constexpr (CheckBounds)
            {
                if (offset % ISA::INSTRUCTION_SIZE != 0)
                    VM_FAULT(CORE::INVALID_TARGET);
            }
            if (regs[VM_A])
            {
                ipc += static_cast<dword>(offset);
                VM_TAKEN_BARRIER();
            }
            break;
        }
        case ISA::halt:
            status = CORE::IDLE;
            VM_SYNC();
            return STEP_HALT;
        case ISA::call:
        {
            const dword index =
                static_cast<dword>(merge(static_cast<byte>(VM_C), static_cast<byte>(VM_B)));
            if (index >= callTable.size())
                VM_FAULT(CORE::INVALID_FUNCTION);
            const dword target = callTable[index].target;
            if constexpr (CheckBounds)
            {
                if (target >= size || size - target < ISA::INSTRUCTION_SIZE)
                    VM_FAULT(CORE::INVALID_TARGET);
            }
            if (callStack.size() >= CORE::MAX_CALL_DEPTH)
                VM_FAULT(CORE::STACK_OVERFLOW);
            if (callStack.size() == callStack.capacity())
            {
                const std::size_t grown = std::max<std::size_t>(64, callStack.capacity() * 2);
                callStack.reserve(std::min<std::size_t>(CORE::MAX_CALL_DEPTH, grown));
                frame = &callStack.back();
                regs = frame->registers.data();
            }
            callStack.emplace_back(ipc, static_cast<byte>(VM_A));
            StackFrame &callee = callStack.back();
            const dword copies = std::min<dword>(callTable[index].copies, ISA::REGISTER_COUNT);
            dword *dst = callee.registers.data();
            const dword *src = frame->registers.data();
            // The overwhelmingly common extents are small fixed windows; an
            // unrolled copy avoids a variable-size memcpy call per CALL.
            switch (copies)
            {
            case 8:
                for (dword i = 0; i < 8; ++i)
                    dst[i] = src[i];
                break;
            case 12:
                for (dword i = 0; i < 12; ++i)
                    dst[i] = src[i];
                break;
            case 16:
                for (dword i = 0; i < 16; ++i)
                    dst[i] = src[i];
                break;
            case 24:
                for (dword i = 0; i < 24; ++i)
                    dst[i] = src[i];
                break;
            case 32:
                for (dword i = 0; i < 32; ++i)
                    dst[i] = src[i];
                break;
            case 48:
                for (dword i = 0; i < 48; ++i)
                    dst[i] = src[i];
                break;
            case 64:
                for (dword i = 0; i < 64; ++i)
                    dst[i] = src[i];
                break;
            default:
                if (copies != 0)
                    std::memcpy(dst, src, copies * sizeof(dword));
                break;
            }
            frame = &callee;
            regs = callee.registers.data();
            ipc = target;
            if (callStack.size() > peakDepth)
                peakDepth = callStack.size();
            break;
        }
        case ISA::ret:
        {
            const dword value = regs[VM_A];
            const byte returnRegister = frame->returnRegister;
            const dword returnAddress = frame->returnAddress;
            callStack.pop_back();
            if (callStack.empty())
            {
                frame = nullptr;
                regs = nullptr;
                status = CORE::IDLE;
                VM_SYNC();
                return STEP_HALT;
            }
            frame = &callStack.back();
            regs = frame->registers.data();
            regs[returnRegister] = value;
            ipc = returnAddress;
            break;
        }
        case ISA::hostcall:
        {
            // Re-read the slots every call: attachImports() stays visible and
            // the hostcall path is slow relative to the dispatch anyway.
            const CORE::HostSlot *slots = hostSlots.data();
            const std::size_t slotCount = hostSlots.size();
            const dword index =
                static_cast<dword>(merge(static_cast<byte>(VM_C), static_cast<byte>(VM_B)));
            if (index >= slotCount || slots[index].function == nullptr ||
                slots[index].arity < 0 || slots[index].arity > ISA::REGISTER_COUNT)
                VM_FAULT(CORE::INVALID_IMPORT);
            // instructionCount()/currentPc() must be observable in host code.
            executed += count;
            count = 0;
            programCounter = ipc;
            const CORE::HostSlot &slot = slots[index];
            regs[VM_A] = slot.function(slot.user, regs, slot.arity);
            if constexpr (GuardMask == 0)
            {
                // The host function may have changed the guard state.
                if (fuelLimit == 0 && interruptHook == nullptr && debugHook == nullptr)
                    break;
            }
            VM_SYNC();
            return STEP_HOSTCALL;
        }
        default:
        {
            // Cold opcode: hand the instruction to the shared implementation.
            const dword instructionPc = ipc - ISA::INSTRUCTION_SIZE;
            pc = ipc;
            executedLocal = count;
            const int result = step(frame, pc, executedLocal, inst);
            ipc = pc;
            count = executedLocal;
            regs = frame != nullptr ? frame->registers.data() : nullptr;
            if (result == STEP_HALT)
            {
                VM_SYNC();
                return STEP_HALT;
            }
            if (result == STEP_HOSTCALL)
            {
                // Kept for completeness; hostcall is handled inline above.
                VM_SYNC();
                return STEP_HOSTCALL;
            }
            if (result != CORE::OK)
            {
                lastError = result;
                lastErrorPc = instructionPc;
                VM_SYNC();
                return result;
            }
            break;
        }
        }

        if constexpr (GUARD_INTERRUPT || GUARD_DEBUG)
        {
            if (guardsDirty)
            {
                VM_SYNC();
                return STEP_HOSTCALL;
            }
        }
    }

#undef VM_A
#undef VM_B
#undef VM_C
#undef VM_OFF16
#undef VM_OFF24
#undef VM_SYNC
#undef VM_FAULT
#undef VM_GUARDS_DIRTY_CHECK
#undef VM_TAKEN_BARRIER
#undef VM_COMPILER_BARRIER
}

template <>
int Core::execute<false>(StackFrame *&frame, dword &pc, dword &executedLocal)
{
    if (verified)
        return executeHot<false, 0>(frame, pc, executedLocal);
    return executeHot<true, 0>(frame, pc, executedLocal);
}

// Guarded dispatch: pick the compile-time specialization for the guard state
// that run() observed. The hot loop preserves the historical check order
// (fuel -> executed -> interrupt -> bounds -> fetch -> debug -> dispatch).
template <>
int Core::execute<true>(StackFrame *&frame, dword &pc, dword &executedLocal)
{
    unsigned mask = 0;
    if (fuelLimit != 0)
        mask |= 1u;
    if (interruptHook != nullptr)
        mask |= 2u;
    if (debugHook != nullptr)
        mask |= 4u;

    switch (mask)
    {
    case 1:
        return executeHot<true, 1>(frame, pc, executedLocal);
    case 2:
        return executeHot<true, 2>(frame, pc, executedLocal);
    case 3:
        return executeHot<true, 3>(frame, pc, executedLocal);
    case 4:
        return executeHot<true, 4>(frame, pc, executedLocal);
    case 5:
        return executeHot<true, 5>(frame, pc, executedLocal);
    case 6:
        return executeHot<true, 6>(frame, pc, executedLocal);
    case 7:
        return executeHot<true, 7>(frame, pc, executedLocal);
    default:
        break;
    }
    if (verified)
        return executeHot<false, 0>(frame, pc, executedLocal);
    return executeHot<true, 0>(frame, pc, executedLocal);
}

void Core::run(dword functionIndex)
{
    callStack.clear();
    lastError = CORE::OK;
    lastErrorPc = 0;
    fuelLeft = fuelLimit;
    executed = 0;
    peakDepth = 1;
    lastTrap = 0;

    if (memory == nullptr || text.size() < ISA::INSTRUCTION_SIZE)
    {
        status = CORE::IDLE;
        return;
    }
    if (functionIndex >= functionTable.size())
    {
        lastError = CORE::INVALID_FUNCTION;
        status = CORE::IDLE;
        return;
    }

    dword pc = functionTable[functionIndex].entry * ISA::INSTRUCTION_SIZE;
    if (static_cast<std::size_t>(pc) + ISA::INSTRUCTION_SIZE > text.size())
    {
        lastError = CORE::INVALID_TARGET;
        status = CORE::IDLE;
        return;
    }

    if (callStack.capacity() < 64)
        callStack.reserve(64);
    callStack.emplace_back(); // root frame; zeroed explicitly below
    StackFrame *frame = &callStack.back();
    std::memset(frame->registers.data(), 0, sizeof(frame->registers));
    peakDepth = callStack.size();
    status = CORE::RUN;

    dword executedLocal = 0;
    for (;;)
    {
        const bool guarded = fuelLimit != 0 || interruptHook != nullptr || debugHook != nullptr;
        const int signal = guarded ? execute<true>(frame, pc, executedLocal)
                                   : execute<false>(frame, pc, executedLocal);
        if (signal == STEP_HOSTCALL)
            continue; // re-evaluate which loop to use; step() synced the boundary
        break;
    }

    executed += executedLocal;
    programCounter = pc;
    if (lastError == CORE::OK) // last executed instruction (debugger/trace post-mortem)
        lastErrorPc = pc >= ISA::INSTRUCTION_SIZE ? pc - ISA::INSTRUCTION_SIZE : 0;
    status = CORE::IDLE;
}

void Core::setFuel(dword maxInstructions)
{
    fuelLimit = maxInstructions;
}

void Core::setInterruptHook(bool (*hook)(void *), void *user)
{
    interruptHook = hook;
    interruptUser = user;
}

void Core::setDebugHook(CORE::DebugHook hook, void *user)
{
    debugHook = hook;
    debugUser = user;
}

dword Core::codeWord(dword address) const
{
    if (address > text.size() || text.size() - address < ISA::INSTRUCTION_SIZE)
        return 0;
    return static_cast<dword>(text[address]) |
           (static_cast<dword>(text[address + 1]) << 8) |
           (static_cast<dword>(text[address + 2]) << 16) |
           (static_cast<dword>(text[address + 3]) << 24);
}
