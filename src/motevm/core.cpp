#include "core.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace
{
    const dword FLOAT_SIGN_BIT = 0x80000000u;
    const dword FLOAT_ABS_MASK = 0x7fffffffu;

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

int Core::step(byte instruction, byte op1, byte op2, byte op3)
{
    if (nowFrame == nullptr)
        return CORE::NO_ACTIVE_FRAME;

    std::array<dword, ISA::REGISTER_COUNT> &reg = nowFrame->registers;
    int result = CORE::OK;
    switch (static_cast<int>(instruction))
    {
    case ISA::zero:
        reg[op1] = 0;
        break;
    case ISA::load:
        if (memory->read(reg[op1], reg[op2] + op3) != MEMORY::OK)
            result = CORE::MEMORY_FAULT;
        break;
    case ISA::sto:
        if (memory->write(reg[op1], reg[op2] + op3) != MEMORY::OK)
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
        result = saveContext(pc, op1); // pc already points to the next instruction
        if (result == CORE::OK)
            pc = target;
        break;
    }
    case ISA::ret:
    {
        const dword value = reg[op1];
        result = takeContext(value);
        if (result == CORE::NO_CALLER)
            status = CORE::IDLE, result = CORE::OK;
        break;
    }
    case ISA::halt:
        status = CORE::IDLE; // stop execution
        break;
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
        const CORE::HostSlot &slot = hostSlots[index];
        reg[op1] = slot.function(slot.user, reg.data(), slot.arity);
        break;
    }
    default:
        result = CORE::UNKNOWN_INSTRUCTION;
        break;
    }
    return result;
}

int Core::saveContext(dword returnAddress, byte returnRegister)
{
    if (nowFrame == nullptr)
        return CORE::NO_ACTIVE_FRAME;
    if (callStack.size() >= CORE::MAX_CALL_DEPTH)
        return CORE::STACK_OVERFLOW;
    StackFrame callee(returnAddress, returnRegister);
    callee.registers = nowFrame->registers; // whole-window copy: arguments in r0..
    callStack.push_back(std::move(callee));
    nowFrame = &callStack.back(); // re-seat: vector growth may move the frames
    if (callStack.size() > peakDepth)
        peakDepth = callStack.size();
    return CORE::OK;
}

int Core::takeContext(dword value)
{
    if (nowFrame == nullptr || callStack.empty())
        return CORE::NO_ACTIVE_FRAME;
    const byte returnRegister = nowFrame->returnRegister;
    const dword returnAddress = nowFrame->returnAddress;
    callStack.pop_back();
    if (callStack.empty())
    {
        nowFrame = nullptr;
        return CORE::NO_CALLER;
    }
    nowFrame = &callStack.back();
    nowFrame->registers[returnRegister] = value;
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
    return CORE::OK;
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
    callStack.clear();
    programCounter = 0;
    nowFrame = nullptr;
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
    callStack.clear();
    programCounter = 0;
    nowFrame = nullptr;
    status = CORE::IDLE;
    lastError = CORE::OK;
    lastErrorPc = 0;
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

    pc = functionTable[functionIndex].entry * ISA::INSTRUCTION_SIZE;
    if (static_cast<std::size_t>(pc) + ISA::INSTRUCTION_SIZE > text.size())
    {
        lastError = CORE::INVALID_TARGET;
        status = CORE::IDLE;
        return;
    }

    callStack.emplace_back(); // root frame: zeroed registers, not saved via saveContext
    nowFrame = &callStack.back();
    peakDepth = callStack.size();
    status = CORE::RUN;

    while (status == CORE::RUN)
    {
        if (fuelLimit != 0) // 0 = unlimited
        {
            if (fuelLeft == 0)
            {
                lastError = CORE::FUEL_EXHAUSTED;
                break;
            }
            --fuelLeft;
        }
        ++executed;
        if (interruptHook != nullptr && interruptHook(interruptUser))
        {
            lastError = CORE::INTERRUPTED;
            break;
        }

        if (static_cast<std::size_t>(pc) + ISA::INSTRUCTION_SIZE > text.size())
        {
            lastError = CORE::INVALID_TARGET;
            lastErrorPc = pc;
            break;
        }

        const dword instructionPc = pc;
        // Explicit little-endian decode to stay independent of host endianness.
        const dword word = static_cast<dword>(text[pc]) |
                           (static_cast<dword>(text[pc + 1]) << 8) |
                           (static_cast<dword>(text[pc + 2]) << 16) |
                           (static_cast<dword>(text[pc + 3]) << 24);

        // Debugger hook: pc still points at the instruction that is about to run,
        // so a stopped debugger sees "the next instruction to execute".
        if (debugHook != nullptr && debugHook(debugUser, *this, pc, word))
        {
            lastError = CORE::DEBUG_BREAK;
            lastErrorPc = pc;
            break;
        }

        pc += ISA::INSTRUCTION_SIZE;

        const int result = step(static_cast<byte>(word & ISA::OPCODE_MASK),
                                static_cast<byte>((word >> ISA::OPERAND1_SHIFT) & ISA::OPERAND_MASK),
                                static_cast<byte>((word >> ISA::OPERAND2_SHIFT) & ISA::OPERAND_MASK),
                                static_cast<byte>((word >> ISA::OPERAND3_SHIFT) & ISA::OPERAND_MASK));
        if (result != CORE::OK)
        {
            lastError = result;
            lastErrorPc = instructionPc;
            break;
        }
    }

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
