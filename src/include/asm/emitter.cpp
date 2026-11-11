#include "asm/emitter.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/text.hpp"
#include "parse.hpp"

namespace
{
    using Bytes = std::vector<byte>;

    void put8(Bytes &out, unsigned value)
    {
        out.push_back(static_cast<byte>(value & 0xFFu));
    }

    void put16(Bytes &out, unsigned value)
    {
        put8(out, value & 0xFFFFu);
        put8(out, (value >> 8) & 0xFFFFu);
    }

    void put32(Bytes &out, std::uint32_t value)
    {
        put16(out, value & 0xFFFFu);
        put16(out, (value >> 16) & 0xFFFFu);
    }

    void put64(Bytes &out, std::uint64_t value)
    {
        put32(out, static_cast<std::uint32_t>(value));
        put32(out, static_cast<std::uint32_t>(value >> 32));
    }

    void putBytes(Bytes &out, std::span<const byte> data)
    {
        out.insert(out.end(), data.begin(), data.end());
    }

    void pad8(Bytes &out)
    {
        while ((out.size() & 7u) != 0)
            out.push_back(0);
    }

    struct Section
    {
        dword id;
        Bytes data;
    };

    Bytes metaSection(const ASM::Module &module)
    {
        struct Record
        {
            word key;
            const std::string *value;
        };
        std::vector<Record> records;
        if (module.hasModule)
            records.push_back({PARSE::META_MODULE_NAME, &module.moduleName});
        for (const ASM::MetaEntry &entry : module.meta)
        {
            if (entry.key == PARSE::META_MODULE_NAME)
                continue;
            records.push_back({entry.key, &entry.value});
        }
        std::sort(records.begin(), records.end(),
                  [](const Record &left, const Record &right) { return left.key < right.key; });

        Bytes out;
        for (const Record &record : records)
        {
            put16(out, record.key);
            put16(out, static_cast<unsigned>(record.value->size()));
            putBytes(out, std::span<const byte>(
                              reinterpret_cast<const byte *>(record.value->data()),
                              record.value->size()));
            pad8(out);
        }
        return out;
    }

    Bytes constSection(const ASM::Module &module)
    {
        Bytes out;
        put32(out, static_cast<std::uint32_t>(module.constants.size()));
        put32(out, 0);
        for (const ASM::Constant &constant : module.constants)
        {
            put8(out, static_cast<unsigned>(constant.value.kind));
            put8(out, 0);
            put16(out, 0);
            if (constant.value.kind == PARSE::ConstKind::string)
            {
                put32(out, static_cast<std::uint32_t>(constant.value.bytes.size()));
                putBytes(out, std::span<const byte>(
                                  reinterpret_cast<const byte *>(constant.value.bytes.data()),
                                  constant.value.bytes.size()));
            }
            else
            {
                put32(out, 8);
                put64(out, constant.value.bits);
            }
            pad8(out);
        }
        return out;
    }
}

namespace
{
    Bytes debugSection(ASM::Module &module, bool enabled)
    {
        if (!enabled)
        {
            for (ASM::Function &function : module.functions)
                function.dbgOffset = NO_INDEX;
            return Bytes{};
        }

        Bytes out;
        put8(out, PARSE::DEBUG_FORMAT_VERSION);
        put8(out, 0);
        put16(out, 0);
        put32(out, 0);
        for (std::size_t i = 0; i < module.functions.size(); ++i)
        {
            ASM::Function &function = module.functions[i];
            if (function.lines.empty() && function.locals.empty())
            {
                function.dbgOffset = NO_INDEX;
                continue;
            }
            function.dbgOffset = static_cast<dword>(out.size());
            put32(out, static_cast<std::uint32_t>(i));
            put32(out, static_cast<std::uint32_t>(function.lines.size()));
            put32(out, static_cast<std::uint32_t>(function.locals.size()));
            put32(out, 0);
            for (const ASM::LineRecord &line : function.lines)
            {
                put32(out, line.pc);
                put32(out, line.line);
                put32(out, line.column);
            }
            for (const ASM::Local &local : function.locals)
            {
                put32(out, local.name.index);
                put16(out, local.slot);
                put8(out, local.param ? 1u : 0u);
                put8(out, 0);
                put32(out, local.startPc);
                put32(out, local.endPc);
            }
            pad8(out);
        }
        return out;
    }

    Bytes funcsSection(const ASM::Module &module)
    {
        Bytes out;
        put32(out, static_cast<std::uint32_t>(module.functions.size()));
        for (const ASM::Function &function : module.functions)
        {
            put32(out, function.name.index);
            put32(out, function.entryPc);
            put32(out, static_cast<std::uint32_t>(function.words.size()));
            put16(out, static_cast<unsigned>(function.regs));
            put8(out, static_cast<unsigned>(function.arity));
            put8(out, static_cast<unsigned>(function.flags));
            put16(out, function.localCount);
            put16(out, 0);
            put32(out, function.dbgOffset);
            put32(out, 0);
            put32(out, 0);
        }
        pad8(out);
        return out;
    }

