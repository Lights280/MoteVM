#include "dis/disassembler.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/text.hpp"
#include "common/trap.hpp"
#include "disasm.hpp"
#include "parse.hpp"
#include "verifier.hpp"

namespace
{
    dword at32(std::span<const byte> data, std::size_t offset)
    {
        return static_cast<dword>(data[offset]) | (static_cast<dword>(data[offset + 1]) << 8) |
               (static_cast<dword>(data[offset + 2]) << 16) |
               (static_cast<dword>(data[offset + 3]) << 24);
    }

    std::uint64_t at64(const std::span<const byte> payload)
    {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < payload.size() && i < 8; ++i)
            value |= static_cast<std::uint64_t>(payload[i]) << (i * 8);
        return value;
    }

    std::string toUpperAscii(std::string_view text)
    {
        std::string upper;
        upper.reserve(text.size());
        for (char character : text)
        {
            if (character >= 'a' && character <= 'z')
                character = static_cast<char>(character - 'a' + 'A');
            upper.push_back(character);
        }
        return upper;
    }

    std::string constantKey(const PARSE::Constant &constant)
    {
        std::string key(1, static_cast<char>(constant.kind));
        if (constant.kind == PARSE::ConstKind::string)
            key.append(reinterpret_cast<const char *>(constant.payload.data()),
                       constant.payload.size());
        else
            for (std::size_t i = 0; i < constant.payload.size() && i < 8; ++i)
                key.push_back(static_cast<char>(constant.payload[i]));
        return key;
    }

    std::string literalOf(const PARSE::Constant &constant)
    {
        switch (constant.kind)
        {
        case PARSE::ConstKind::null:
            return "null";
        case PARSE::ConstKind::boolean:
            return at64(constant.payload) != 0 ? "true" : "false";
        case PARSE::ConstKind::integer:
            return TOOL::formatInt64(static_cast<std::int64_t>(at64(constant.payload)));
        case PARSE::ConstKind::real:
            return TOOL::formatFloatBits(at64(constant.payload));
        case PARSE::ConstKind::string:
            return "\"" + TOOL::escapeBytes(constant.payload) + "\"";
        }
        return "null";
    }

    std::string nameRef(const PARSE::Image &image,
                        const std::unordered_map<std::string, dword> &first, dword index,
                        const char *anonymous)
    {
        if (index == NO_INDEX)
            return anonymous;
        if (index < image.constants.size() &&
            image.constants[index].kind == PARSE::ConstKind::string)
        {
            const std::span<const byte> payload = image.constants[index].payload;
            const std::string content(reinterpret_cast<const char *>(payload.data()),
                                      payload.size());
            const auto found = first.find(constantKey(image.constants[index]));
            if (TOOL::isIdentifier(content) && found != first.end() && found->second == index)
                return content;
        }
        return "#" + std::to_string(index);
    }

    bool hasMeta(const PARSE::Image &image, word key)
    {
        for (const PARSE::MetaEntry &entry : image.meta)
            if (entry.key == key)
                return true;
        return false;
    }

    std::string registerName(byte value)
    {
        return "r" + std::to_string(static_cast<unsigned>(value));
    }
}

namespace
{
    std::string labelName(dword pc)
    {
        char buffer[16] = {};
        std::snprintf(buffer, sizeof(buffer), "L%04X", pc);
        return buffer;
    }

    std::string functionRef(const PARSE::Image &image,
                            const std::unordered_map<std::string, dword> &first, dword index)
    {
        if (index >= image.functions.size())
            return "#" + std::to_string(index);
        const PARSE::Function &function = image.functions[index];
        if (function.name != NO_INDEX && function.name < image.constants.size() &&
            image.constants[function.name].kind == PARSE::ConstKind::string)
        {
            const std::span<const byte> payload = image.constants[function.name].payload;
            const std::string content(reinterpret_cast<const char *>(payload.data()),
                                      payload.size());
            const auto found = first.find(constantKey(image.constants[function.name]));
            if (TOOL::isIdentifier(content) && found != first.end() &&
                found->second == function.name)
                return content;
        }
        return "#" + std::to_string(index);
    }

