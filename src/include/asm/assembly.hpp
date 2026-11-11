#pragma once
// Two-pass assembler model for the self-ISA (plan/self/isa.md).
// The .mbc container layout is produced by asm/emitter.cpp and validated
// against the same Parse/Verifier implementation used by motevm.

#include <cstdint>
#include <string>
#include <vector>

#include "common/types.hpp"
#include "define.hpp"
#include "parse.hpp"

namespace ASM
{
    struct ConstantValue
    {
        PARSE::ConstKind kind = PARSE::ConstKind::null;
        std::uint64_t bits = 0; // boolean/integer/real/null, little-endian payload
        std::string bytes;      // string payload
    };

    struct Constant
    {
        bool named = false;
        std::string name; // named mode only; not stored in .mbc
        ConstantValue value;
        TOOL::SourcePosition position;
    };

    struct MetaEntry
    {
        word key = 0;
        std::string value;
        TOOL::SourcePosition position;
    };

    // Reference to a name constant. Name = identifier resolved by content,
    // Raw = canonical #n fallback (explicit CONST index), Anonymous = no name.
    struct NameRef
    {
        enum class Kind
        {
            Name,
            Raw,
            Anonymous
        };

        Kind kind = Kind::Name;
        std::string name;
        dword index = NO_INDEX;
        TOOL::SourcePosition position;
    };

    struct Import
    {
        NameRef name;
        dword arity = 0;
        dword flags = 0;
        TOOL::SourcePosition position;
    };

    struct Global
    {
        NameRef name;
        bool isConst = false;
        bool hasInit = false;
        bool initRaw = false; // #n instead of a literal
        ConstantValue init;
        dword initIndex = NO_INDEX;
        TOOL::SourcePosition position;
        TOOL::SourcePosition initPos;
    };

    struct Operand
    {
        enum class Kind
        {
            None,
            Reg,
            Imm,
            Index,
            Name,
            Relative,
            Trap
        };

        Kind kind = Kind::None;
        dword value = 0;           // register / immediate / raw index / trap code
        std::int64_t offset = 0;   // Relative (bytes)
        std::string text;          // name or trap name
        TOOL::SourcePosition position;
    };

    struct Instruction
    {
        byte opcode = 0;
        Operand a;
        Operand b;
        Operand c;
        TOOL::SourcePosition position;
        std::string source;
    };

    struct Label
    {
        std::string name;
        bool local = false;
        dword pc = 0;
        TOOL::SourcePosition position;
    };

    struct LineRecord
    {
        dword pc = 0;
        dword line = 0;
        dword column = 0;
    };

    struct Local
    {
        NameRef name;
        dword slot = 0;
        bool param = false;
        bool hasStart = false;
        bool hasEnd = false;
        bool startIsLabel = false;
        bool endIsLabel = false;
        dword startPc = 0;
        dword endPc = 0;
        std::string startLabel;
        std::string endLabel;
        TOOL::SourcePosition position;
    };

    struct Function
    {
        NameRef name;
        dword arity = 0;
        dword regs = 0;
        bool autoRegs = false;
        dword flags = 0;
        dword entryPc = 0;
        std::vector<Instruction> code;
        std::vector<dword> words; // encoded CODE words, filled in pass 2
        std::vector<Label> labels;
        std::vector<LineRecord> lines;
        std::vector<Local> locals;
        dword dbgOffset = NO_INDEX;
        word localCount = 0;
        TOOL::SourcePosition position;
        std::string displayName; // diagnostics/listing ("anonymous" or source name)
    };

    enum class ConstMode
    {
        Unset,
        Named,
        Explicit
    };

    struct Module
    {
        bool hasModule = false;
        std::string moduleName;
        TOOL::SourcePosition modulePos;
        std::vector<MetaEntry> meta;
        ConstMode mode = ConstMode::Unset;
        std::vector<Constant> constants;
        std::vector<Import> imports;
        std::vector<Global> globals;
        bool hasEntry = false;
        NameRef entry;
        std::vector<Function> functions;
        dword entryFunction = 0;
    };

    struct ListingEntry
    {
        std::string function;
        dword funcIndex = 0;
        dword pc = 0;
        dword word = 0;
        std::string source;
    };

    struct AssembleResult
    {
        int status = TOOL::OK;
        std::vector<byte> image;
        std::vector<ListingEntry> listing;
        std::string moduleName;
        std::size_t constantCount = 0;
        std::size_t functionCount = 0;
        std::size_t globalCount = 0;
        std::size_t importCount = 0;
        dword instructionCount = 0;
    };
}