    Bytes codeSection(const ASM::Module &module)
    {
        Bytes out;
        std::uint32_t count = 0;
        for (const ASM::Function &function : module.functions)
        {
            for (dword word : function.words)
            {
                put32(out, word);
                ++count;
            }
        }
        if ((count & 1u) != 0)
            put32(out, PARSE::CODE_NOP);
        return out;
    }

    Bytes globalsSection(const ASM::Module &module)
    {
        Bytes out;
        put32(out, static_cast<std::uint32_t>(module.globals.size()));
        for (const ASM::Global &global : module.globals)
        {
            put32(out, global.name.index);
            put32(out, global.hasInit ? global.initIndex : NO_INDEX);
            put32(out, global.isConst ? 1u : 0u);
        }
        pad8(out);
        return out;
    }

    Bytes importsSection(const ASM::Module &module)
    {
        Bytes out;
        put32(out, static_cast<std::uint32_t>(module.imports.size()));
        for (const ASM::Import &import : module.imports)
        {
            put32(out, import.name.index);
            put8(out, static_cast<unsigned>(import.arity));
            put8(out, static_cast<unsigned>(import.flags));
            put16(out, 0);
        }
        pad8(out);
        return out;
    }
}


bool ASM::emitImage(Module &module, const Options &options, std::vector<byte> &image,
                    std::vector<ListingEntry> &listing, TOOL::Diagnostics &diagnostics)
{
    (void)listing;
    bool hasDebug = false;
    if (!options.noDebug)
    {
        for (const Function &function : module.functions)
        {
            if (!function.lines.empty() || !function.locals.empty())
            {
                hasDebug = true;
                break;
            }
        }
        if (options.forceDebug)
            hasDebug = true;
    }

    Bytes debug = debugSection(module, hasDebug);
    std::vector<Section> sections;
    sections.push_back({PARSE::SECTION_META, metaSection(module)});
    sections.push_back({PARSE::SECTION_CONST, constSection(module)});
    sections.push_back({PARSE::SECTION_FUNCS, funcsSection(module)});
    sections.push_back({PARSE::SECTION_CODE, codeSection(module)});
    sections.push_back({PARSE::SECTION_GLOBALS, globalsSection(module)});
    sections.push_back({PARSE::SECTION_IMPORTS, importsSection(module)});
    if (hasDebug)
        sections.push_back({PARSE::SECTION_DEBUG, std::move(debug)});

    for (const Section &section : sections)
    {
        if ((section.data.size() & 7u) != 0)
        {
            diagnostics.error("E031", TOOL::SourcePosition{}, "section size is not 8-byte aligned");
            return false;
        }
    }

    const word sectionCount = static_cast<word>(sections.size());
    const qword tableEnd = PARSE::HEADER_SIZE + PARSE::SECBLOCK_SIZE * sectionCount;
    qword offset = tableEnd;
    for (const Section &section : sections)
    {
        if ((offset & 7ull) != 0)
        {
            diagnostics.error("E031", TOOL::SourcePosition{}, "section offset is not 8-byte aligned");
            return false;
        }
        offset += section.data.size();
    }
    if (offset > 0xFFFFFFFFull)
    {
        diagnostics.error("E031", TOOL::SourcePosition{}, "module exceeds 4 GiB");
        return false;
    }

    image.clear();
    image.reserve(static_cast<std::size_t>(offset));
    image.push_back('M');
    image.push_back('O');
    image.push_back('T');
    image.push_back('E');
    put16(image, PARSE::SUPPORTED_MAJOR);
    put16(image, PARSE::SUPPORTED_MINOR);
    put32(image, hasDebug ? PARSE::FLAG_DEBUG_PRESENT : 0);
    put32(image, module.entryFunction);
    put16(image, sectionCount);
    put16(image, 0);
    put32(image, 0); // file size, patched below
    put32(image, 0); // checksum, patched below
    put32(image, 0);

    qword cursor = tableEnd;
    for (const Section &section : sections)
    {
        put32(image, section.id);
        put32(image, 0);
        put64(image, cursor);
        put64(image, static_cast<std::uint64_t>(section.data.size()));
        cursor += section.data.size();
    }
    for (const Section &section : sections)
        putBytes(image, section.data);

    const auto patch32 = [&image](std::size_t at, std::uint32_t value) {
        image[at] = static_cast<byte>(value & 0xFFu);
        image[at + 1] = static_cast<byte>((value >> 8) & 0xFFu);
        image[at + 2] = static_cast<byte>((value >> 16) & 0xFFu);
        image[at + 3] = static_cast<byte>((value >> 24) & 0xFFu);
    };

    patch32(20, static_cast<std::uint32_t>(image.size()));
    patch32(24, 0);
    patch32(24, Parse::crc32c(std::span<const byte>(image.data(), image.size()), 24, 28));
    return true;
}

