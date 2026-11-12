#pragma once
#include "parse.hpp"

#include <span>
#include <vector>

namespace PARSE
{
    // CALL edge collected while verifying: caller/callee are FUNCS indices.
    struct CallEdge
    {
        dword caller = 0;
        dword callee = 0;
    };

    // max-closure used by both the verifier and Core:
    //   extent[f] = max(extent[f], extent[g]) for every CALL edge f -> g
    void fixpointCopyExtents(std::vector<word> &extents, std::span<const CallEdge> edges);

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
        int verifyWordWith(dword pc, const Function &fn, dword instruction, const OpInfo &info);

    public:
        int verify(const Image &target);
        // Single in-order pass over CODE. Besides V51..V62 it seeds and returns
        // per-function copy extents (the CALL-graph max closure), so Core does
        // not have to rescan the whole CODE section on attach.
        int verify(const Image &target, std::vector<word> &copyExtents);
    };
}
