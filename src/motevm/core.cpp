#include "core.hpp"
#include "verifier.hpp"
#include <algorithm>
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

int Core::step(StackFrame *&frame, dword &pc, dword &executedLocal, byte instruction, byte op1,
               byte op2, byte op3)
{
    if (frame == nullptr)
        return CORE::NO_ACTIVE_FRAME;

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

int Core::attachModule(std::span<const byte> text, std::span<const PARSE::Function> functions)
{
    if (text.empty() || text.size() % ISA::INSTRUCTION_SIZE != 0)
        return CORE::INVALID_TEXT;
    for (const PARSE::Function &fn : functions)
        if (static_cast<qword>(fn.entry) * ISA::INSTRUCTION_SIZE + ISA::INSTRUCTION_SIZE > text.size())
            return CORE::INVALID_TARGET;
    const int attached = attachText(text);
    if (attached != CORE::OK)
        return attached;
    functionTable = functions;
    computeCopyExtents();
    return CORE::OK;
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
    std::vector<std::pair<dword, dword>> edges;
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
                    edges.emplace_back(static_cast<dword>(f), target);
            }
        }
        copyExtent[f] = static_cast<word>(extent);
    }

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto &edge : edges)
        {
            if (copyExtent[edge.first] < copyExtent[edge.second])
            {
                copyExtent[edge.first] = copyExtent[edge.second];
                changed = true;
            }
        }
    }
}

void Core::attachImports(std::span<const CORE::HostSlot> slots)
{
    hostSlots = slots;
}

int Core::attachText(std::span<const byte> text)
{
    if (text.empty() || text.size() % ISA::INSTRUCTION_SIZE != 0)
        return CORE::INVALID_TEXT;
    this->text = text;
    functionTable = {};
    hostSlots = {};
    copyExtent.clear();
    callStack.clear();
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
    callStack.clear();
    programCounter = 0;
    status = CORE::IDLE;
    lastError = CORE::OK;
    lastErrorPc = 0;
}

// Unguarded hot loop: no fuel, interrupt or debugger work. It returns only on
// halt/root-ret, a hostcall (so run() can re-evaluate the guard state) or an
// error. pc, the frame pointer and the instruction counter stay in locals.
template <>
int Core::execute<false>(StackFrame *&frame, dword &pc, dword &executedLocal)
{
    const byte *code = text.data();
    const std::size_t size = text.size();
    for (;;)
    {
        ++executedLocal;
        if (static_cast<std::size_t>(pc) + ISA::INSTRUCTION_SIZE > size)
        {
            lastError = CORE::INVALID_TARGET;
            lastErrorPc = pc;
            return CORE::INVALID_TARGET;
        }

        const dword instructionPc = pc;
        const dword word = loadWord(code + pc);
        pc += ISA::INSTRUCTION_SIZE;

        const int result = step(frame, pc, executedLocal, static_cast<byte>(word & ISA::OPCODE_MASK),
                                static_cast<byte>((word >> ISA::OPERAND1_SHIFT) & ISA::OPERAND_MASK),
                                static_cast<byte>((word >> ISA::OPERAND2_SHIFT) & ISA::OPERAND_MASK),
                                static_cast<byte>((word >> ISA::OPERAND3_SHIFT) & ISA::OPERAND_MASK));
        if (result == STEP_HALT)
            return STEP_HALT;
        if (result == STEP_HOSTCALL)
        {
            // A host function may have changed the guard state; only fall back
            // to the guarded loop when that actually happened.
            if (fuelLimit == 0 && interruptHook == nullptr && debugHook == nullptr)
                continue;
            return STEP_HOSTCALL;
        }
        if (result != CORE::OK)
        {
            lastError = result;
            lastErrorPc = instructionPc;
            return result;
        }
    }
}

// Guarded loop: preserves the historical check order exactly
// (fuel -> executed -> interrupt -> bounds -> fetch -> debug -> dispatch).
template <>
int Core::execute<true>(StackFrame *&frame, dword &pc, dword &executedLocal)
{
    const byte *code = text.data();
    const std::size_t size = text.size();
    for (;;)
    {
        if (fuelLimit != 0) // 0 = unlimited
        {
            if (fuelLeft == 0)
            {
                lastError = CORE::FUEL_EXHAUSTED;
                return CORE::FUEL_EXHAUSTED;
            }
            --fuelLeft;
        }

        ++executedLocal;
        if (interruptHook != nullptr)
        {
            executed += executedLocal;
            executedLocal = 0;
            programCounter = pc;
            if (interruptHook(interruptUser))
            {
                lastError = CORE::INTERRUPTED;
                return CORE::INTERRUPTED;
            }
        }

        if (static_cast<std::size_t>(pc) + ISA::INSTRUCTION_SIZE > size)
        {
            lastError = CORE::INVALID_TARGET;
            lastErrorPc = pc;
            return CORE::INVALID_TARGET;
        }

        const dword instructionPc = pc;
        const dword word = loadWord(code + pc);

        // The debugger hook sees pc of the unexecuted instruction and a counter
        // that already includes it.
        if (debugHook != nullptr)
        {
            executed += executedLocal;
            executedLocal = 0;
            programCounter = pc;
            if (debugHook(debugUser, *this, pc, word))
            {
                lastError = CORE::DEBUG_BREAK;
                lastErrorPc = pc;
                return CORE::DEBUG_BREAK;
            }
        }

        pc += ISA::INSTRUCTION_SIZE;

        const int result = step(frame, pc, executedLocal, static_cast<byte>(word & ISA::OPCODE_MASK),
                                static_cast<byte>((word >> ISA::OPERAND1_SHIFT) & ISA::OPERAND_MASK),
                                static_cast<byte>((word >> ISA::OPERAND2_SHIFT) & ISA::OPERAND_MASK),
                                static_cast<byte>((word >> ISA::OPERAND3_SHIFT) & ISA::OPERAND_MASK));
        if (result == STEP_HALT)
            return STEP_HALT;
        if (result == STEP_HOSTCALL)
            continue; // step() already synced executed/programCounter
        if (result != CORE::OK)
        {
            lastError = result;
            lastErrorPc = instructionPc;
            return result;
        }
    }
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
