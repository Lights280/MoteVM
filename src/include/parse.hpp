#pragma once
#include "define.hpp"
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace PARSE
{
    // status
    const int OK = 0;
    const int FAIL_OPEN = 1;   // open/read
    const int EMPTY = 2;       // no input
    const int TRUNC = 3;       // too short
    const int TOO_LARGE = 4;   // > MAX_MODULE_BYTES
    const int BAD_FILE = 5;    // header/CRC/table
    const int BAD_CONTENT = 6; // segment data
    const int BAD_CODE = 7;    // instruction rules

    // User-facing text for the status codes above.
    const char *errorText(int code);

    // consts
    inline constexpr byte MAGIC[4] = {0x4D, 0x4F, 0x54, 0x45}; // "MOTE"
    inline constexpr word SUPPORTED_MAJOR = 1;
    inline constexpr word SUPPORTED_MINOR = 0;

    inline constexpr qword HEADER_SIZE = 0x20;   // 32 B
    inline constexpr qword SECBLOCK_SIZE = 0x18; // 24 B

    inline constexpr dword FLAG_DEBUG_PRESENT = 0x1u;  // bit0 debug
    inline constexpr byte FUNC_FLAG_EXPORTED = 0x1u;   // bit0 exported
    inline constexpr dword GLOBAL_FLAG_CONST = 0x1u;   // bit0 const
    inline constexpr byte LOCAL_FLAG_PARAMETER = 0x1u; // bit0 parameter

    // section ids
    inline constexpr dword SECTION_META = 1;
    inline constexpr dword SECTION_CONST = 2;
    inline constexpr dword SECTION_FUNCS = 3;
    inline constexpr dword SECTION_CODE = 4;
    inline constexpr dword SECTION_GLOBALS = 5;
    inline constexpr dword SECTION_IMPORTS = 6;
    inline constexpr dword SECTION_DEBUG = 7;

    // keys
    inline constexpr word META_MODULE_NAME = 1;
    inline constexpr word META_SOURCE_FILE = 2;
    inline constexpr word META_BUILD_ID = 3;
    inline constexpr word META_COMMENT = 4;

    // limits
    inline constexpr dword MAX_STATIC_COUNT = 65536;
    inline constexpr qword MAX_MODULE_BYTES = 64ull << 20; // 64 MiB default
    inline constexpr qword MAX_STRING_BYTES = 16ull << 20;

    // header
    struct Header
    {
        byte magic[4];
        word majorVer;
        word minorVer;
        dword flags;     // bit0 debug
        dword entry;     // FUNCS index
        word seccnt;     // >= 6
        word reserved0;  // 0
        dword fileSize;  // == input size
        dword checksum;  // CRC-32C, field = 0
        dword reserved1; // 0
    };
    static_assert(sizeof(Header) == 0x20); // 32 B
    static_assert(offsetof(Header, flags) == 0x08);
    static_assert(offsetof(Header, entry) == 0x0C);
    static_assert(offsetof(Header, fileSize) == 0x14);
    static_assert(offsetof(Header, checksum) == 0x18);

    // section table
    struct SecBlock
    {
        dword id;     // 1..7 known
        dword flags;  // bit0 critical
        qword offset; // %8, >= table end
        qword size;   // %8, fits file
    };
    static_assert(sizeof(SecBlock) == 0x18); // 24 B
    static_assert(offsetof(SecBlock, flags) == 0x04);
    static_assert(offsetof(SecBlock, offset) == 0x08);
    static_assert(offsetof(SecBlock, size) == 0x10);

    // META
    struct MetaEntry
    {
        word key;                  // 1..4 known
        word length;               // bytes
        std::span<const byte> raw; // payload
    };
    static_assert(offsetof(MetaEntry, key) == 0x00);
    static_assert(offsetof(MetaEntry, length) == 0x02);

    // CONST
    enum class ConstKind : byte
    {
        null = 0x00,    // 8 zero bytes
        boolean = 0x01, // 0/1
        integer = 0x02, // int64
        real = 0x03,    // f64
        string = 0x04   // utf-8
    };

    struct ConstHeader
    {
        dword count;     // <= 65536
        dword reserved0; // 0
    };
    static_assert(sizeof(ConstHeader) == 0x08);

    struct ConstEntry
    {
        byte kind;     // ConstKind
        byte reserved; // 0
        word flags;    // 0
        dword length;  // payload_len
    };
    static_assert(sizeof(ConstEntry) == 0x08);
    static_assert(offsetof(ConstEntry, kind) == 0x00);
    static_assert(offsetof(ConstEntry, length) == 0x04);
    // 8 + align8(len)

    // FUNCS
    struct FuncHeader
    {
        dword count; // <= 65536
    };
    static_assert(sizeof(FuncHeader) == 0x04);

    struct Function
    {
        dword name;      // 0xFFFFFFFF anon
        dword entry;     // instruction index
        dword instr;     // >= 1
        word regcnt;     // 1..256
        byte arity;      // <= 16, <= regcnt
        byte flags;      // bit0 exported
        word local;      // named slots
        word reserved0;  // 0
        dword dbgOffset; // DEBUG offset or NO_INDEX
        dword reserved1; // 0
        dword reserved2; // 0
    };
    static_assert(sizeof(Function) == 0x20); // 32 B
    static_assert(offsetof(Function, regcnt) == 0x0C);
    static_assert(offsetof(Function, arity) == 0x0E);
    static_assert(offsetof(Function, flags) == 0x0F);
    static_assert(offsetof(Function, local) == 0x10);
    static_assert(offsetof(Function, dbgOffset) == 0x14);
    static_assert(offsetof(Function, reserved2) == 0x1C);
    // 8 + 32 * count

    // CODE
    inline constexpr dword CODE_NOP = 0x00000000;

    // GLOBALS
    struct GlobalHeader
    {
        dword count; // <= 65536
    };
    static_assert(sizeof(GlobalHeader) == 0x04);

    struct Global
    {
        dword name;  // 0xFFFFFFFF anon
        dword init;  // 0xFFFFFFFF null
        dword flags; // bit0 const
    };
    static_assert(sizeof(Global) == 0x0C); // 12 B
    static_assert(offsetof(Global, init) == 0x04);
    static_assert(offsetof(Global, flags) == 0x08);
    // 4 + 12 * count + pad

    // IMPORTS
    struct ImportHeader
    {
        dword count; // <= 65536
    };
    static_assert(sizeof(ImportHeader) == 0x04);

    struct Import
    {
        dword name;    // string const
        byte arity;    // <= 16
        byte flags;    // 0
        word reserved; // 0
    };
    static_assert(sizeof(Import) == 0x08); // 8 B
    static_assert(offsetof(Import, arity) == 0x04);
    static_assert(offsetof(Import, reserved) == 0x06);
    // 8 + 8 * count

    // DEBUG
    struct DebugHeader
    {
        byte version;   // 1
        byte reserved0; // 0
        word reserved1; // 0
        dword flags;    // 0
    };
    static_assert(sizeof(DebugHeader) == 0x08);
    static_assert(offsetof(DebugHeader, flags) == 0x04);
    inline constexpr byte DEBUG_FORMAT_VERSION = 1;

    struct DebugBlock
    {
        dword function;  // FUNCS index
        dword lines;     // <= instr_count
        dword locals;    // <= 65536
        dword reserved0; // 0
    };
    static_assert(sizeof(DebugBlock) == 0x10); // 16 B

    struct LineEntry
    {
        dword pc;     // increasing, first 0
        dword line;   // >= 1
        dword column; // 0 = unknown
    };
    static_assert(sizeof(LineEntry) == 0x0C); // 12 B

    struct LocalEntry
    {
        dword name;    // string const
        word slot;     // < regcnt
        byte flags;    // bit0 param
        byte reserved; // 0
        dword startPc; // begin
        dword endPc;   // end, <= instr_count
    };
    static_assert(sizeof(LocalEntry) == 0x10); // 16 B
    static_assert(offsetof(LocalEntry, slot) == 0x04);
    static_assert(offsetof(LocalEntry, startPc) == 0x08);
    static_assert(offsetof(LocalEntry, endPc) == 0x0C);
    // block = align8(16 + 12L + 16V)
    // image
    struct Constant
    {
        ConstKind kind = ConstKind::null;
        qword offset = 0;              // file offset
        std::span<const byte> payload; // bytes
    };

    struct DebugInfo
    {
        dword function = 0;           // FUNCS index
        std::vector<LineEntry> lines; // host order
        std::vector<LocalEntry> locals;
    };

    // views into buffer
    struct Image
    {
        Header header{};
        std::vector<SecBlock> sections;
        std::vector<MetaEntry> meta;
        std::span<const byte> moduleName; // key 1
        std::span<const byte> sourceFile; // key 2
        std::span<const byte> buildId;    // key 3
        std::span<const byte> comment;    // key 4
        std::vector<Constant> constants;
        std::span<const byte> code; // raw words
        dword codeCount = 0;        // N
        std::vector<Function> functions;
        std::vector<Global> globals;
        std::vector<Import> imports;
        DebugHeader debugHeader{};
        std::vector<DebugInfo> debug; // per function
    };
}

