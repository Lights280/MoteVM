#pragma once

typedef unsigned char byte, uchar, u8;
typedef unsigned short word, ushort, u16;
typedef unsigned int dword, uint, u32;
typedef unsigned long long qword, u64;
// NOTE: do not alias `ulong` here. glibc's <sys/types.h> (pulled in by
// <cstdlib>/<string> on Linux) defines `typedef unsigned long ulong;` under
// __USE_MISC, and redefining it as `unsigned long long` is a hard error.
// The other aliases below (uchar/ushort/uint/byte/word/dword) happen to match
// glibc's types exactly, so identical redefinition is legal; `ulong` does not.

// versions (single source for tools and documents)
inline constexpr const char *MOTEVM_VERSION = "0.1.0";
inline constexpr const char *MOTEBC_VERSION = "1.0";

// shared constants
inline constexpr dword NO_INDEX = 0xFFFFFFFFu; // anonymous / null / no debug
inline constexpr qword align8(qword value) { return (value + 7) & ~qword(7); }

namespace ISA
{
    constexpr int REGISTER_COUNT = 256; // 8-bit register field
    constexpr int INSTRUCTION_SIZE = 4; // bytes per instruction

    constexpr dword OPCODE_MASK = 0x000000FFu;
    constexpr dword OPERAND_MASK = 0x000000FFu;
    constexpr int OPERAND1_SHIFT = 8;
    constexpr int OPERAND2_SHIFT = 16;
    constexpr int OPERAND3_SHIFT = 24;

    // operand layouts
    enum Format : byte
    {
        fmt_op = 0, // op
        fmt_r,      // op a
        fmt_rr,     // op a, b
        fmt_rrr,    // op a, b, c
        fmt_ri16,   // op a, imm16 = (c << 8) | b
        fmt_i24,    // op imm24 = (c << 16) | (b << 8) | a
        fmt_call16  // op a, index16 = (c << 8) | b
    };

    enum Immediate : int // 0x0x: immediate and memory
    {
        zero = 0x00, // r[a] = 0
        load = 0x01, // r[a] = memory[r[b] + c]
        sto = 0x02,  // memory[r[b] + c] = r[a]
        lhi = 0x03,  // r[a] = (r[a] & 0x0000FFFF) | (imm16 << 16)
        llo = 0x04,  // r[a] = (r[a] & 0xFFFF0000) | imm16
        immediate_end
    };
    static_assert(Immediate::immediate_end <= 0x10);

    enum Integer : int // 0x1x: 32-bit integer
    {
        mov = 0x10,  // r[a] = r[b]
        add = 0x11,  // r[a] = r[b] + r[c]
        sub = 0x12,  // r[a] = r[b] - r[c]
        mul = 0x13,  // r[a] = r[b] * r[c]
        div = 0x14,  // r[a] = r[b] / r[c]
        mod = 0x15,  // r[a] = r[b] % r[c]
        neg = 0x16,  // r[a] = -r[b]
        addu = 0x17, // unsigned variants
        subu = 0x18,
        mulu = 0x19,
        divu = 0x1A,
        modu = 0x1B,
        integer_end
    };
    static_assert(Integer::integer_end <= 0x20);

    enum Logic : int // 0x2x: bitwise and shifts
    {
        band = 0x20, // r[a] = r[b] & r[c]
        bor = 0x21,  // r[a] = r[b] | r[c]
        bxor = 0x22, // r[a] = r[b] ^ r[c]
        bnot = 0x23, // r[a] = ~r[b]
        shl = 0x24,  // r[a] = r[b] << (r[c] & 31)
        shr = 0x25,  // logical right shift
        sar = 0x26,  // arithmetic right shift
        logic_end
    };
    static_assert(Logic::logic_end <= 0x30);

    // 0x30-0x3F: IEEE-754 binary32 calculation
    // Registers hold raw float32 bit patterns; use llo + lhi to build constants.
    enum Float : int
    {
        fadd = 0x30,  // r[a] = f32(r[b]) + f32(r[c])
        fsub = 0x31,  // r[a] = f32(r[b]) - f32(r[c])
        fmul = 0x32,  // r[a] = f32(r[b]) * f32(r[c])
        fdiv = 0x33,  // IEEE division; /0 -> Inf/NaN, no trap
        fneg = 0x34,  // r[a] = -f32(r[b]); c must be 0
        fabs = 0x35,  // r[a] = |f32(r[b])|; c must be 0
        fsqrt = 0x36, // r[a] = sqrt(f32(r[b])); c must be 0
        i2f = 0x37,   // r[a] = f32(int32(r[b])); c must be 0
        f2i = 0x38,   // r[a] = int32(f32(r[b])); c must be 0
        feq = 0x39,   // r[a] = (f32(b) == f32(c)) ? 1 : 0
        fne = 0x3A,   // unordered: NaN != NaN -> 1
        flt = 0x3B,   // r[a] = (f32(b) < f32(c)) ? 1 : 0
        fle = 0x3C,   // r[a] = (f32(b) <= f32(c)) ? 1 : 0
        fgt = 0x3D,   // r[a] = (f32(b) > f32(c)) ? 1 : 0
        fge = 0x3E,   // r[a] = (f32(b) >= f32(c)) ? 1 : 0
        float_end     // 0x3F remains reserved
    };
    static_assert(Float::float_end <= 0x40);

    enum Compare : int // 0x4x: result is 0/1 in r[a]
    {
        eq = 0x40,
        ne = 0x41,
        lt = 0x42,
        le = 0x43,
        gt = 0x44,
        ge = 0x45,
        ltu = 0x46, // unsigned comparisons
        leu = 0x47,
        gtu = 0x48,
        geu = 0x49,
        compare_end
    };
    static_assert(Compare::compare_end <= 0x50);

    enum Control : int // 0x5x
    {
        jmp = 0x50,        // pc = pc + 4 + int24
        jz = 0x51,         // if r[a] == 0: pc = pc + 4 + int16
        jnz = 0x52,        // if r[a] != 0: pc = pc + 4 + int16
        call = 0x53,       // push StackFrame; target = FUNCS[index16].entry_pc
        ret = 0x54,        // pop StackFrame; result = r[a]
        halt = 0x55,       // stop execution
        breakpoint = 0x56, // debug hook; NOP without debugger
        trap = 0x57,       // raise trap imm24
        control_end
    };
    static_assert(Control::control_end <= 0x60);

    enum System : int // 0x6x
    {
        hostcall = 0x60, // call imports[index16]; dest = r[a]
        system_end
    };
    static_assert(System::system_end <= 0x70);
}