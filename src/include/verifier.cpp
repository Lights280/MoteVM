#include "verifier.hpp"

#include <array>
#include <cstdint>

namespace
{
    constexpr byte RA = 0x1;
    constexpr byte RB = 0x2;
    constexpr byte RC = 0x4;

    constexpr std::array<PARSE::OpInfo, 256> makeOpcodeTable(void)
    {
        std::array<PARSE::OpInfo, 256> table{};

        const auto set = [&table](byte opcode, PARSE::InstrFormat format, byte regMask, byte zeroMask, bool terminator)
        {
            table[opcode] = PARSE::OpInfo{format, regMask, zeroMask, terminator, true};
        };

        // 0x0x
        set(0x00, PARSE::InstrFormat::r, RA, RB | RC, false);  // zero
        set(0x01, PARSE::InstrFormat::rr8, RA | RB, 0, false); // load
        set(0x02, PARSE::InstrFormat::rr8, RA | RB, 0, false); // sto
        set(0x03, PARSE::InstrFormat::ri16, RA, 0, false);     // lhi
        set(0x04, PARSE::InstrFormat::ri16, RA, 0, false);     // llo

        // 0x1x
        set(0x10, PARSE::InstrFormat::rr, RA | RB, RC, false); // mov
        for (byte opcode = 0x11; opcode <= 0x15; ++opcode)
            set(opcode, PARSE::InstrFormat::rrr, RA | RB | RC, 0, false); // add..mod
        set(0x16, PARSE::InstrFormat::rr, RA | RB, RC, false);            // neg
        for (byte opcode = 0x17; opcode <= 0x1B; ++opcode)
            set(opcode, PARSE::InstrFormat::rrr, RA | RB | RC, 0, false); // addu..modu

        // 0x2x
        for (byte opcode = 0x20; opcode <= 0x22; ++opcode)
            set(opcode, PARSE::InstrFormat::rrr, RA | RB | RC, 0, false); // bit ops
        set(0x23, PARSE::InstrFormat::rr, RA | RB, RC, false);            // bnot
        for (byte opcode = 0x24; opcode <= 0x26; ++opcode)
            set(opcode, PARSE::InstrFormat::rrr, RA | RB | RC, 0, false); // shl/shr/sar

        // 0x3x
        for (byte opcode = 0x30; opcode <= 0x33; ++opcode)
            set(opcode, PARSE::InstrFormat::rrr, RA | RB | RC, 0, false); // fadd..fdiv
        for (byte opcode = 0x34; opcode <= 0x38; ++opcode)
            set(opcode, PARSE::InstrFormat::rr, RA | RB, RC, false); // fneg..f2i
        for (byte opcode = 0x39; opcode <= 0x3E; ++opcode)
            set(opcode, PARSE::InstrFormat::rrr, RA | RB | RC, 0, false); // feq..fge

        // 0x4x
        for (byte opcode = 0x40; opcode <= 0x49; ++opcode)
            set(opcode, PARSE::InstrFormat::rrr, RA | RB | RC, 0, false); // eq..geu

        // 0x5x
        set(0x50, PARSE::InstrFormat::i24, 0, 0, true);            // jmp
        set(0x51, PARSE::InstrFormat::ri16, RA, 0, false);         // jz
        set(0x52, PARSE::InstrFormat::ri16, RA, 0, false);         // jnz
        set(0x53, PARSE::InstrFormat::call16, RA, 0, false);       // call
        set(0x54, PARSE::InstrFormat::r, RA, RB | RC, true);       // ret
        set(0x55, PARSE::InstrFormat::op, 0, RA | RB | RC, true);  // halt
        set(0x56, PARSE::InstrFormat::op, 0, RA | RB | RC, false); // breakpoint
        set(0x57, PARSE::InstrFormat::i24, 0, 0, true);            // trap

        // 0x6x
        set(0x60, PARSE::InstrFormat::call16, RA, 0, false); // hostcall

        return table;
    }

    constexpr std::array<PARSE::OpInfo, 256> OPCODE_TABLE = makeOpcodeTable();

    inline dword at32(std::span<const byte> data, qword offset)
    {
        return static_cast<dword>(data[static_cast<std::size_t>(offset)]) |
               (static_cast<dword>(data[static_cast<std::size_t>(offset) + 1]) << 8) |
               (static_cast<dword>(data[static_cast<std::size_t>(offset) + 2]) << 16) |
               (static_cast<dword>(data[static_cast<std::size_t>(offset) + 3]) << 24);
    }

    inline std::int32_t signExtend16(word value)
    {
        return static_cast<std::int16_t>(value);
    }

    inline std::int32_t signExtend24(dword value)
    {
        const dword bits = value & 0x00FFFFFFu;
        return (bits & 0x00800000u) ? static_cast<std::int32_t>(bits | 0xFF000000u)
                                    : static_cast<std::int32_t>(bits);
    }

    // L1: masked fields are 0
    inline bool unusedFieldsZero(dword instruction, const PARSE::OpInfo &info)
    {
        const dword fields = instruction >> 8;
        if ((info.zeroMask & 0x1) != 0 && (fields & 0x000000FFu) != 0)
            return false;
        if ((info.zeroMask & 0x2) != 0 && (fields & 0x0000FF00u) != 0)
            return false;
        if ((info.zeroMask & 0x4) != 0 && (fields & 0x00FF0000u) != 0)
            return false;
        return true;
    }
}

