#pragma once
// Canonical instruction text for the self-ISA (plan/self/isa.md).
// Shared by -d stop context, trace output and the debugger REPL so every
// surface renders an instruction the same way.

#include <string>

#include "define.hpp"

namespace DISASM
{
    // Canonical mnemonic; nullptr when the opcode is reserved.
    const char *mnemonic(byte opcode);

    // Formats a whole 32-bit instruction word ("llo r0, 0x0003").
    std::string format(dword word);

    // Formats only the operand list, including the leading space.
    std::string formatOperands(byte opcode, byte a, byte b, byte c);

    inline int signExtend16(word value) { return static_cast<short>(value); }
    inline int signExtend24(dword value)
    {
        const dword bits = value & 0x00FFFFFFu;
        return (bits & 0x00800000u) ? static_cast<int>(bits | 0xFF000000u)
                                    : static_cast<int>(bits);
    }
}
