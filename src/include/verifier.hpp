#pragma once
#include "parse.hpp"

namespace PARSE
{
    // instruction formats
    enum class InstrFormat : byte
    {
        op = 0, // none
        r,      // a
        rr,     // a, b
        rr8,    // a, b + imm8
        rrr,    // a, b, c
        ri16,   // a + imm16
        i24,    // imm24
        call16  // a + index16
    };

    struct OpInfo
    {
        InstrFormat format = InstrFormat::op;
        byte regMask = 0;  // register bits
        byte zeroMask = 0; // zero bits
        bool terminator = false;
        bool known = false;
    };

    // null if reserved
    const OpInfo *opcodeInfo(byte opcode);

    class Verifier
    {
    private:
        const Image *image = nullptr;

        int verifyFunction(dword index);
        int verifyWord(dword pc, const Function &fn, dword instruction);

    public:
        int verify(const Image &target);
    };
}