const PARSE::OpInfo *PARSE::opcodeInfo(byte opcode)
{
    const PARSE::OpInfo &info = OPCODE_TABLE[opcode];
    return info.known ? &info : nullptr;
}

int PARSE::Verifier::verify(const Image &target)
{
    image = &target;

    // V51: all words decode
    const qword words = image->code.size() / ISA::INSTRUCTION_SIZE;
    for (qword i = 0; i < words; ++i)
    {
        const dword instruction = at32(image->code, i * ISA::INSTRUCTION_SIZE);
        const PARSE::OpInfo *info = PARSE::opcodeInfo(static_cast<byte>(instruction & 0xFFu));
        if (info == nullptr)
            return PARSE::BAD_CODE;
        if (!unusedFieldsZero(instruction, *info))
            return PARSE::BAD_CODE;
    }

    // V53..V62
    for (dword i = 0; i < static_cast<dword>(image->functions.size()); ++i)
    {
        const int result = verifyFunction(i);
        if (result != PARSE::OK)
            return result;
    }
    return PARSE::OK;
}

int PARSE::Verifier::verifyFunction(dword index)
{
    const PARSE::Function &fn = image->functions[index];
    for (dword pc = fn.entry; pc < fn.entry + fn.instr; ++pc)
    {
        const dword word = at32(image->code, static_cast<qword>(pc) * ISA::INSTRUCTION_SIZE);
        const int result = verifyWord(pc, fn, word);
        if (result != PARSE::OK)
            return result;
    }

    // V62: last terminates
    const dword last = at32(image->code, static_cast<qword>(fn.entry + fn.instr - 1) * ISA::INSTRUCTION_SIZE);
    const PARSE::OpInfo *info = PARSE::opcodeInfo(static_cast<byte>(last & 0xFFu));
    if (info == nullptr || !info->terminator)
        return PARSE::BAD_CODE;
    return PARSE::OK;
}

int PARSE::Verifier::verifyWord(dword pc, const Function &fn, dword instruction)
{
    const byte opcode = static_cast<byte>(instruction & 0xFFu);
    const PARSE::OpInfo *info = PARSE::opcodeInfo(opcode);
    if (info == nullptr)
        return PARSE::BAD_CODE;

    // L1
    if (!unusedFieldsZero(instruction, *info))
        return PARSE::BAD_CODE;

    const byte a = static_cast<byte>((instruction >> 8) & 0xFFu);
    const byte b = static_cast<byte>((instruction >> 16) & 0xFFu);
    const byte c = static_cast<byte>((instruction >> 24) & 0xFFu);
    const dword regLimit = fn.regcnt; // 1..256

    // V53
    if ((info->regMask & 0x1) != 0 && a >= regLimit)
        return PARSE::BAD_CODE;
    if ((info->regMask & 0x2) != 0 && b >= regLimit)
        return PARSE::BAD_CODE;
    if ((info->regMask & 0x4) != 0 && c >= regLimit)
        return PARSE::BAD_CODE;

    switch (opcode)
    {
    case ISA::jmp: // V61
    {
        const std::int32_t offset = signExtend24(instruction >> 8);
        if (offset % ISA::INSTRUCTION_SIZE != 0)
            return PARSE::BAD_CODE;
        const std::int64_t target = static_cast<std::int64_t>(pc) + 1 + offset / ISA::INSTRUCTION_SIZE;
        if (target < static_cast<std::int64_t>(fn.entry) ||
            target >= static_cast<std::int64_t>(fn.entry) + fn.instr)
            return PARSE::BAD_CODE;
        break;
    }
    case ISA::jz:
    case ISA::jnz:
    {
        const std::int32_t offset = signExtend16(static_cast<word>((instruction >> 16) & 0xFFFFu));
        if (offset % ISA::INSTRUCTION_SIZE != 0)
            return PARSE::BAD_CODE;
        const std::int64_t target = static_cast<std::int64_t>(pc) + 1 + offset / ISA::INSTRUCTION_SIZE;
        if (target < static_cast<std::int64_t>(fn.entry) ||
            target >= static_cast<std::int64_t>(fn.entry) + fn.instr)
            return PARSE::BAD_CODE;
        break;
    }
    case ISA::call: // V56/V59: FUNCS index
    {
        const dword index = (instruction >> 16) & 0xFFFFu;
        if (index >= image->functions.size())
            return PARSE::BAD_CODE;
        if (image->functions[index].arity > fn.regcnt)
            return PARSE::BAD_CODE;
        break;
    }
    case ISA::hostcall: // V56/V60: IMPORTS index
    {
        const dword index = (instruction >> 16) & 0xFFFFu;
        if (index >= image->imports.size())
            return PARSE::BAD_CODE;
        if (image->imports[index].arity > fn.regcnt)
            return PARSE::BAD_CODE;
        break;
    }
    case ISA::trap: // V57
    {
        const dword code = instruction >> 8;
        if (code < 1 || code > 16)
            return PARSE::BAD_CODE;
        break;
    }
    default:
        break;
    }
    return PARSE::OK;
}
