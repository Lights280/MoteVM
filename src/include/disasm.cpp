#include "disasm.hpp"

#include <array>
#include <iomanip>
#include <sstream>

#include "verifier.hpp"

namespace
{
    using MnemonicTable = std::array<const char *, 0x61>;

    // Every opcode the verifier accepts has a mnemonic here; reserved slots stay
    // nullptr so disassembly and trace never invent names.
    const MnemonicTable &mnemonics(void)
    {
        static const MnemonicTable table = [] {
            MnemonicTable names{};
            names[ISA::zero] = "zero";
            names[ISA::load] = "load";
            names[ISA::sto] = "sto";
            names[ISA::lhi] = "lhi";
            names[ISA::llo] = "llo";
            names[ISA::mov] = "mov";
            names[ISA::add] = "add";
            names[ISA::sub] = "sub";
            names[ISA::mul] = "mul";
            names[ISA::div] = "div";
            names[ISA::mod] = "mod";
            names[ISA::neg] = "neg";
            names[ISA::addu] = "addu";
            names[ISA::subu] = "subu";
            names[ISA::mulu] = "mulu";
            names[ISA::divu] = "divu";
            names[ISA::modu] = "modu";
            names[ISA::band] = "band";
            names[ISA::bor] = "bor";
            names[ISA::bxor] = "bxor";
            names[ISA::bnot] = "bnot";
            names[ISA::shl] = "shl";
            names[ISA::shr] = "shr";
            names[ISA::sar] = "sar";
            names[ISA::fadd] = "fadd";
            names[ISA::fsub] = "fsub";
            names[ISA::fmul] = "fmul";
            names[ISA::fdiv] = "fdiv";
            names[ISA::fneg] = "fneg";
            names[ISA::fabs] = "fabs";
            names[ISA::fsqrt] = "fsqrt";
            names[ISA::i2f] = "i2f";
            names[ISA::f2i] = "f2i";
            names[ISA::feq] = "feq";
            names[ISA::fne] = "fne";
            names[ISA::flt] = "flt";
            names[ISA::fle] = "fle";
            names[ISA::fgt] = "fgt";
            names[ISA::fge] = "fge";
            names[ISA::eq] = "eq";
            names[ISA::ne] = "ne";
            names[ISA::lt] = "lt";
            names[ISA::le] = "le";
            names[ISA::gt] = "gt";
            names[ISA::ge] = "ge";
            names[ISA::ltu] = "ltu";
            names[ISA::leu] = "leu";
            names[ISA::gtu] = "gtu";
            names[ISA::geu] = "geu";
            names[ISA::jmp] = "jmp";
            names[ISA::jz] = "jz";
            names[ISA::jnz] = "jnz";
            names[ISA::call] = "call";
            names[ISA::ret] = "ret";
            names[ISA::halt] = "halt";
            names[ISA::breakpoint] = "breakpoint";
            names[ISA::trap] = "trap";
            names[ISA::hostcall] = "hostcall";
            return names;
        }();
        return table;
    }

    word merge(byte high, byte low)
    {
        return static_cast<word>((static_cast<word>(high) << 8) | low);
    }

    std::string hex(dword value, int width)
    {
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(width) << std::setfill('0') << value;
        return out.str();
    }
}

const char *DISASM::mnemonic(byte opcode)
{
    if (opcode >= 0x61)
        return nullptr;
    return mnemonics()[opcode];
}

std::string DISASM::formatOperands(byte opcode, byte a, byte b, byte c)
{
    const PARSE::OpInfo *info = PARSE::opcodeInfo(opcode);
    if (info == nullptr)
        return {};

    std::ostringstream out;
    switch (opcode)
    {
    case ISA::load:
    case ISA::sto:
        out << " r" << static_cast<int>(a) << ", r" << static_cast<int>(b) << ", +"
            << static_cast<int>(c);
        return out.str();
    case ISA::lhi:
    case ISA::llo:
        out << " r" << static_cast<int>(a) << ", " << hex(merge(c, b), 4);
        return out.str();
    case ISA::jmp:
        out << ' '
            << signExtend24(static_cast<dword>(a) | (static_cast<dword>(b) << 8) |
                            (static_cast<dword>(c) << 16));
        return out.str();
    case ISA::jz:
    case ISA::jnz:
        out << " r" << static_cast<int>(a) << ", " << signExtend16(merge(c, b));
        return out.str();
    case ISA::call:
    case ISA::hostcall:
        out << " r" << static_cast<int>(a) << ", #" << merge(c, b);
        return out.str();
    case ISA::trap:
        out << ' '
            << (static_cast<dword>(a) | (static_cast<dword>(b) << 8) |
                (static_cast<dword>(c) << 16));
        return out.str();
    default:
        break;
    }

    switch (info->format)
    {
    case PARSE::InstrFormat::op:
        break;
    case PARSE::InstrFormat::r:
        out << " r" << static_cast<int>(a);
        break;
    case PARSE::InstrFormat::rr:
        out << " r" << static_cast<int>(a) << ", r" << static_cast<int>(b);
        break;
    case PARSE::InstrFormat::rr8:
        out << " r" << static_cast<int>(a) << ", r" << static_cast<int>(b) << ", "
            << static_cast<int>(c);
        break;
    case PARSE::InstrFormat::rrr:
        out << " r" << static_cast<int>(a) << ", r" << static_cast<int>(b) << ", r"
            << static_cast<int>(c);
        break;
    case PARSE::InstrFormat::ri16:
        out << " r" << static_cast<int>(a) << ", " << hex(merge(c, b), 4);
        break;
    case PARSE::InstrFormat::i24:
        out << ' '
            << (static_cast<dword>(a) | (static_cast<dword>(b) << 8) |
                (static_cast<dword>(c) << 16));
        break;
    case PARSE::InstrFormat::call16:
        out << " r" << static_cast<int>(a) << ", #" << merge(c, b);
        break;
    }
    return out.str();
}

std::string DISASM::format(dword word)
{
    const byte opcode = static_cast<byte>(word & ISA::OPCODE_MASK);
    const byte a = static_cast<byte>((word >> ISA::OPERAND1_SHIFT) & ISA::OPERAND_MASK);
    const byte b = static_cast<byte>((word >> ISA::OPERAND2_SHIFT) & ISA::OPERAND_MASK);
    const byte c = static_cast<byte>((word >> ISA::OPERAND3_SHIFT) & ISA::OPERAND_MASK);

    const char *name = mnemonic(opcode);
    if (name == nullptr)
        return ".word " + hex(word, 8);
    return std::string(name) + formatOperands(opcode, a, b, c);
}