class Parse
{
private:
    std::vector<byte> buffer; // file bytes
    PARSE::Image imageData;   // views into buffer
    bool parsed = false;

private:
    void clearImage(void);

    int check(void);        // header, CRC, table
    int readSections(void); // segments
    int readMeta(void);
    int readConst(void);
    int readCode(void);
    int readFunc(void);
    int readGlobal(void);
    int readImport(void);
    int readDebug(void);
    int readCrossTable(void); // V76-79

    std::span<const byte> sectionBytes(dword id) const;
    bool isStringConstant(dword index) const;

public:
    Parse() = default;
    ~Parse() = default;
    Parse(const Parse &) = delete; // owns spans
    Parse &operator=(const Parse &) = delete;
    Parse(Parse &&) noexcept = default;
    Parse &operator=(Parse &&) noexcept = default;

    int load(const std::string &path);     // file
    int load(std::span<const byte> bytes); // copy
    int parse(void);                       // clear on failure

    bool isValid(void) const { return parsed; }
    const PARSE::Image &image(void) const { return imageData; }
    const PARSE::Header &header(void) const { return imageData.header; }

    // views into buffer; call index16 = FUNCS, hostcall index16 = IMPORTS
    std::span<const byte> text(void) const { return imageData.code; }                      // whole CODE
    std::span<const byte> text(dword functionIndex) const;                                 // body of function i
    std::span<const byte> entryText(void) const;                                           // entry body (tools)
    std::span<const PARSE::Function> functions(void) const { return imageData.functions; } // FUNCS table
    std::span<const PARSE::Import> imports(void) const { return imageData.imports; }       // IMPORTS table
    dword entryFunction(void) const { return imageData.header.entry; }                     // FUNCS index
    dword entryPc(void) const;                                                             // CODE instruction index
    bool hasDebug(void) const;

    static dword crc32c(std::span<const byte> data);
    static dword crc32c(std::span<const byte> data, qword zeroBegin, qword zeroEnd);
};