    std::string importRef(const PARSE::Image &image,
                          const std::unordered_map<std::string, dword> &first, dword index)
    {
        if (index >= image.imports.size())
            return "#" + std::to_string(index);
        const std::string fallback = "#" + std::to_string(index);
        return nameRef(image, first, image.imports[index].name, fallback.c_str());
    }

    std::string instructionText(const PARSE::Image &image, const PARSE::Function &function,
                                dword pc, const std::unordered_map<std::string, dword> &first)
    {
        const dword encoded = at32(image.code, static_cast<std::size_t>(function.entry + pc) * 4);
        const byte opcode = static_cast<byte>(encoded & ISA::OPCODE_MASK);
        const PARSE::OpInfo *info = PARSE::opcodeInfo(opcode);
        const char *mnemonic = DISASM::mnemonic(opcode);
        if (info == nullptr || mnemonic == nullptr)
            return ".word " + TOOL::hexFixed(encoded, 8);

        const byte a = static_cast<byte>((encoded >> 8) & 0xFFu);
        const byte b = static_cast<byte>((encoded >> 16) & 0xFFu);
        const byte c = static_cast<byte>((encoded >> 24) & 0xFFu);
        std::string line = toUpperAscii(mnemonic);

        switch (info->format)
        {
        case PARSE::InstrFormat::op:
            break;
        case PARSE::InstrFormat::r:
            line += " " + registerName(a);
            break;
        case PARSE::InstrFormat::rr:
            line += " " + registerName(a) + ", " + registerName(b);
            break;
        case PARSE::InstrFormat::rr8:
            line += " " + registerName(a) + ", " + registerName(b) + ", +" +
                    std::to_string(static_cast<unsigned>(c));
            break;
        case PARSE::InstrFormat::rrr:
            line += " " + registerName(a) + ", " + registerName(b) + ", " + registerName(c);
            break;
        case PARSE::InstrFormat::ri16:
            if (opcode == ISA::jz || opcode == ISA::jnz)
            {
                const word immediate = static_cast<word>((static_cast<word>(c) << 8) | b);
                const std::int32_t offset = DISASM::signExtend16(immediate);
                const dword target = static_cast<dword>(
                    static_cast<std::int64_t>(pc) + 1 + offset / ISA::INSTRUCTION_SIZE);
                line += " " + registerName(a) + ", " + labelName(target);
            }
            else
            {
                const word immediate = static_cast<word>((static_cast<word>(c) << 8) | b);
                line += " " + registerName(a) + ", " + TOOL::hexFixed(immediate, 4);
            }
            break;
        case PARSE::InstrFormat::i24:
            if (opcode == ISA::jmp)
            {
                const std::int32_t offset = DISASM::signExtend24(encoded >> 8);
                const dword target = static_cast<dword>(
                    static_cast<std::int64_t>(pc) + 1 + offset / ISA::INSTRUCTION_SIZE);
                line += " " + labelName(target);
            }
            else if (opcode == ISA::trap)
            {
                const dword code = encoded >> 8;
                const char *name = TOOL::trapName(code);
                line += " ";
                line += name != nullptr ? name : std::to_string(code);
            }
            else
            {
                return ".word " + TOOL::hexFixed(encoded, 8);
            }
            break;
        case PARSE::InstrFormat::call16:
        {
            const dword index = (encoded >> 16) & 0xFFFFu;
            line += " " + registerName(a) + ", ";
            line += opcode == ISA::call ? functionRef(image, first, index)
                                        : importRef(image, first, index);
            break;
        }
        }
        return line;
    }
}


DISASM::TextResult DISASM::renderCanonical(std::span<const byte> module, bool includeDebug,
                                             bool annotate)
{
    TextResult result;
    Parse parser;
    int code = parser.load(module);
    if (code == PARSE::OK)
        code = parser.parse();
    if (code != PARSE::OK)
    {
        result.status = TOOL::INPUT;
        result.error = PARSE::errorText(code);
        return result;
    }

    const PARSE::Image &image = parser.image();
    std::unordered_map<std::string, dword> first;
    for (std::size_t i = 0; i < image.constants.size(); ++i)
        first.emplace(constantKey(image.constants[i]), static_cast<dword>(i));

    bool nameFallback = false;
    const auto checkName = [&image, &first, &nameFallback](dword index) {
        if (index == NO_INDEX || index >= image.constants.size() ||
            image.constants[index].kind != PARSE::ConstKind::string)
            return;
        const std::span<const byte> payload = image.constants[index].payload;
        const std::string content(reinterpret_cast<const char *>(payload.data()), payload.size());
        const auto found = first.find(constantKey(image.constants[index]));
        if (!TOOL::isIdentifier(content) || found == first.end() || found->second != index)
            nameFallback = true;
    };
    for (const PARSE::Import &import : image.imports)
        checkName(import.name);
    for (const PARSE::Global &global : image.globals)
        checkName(global.name);
    for (const PARSE::Function &function : image.functions)
        checkName(function.name);
    for (const PARSE::DebugInfo &debug : image.debug)
        for (const PARSE::LocalEntry &local : debug.locals)
            checkName(local.name);

    result.text = "; canonical text generated by motedis 1.0; format MOTEBC 1.0\n";
    if (hasMeta(image, PARSE::META_MODULE_NAME))
        result.text += ".module \"" + TOOL::escapeBytes(image.moduleName) + "\"\n";

    std::vector<const PARSE::MetaEntry *> metas;
    for (const PARSE::MetaEntry &entry : image.meta)
        if (entry.key != PARSE::META_MODULE_NAME)
            metas.push_back(&entry);
    std::sort(metas.begin(), metas.end(),
              [](const PARSE::MetaEntry *left, const PARSE::MetaEntry *right) {
                  return left->key < right->key;
              });
    for (const PARSE::MetaEntry *entry : metas)
    {
        result.text += ".meta " + std::to_string(static_cast<unsigned>(entry->key)) + ", \"" +
                       TOOL::escapeBytes(entry->raw) + "\"\n";
    }

    for (const PARSE::Constant &constant : image.constants)
        result.text += ".const " + literalOf(constant) + "\n";

    for (std::size_t i = 0; i < image.imports.size(); ++i)
    {
        const PARSE::Import &import = image.imports[i];
        const std::string fallback = "#" + std::to_string(i);
        result.text += ".import " + nameRef(image, first, import.name, fallback.c_str()) +
                       " " + std::to_string(static_cast<unsigned>(import.arity)) + "\n";
    }

    for (const PARSE::Global &global : image.globals)
    {
        result.text += ".global " + nameRef(image, first, global.name, "anonymous");
        if ((global.flags & PARSE::GLOBAL_FLAG_CONST) != 0)
            result.text += " const";
        if (global.init != NO_INDEX)
        {
            const auto found = global.init < image.constants.size()
                                   ? first.find(constantKey(image.constants[global.init]))
                                   : first.end();
            if (found != first.end() && found->second == global.init)
                result.text += " " + literalOf(image.constants[global.init]);
            else
                result.text += " #" + std::to_string(global.init);
        }
        result.text += "\n";
    }

    result.text += ".entry " + functionRef(image, first, image.header.entry) + "\n";


    for (std::size_t fi = 0; fi < image.functions.size(); ++fi)
    {
        const PARSE::Function &function = image.functions[fi];
        result.text += ".func " + nameRef(image, first, function.name, "anonymous") + " " +
                       std::to_string(static_cast<unsigned>(function.arity)) + " " +
                       std::to_string(static_cast<unsigned>(function.regcnt));
        if ((function.flags & PARSE::FUNC_FLAG_EXPORTED) != 0)
            result.text += " flags=0x01";
        result.text += "\n";

        const bool hasDebugInfo = includeDebug && fi < image.debug.size();
        if (hasDebugInfo)
        {
            std::vector<PARSE::LocalEntry> locals = image.debug[fi].locals;
            std::sort(locals.begin(), locals.end(),
                      [](const PARSE::LocalEntry &left, const PARSE::LocalEntry &right) {
                          if (left.startPc != right.startPc)
                              return left.startPc < right.startPc;
                          return left.slot < right.slot;
                      });
            for (const PARSE::LocalEntry &local : locals)
            {
                const std::string fallback = "#" + std::to_string(local.name);
                result.text += ".local " + nameRef(image, first, local.name, fallback.c_str()) +
                               ", " + registerName(static_cast<byte>(local.slot));
                if ((local.flags & PARSE::LOCAL_FLAG_PARAMETER) != 0)
                    result.text += ", param";
                result.text += ", start=" + std::to_string(local.startPc) +
                               ", end=" + std::to_string(local.endPc) + "\n";
            }
        }

        std::vector<bool> targets(function.instr, false);
        if (function.instr != 0)
            targets[0] = true;
        for (dword pc = 0; pc < function.instr; ++pc)
        {
            const dword encoded = at32(image.code,
                                       static_cast<std::size_t>(function.entry + pc) * 4);
            const byte opcode = static_cast<byte>(encoded & ISA::OPCODE_MASK);
            if (opcode == ISA::jmp)
            {
                const std::int32_t offset = DISASM::signExtend24(encoded >> 8);
                const std::int64_t target =
                    static_cast<std::int64_t>(pc) + 1 + offset / ISA::INSTRUCTION_SIZE;
                if (target >= 0 && target < static_cast<std::int64_t>(function.instr))
                    targets[static_cast<std::size_t>(target)] = true;
            }
            else if (opcode == ISA::jz || opcode == ISA::jnz)
            {
                const word immediate =
                    static_cast<word>((encoded >> 16) & 0xFFFFu);
                const std::int32_t offset = DISASM::signExtend16(immediate);
                const std::int64_t target =
                    static_cast<std::int64_t>(pc) + 1 + offset / ISA::INSTRUCTION_SIZE;
                if (target >= 0 && target < static_cast<std::int64_t>(function.instr))
                    targets[static_cast<std::size_t>(target)] = true;
            }
        }

        std::unordered_map<dword, const PARSE::LineEntry *> lineMap;
        if (hasDebugInfo)
            for (const PARSE::LineEntry &line : image.debug[fi].lines)
                lineMap.emplace(line.pc, &line);

        for (dword pc = 0; pc < function.instr; ++pc)
        {
            if (targets[pc])
                result.text += labelName(pc) + ":\n";
            if (hasDebugInfo)
            {
                const auto found = lineMap.find(pc);
                if (found != lineMap.end())
                {
                    const PARSE::LineEntry &line = *found->second;
                    result.text += ".loc ";
                    if (hasMeta(image, PARSE::META_SOURCE_FILE))
                        result.text += "\"" + TOOL::escapeBytes(image.sourceFile) + "\", ";
                    result.text += std::to_string(line.line);
                    if (line.column != 0)
                        result.text += ", " + std::to_string(line.column);
                    result.text += "\n";
                }
            }
            result.text += "    " + instructionText(image, function, pc, first);
            if (annotate)
                result.text += " ; pc=" +
                               TOOL::hexFixed(static_cast<std::uint64_t>(function.entry + pc) * 4,
                                              8);
            result.text += "\n";
        }
        result.text += ".end\n";
    }

    dword expected = 0;
    bool canonical = true;
    for (const PARSE::Function &function : image.functions)
    {
        if (function.entry != expected)
            canonical = false;
        expected += function.instr;
    }
    const dword tail = image.codeCount >= expected ? image.codeCount - expected : 0;
    if (tail != (expected & 1u))
        canonical = false;
    if (!canonical)
        result.warnings.push_back(
            "non-canonical function layout: FUNCS order differs from CODE order; "
            "round-trip byte equality is not guaranteed");
    if (nameFallback)
        result.warnings.push_back(
            "W004: some names use #n fallbacks (non-identifier or duplicate string constants)");

    result.status = TOOL::OK;
    return result;
}

