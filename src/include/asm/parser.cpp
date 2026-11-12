#include "asm/parser.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <unordered_map>

#include "asm/lexer.hpp"
#include "common/text.hpp"
#include "common/trap.hpp"
#include "disasm.hpp"
#include "parse.hpp"
#include "verifier.hpp"

using namespace ASM;

namespace
{
    bool isReserved(std::string_view lower)
    {
        return lower == "auto" || lower == "const" || lower == "true" || lower == "false" ||
               lower == "null" || lower == "nan" || lower == "inf";
    }

    bool isLocalLabelName(std::string_view text)
    {
        if (text.size() < 3 || text[0] != '.' || text[1] != 'L')
            return false;
        for (std::size_t i = 2; i < text.size(); ++i)
        {
            const char character = text[i];
            if (!((character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '_'))
                return false;
        }
        return true;
    }

    bool isName(std::string_view text)
    {
        return TOOL::isIdentifier(text) && !isReserved(TOOL::toLowerAscii(text));
    }

    // Import names are identifiers or dotted identifiers ("file.write").
    // A single reserved word is still rejected, matching ordinary names.
    bool isImportName(std::string_view text)
    {
        return !isReserved(TOOL::toLowerAscii(text)) && TOOL::isQualifiedIdentifier(text);
    }

    std::string trim(std::string_view text)
    {
        std::size_t begin = 0;
        while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
            ++begin;
        std::size_t end = text.size();
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                               text[end - 1] == '\r'))
            --end;
        return std::string(text.substr(begin, end - begin));
    }

    const std::unordered_map<std::string, byte> &mnemonics(void)
    {
        static const std::unordered_map<std::string, byte> table = [] {
            std::unordered_map<std::string, byte> map;
            for (int opcode = 0; opcode <= 0x60; ++opcode)
            {
                const char *name = DISASM::mnemonic(static_cast<byte>(opcode));
                if (name != nullptr)
                    map.emplace(TOOL::toLowerAscii(name), static_cast<byte>(opcode));
            }
            return map;
        }();
        return table;
    }

    bool hasNumericShape(std::string_view text)
    {
        if (text.empty())
            return false;
        for (char character : text)
        {
            const bool allowed = (character >= '0' && character <= '9') ||
                                 (character >= 'a' && character <= 'f') ||
                                 (character >= 'A' && character <= 'F') || character == 'x' ||
                                 character == 'X' || character == 'b' || character == 'B' ||
                                 character == '_' || character == '+' || character == '-';
            if (!allowed)
                return false;
        }
        return true;
    }

    TOOL::SourcePosition positionOfOffset(const std::vector<byte> &source, std::size_t offset)
    {
        int line = 1;
        std::size_t lineStart = 0;
        for (std::size_t i = 0; i < offset && i < source.size(); ++i)
        {
            if (source[i] == '\n')
            {
                ++line;
                lineStart = i + 1;
            }
        }
        const std::size_t length = (offset < source.size() ? offset : source.size()) - lineStart;
        const std::string_view text(reinterpret_cast<const char *>(source.data()) + lineStart,
                                    length);
        return TOOL::SourcePosition{line, static_cast<int>(TOOL::codePointColumn(text, length))};
    }
}

namespace
{
    class Parser
    {
    public:
        Parser(const std::vector<byte> &sourceBytes, Module &moduleTarget,
               TOOL::Diagnostics &diagnosticsSink)
            : source(sourceBytes), module(moduleTarget), diagnostics(diagnosticsSink)
        {
        }

        bool run(void);

    private:
        const std::vector<byte> &source;
        Module &module;
        TOOL::Diagnostics &diagnostics;
        int phase = 0; // 0 module/meta, 1 constants, 2 imports/globals, 3 entry, 4 functions
        Function *current = nullptr;
        dword totalInstructions = 0;
        bool sourceFileSet = false;
        std::string sourceFile;
        bool sawStatement = false;
        bool locationSet = false;
        LineRecord location{};
        TOOL::SourcePosition locationPos{};
        bool missingFirstLocation = false;
        std::string currentSource;

        bool handleLine(std::string_view line, int lineNumber);
        bool handleDirective(const std::string &name, const std::vector<Token> &tokens,
                             std::size_t index, TOOL::SourcePosition position);
        bool handleInstruction(const std::string &name, const std::vector<Token> &tokens,
                               std::size_t index, TOOL::SourcePosition position);
        bool handleFunc(const std::vector<Token> &tokens, std::size_t index, TOOL::SourcePosition position);
        bool handleEnd(const std::vector<Token> &tokens, std::size_t index, TOOL::SourcePosition position);
        bool handleLoc(const std::vector<Token> &tokens, std::size_t index);
        bool handleLocal(const std::vector<Token> &tokens, std::size_t index);
        bool handleEntry(const std::vector<Token> &tokens, std::size_t index);
        bool handleMeta(const std::vector<Token> &tokens, std::size_t index, bool sourceAlias);
        bool handleConstant(const std::string &directive, const std::vector<Token> &tokens,
                            std::size_t index);
        bool handleImport(const std::vector<Token> &tokens, std::size_t index);
        bool handleGlobal(const std::vector<Token> &tokens, std::size_t index);

        bool addLabel(const Token &token, TOOL::SourcePosition position);
        bool addInstruction(byte opcode, const std::vector<Token> &tokens, std::size_t index,
                            TOOL::SourcePosition position, std::string sourceText);

        bool parseRegister(const Token &token, Operand &operand);
        bool parseImmediate(const Token &token, std::int64_t minimum, std::int64_t maximum,
                            Operand &operand, const char *code);
        bool parseIndex(const Token &token, Operand &operand);
        bool parseNameRef(const Token &token, bool allowAnonymous, NameRef &ref);
        bool parseImportNameRef(const Token &token, NameRef &ref);
        bool parseOperandName(const Token &token, Operand &operand);
        bool parseImportOperand(const Token &token, Operand &operand);
        bool parseTarget(const Token &token, Operand &operand);
        bool parseTrap(const Token &token, Operand &operand);
        bool parseConstantLiteral(const Token &token, ConstantValue &value);
        bool parseInitValue(const Token &token, Global &global);

        void recordLocation(dword pc);
        void setSourceFile(const std::vector<byte> &bytes, TOOL::SourcePosition position);
        bool hasMetaKey(word key) const;
        Function *findFunction(std::string_view name);
    };
}


namespace
{
    bool Parser::run(void)
    {
        if (source.size() > PARSE::MAX_MODULE_BYTES)
        {
            diagnostics.error("E002", TOOL::SourcePosition{1, 1}, "source file is larger than 64 MiB");
            return false;
        }
        std::size_t badOffset = 0;
        if (!TOOL::validUtf8(source, badOffset))
        {
            diagnostics.error("E001", positionOfOffset(source, badOffset),
                               "source file is not valid UTF-8");
            return false;
        }

        std::size_t offset = 0;
        if (source.size() >= 3 && source[0] == 0xEF && source[1] == 0xBB && source[2] == 0xBF)
            offset = 3;
        int lineNumber = 1;
        while (true)
        {
            std::size_t end = offset;
            while (end < source.size() && source[end] != '\n')
                ++end;
            const std::string_view line(reinterpret_cast<const char *>(source.data()) + offset,
                                        end - offset);
            handleLine(line, lineNumber);
            if (end >= source.size())
                break;
            offset = end + 1;
            ++lineNumber;
        }

        if (current != nullptr)
        {
            diagnostics.error("E027", current->position, "missing .end for function");
            current = nullptr;
        }
        if (module.functions.empty())
            diagnostics.error("E020", TOOL::SourcePosition{1, 1}, "module must contain at least one .func");
        return diagnostics.errorCount() == 0;
    }

    bool Parser::handleLine(std::string_view line, int lineNumber)
    {
        currentSource = trim(line);
        std::vector<Token> tokens;
        if (!ASM::tokenizeLine(line, lineNumber, tokens, diagnostics))
            return false;
        if (tokens.empty())
            return true;

        std::size_t index = 0;
        if (!tokens[index].isString && !tokens[index].text.empty() &&
            tokens[index].text.back() == ':')
        {
            if (!addLabel(tokens[index], tokens[index].position))
                return false;
            ++index;
            if (index >= tokens.size())
                return true;
            if (!tokens[index].isString && !tokens[index].text.empty() &&
                tokens[index].text.back() == ':')
            {
                diagnostics.error("E001", tokens[index].position,
                                   "only one label is allowed per line");
                return false;
            }
        }

        const Token &head = tokens[index];
        if (head.isString)
        {
            diagnostics.error("E001", head.position, "unexpected string literal");
            return false;
        }
        if (head.text.empty())
            return true;
        if (head.text[0] == '.')
            return handleDirective(TOOL::toLowerAscii(head.text), tokens, index + 1, head.position);
        return handleInstruction(head.text, tokens, index + 1, head.position);
    }


    bool Parser::handleDirective(const std::string &name, const std::vector<Token> &tokens,
                                 std::size_t index, TOOL::SourcePosition position)
    {
        if (name != ".module")
            sawStatement = true;
        if (name == ".module")
        {
            if (phase != 0 || sawStatement)
            {
                diagnostics.error("E018", position, ".module must be the first statement");
                return false;
            }
            if (module.hasModule)
            {
                diagnostics.error("E030", position, "duplicate .module");
                return false;
            }
            if (index + 1 != tokens.size())
            {
                diagnostics.error("E013", position, ".module expects exactly one value");
                return false;
            }
            const Token &value = tokens[index];
            module.hasModule = true;
            module.modulePos = position;
            if (value.isString)
                module.moduleName.assign(reinterpret_cast<const char *>(value.bytes.data()),
                                          value.bytes.size());
            else
                module.moduleName = value.text;
            if (module.moduleName.size() > 0xFFFFu)
                diagnostics.error("E023", position, "module name exceeds 65535 bytes");
            return true;
        }
        if (name == ".source")
            return handleMeta(tokens, index, true);
        if (name == ".meta")
            return handleMeta(tokens, index, false);
        if (name == ".const" || name == ".string" || name == ".int" || name == ".float" ||
            name == ".bool" || name == ".null")
            return handleConstant(name, tokens, index);
        if (name == ".import")
            return handleImport(tokens, index);
        if (name == ".global")
            return handleGlobal(tokens, index);
        if (name == ".entry")
            return handleEntry(tokens, index);
        if (name == ".func")
            return handleFunc(tokens, index, position);
        if (name == ".end")
            return handleEnd(tokens, index, position);
        if (name == ".loc")
            return handleLoc(tokens, index);
        if (name == ".local")
            return handleLocal(tokens, index);
        diagnostics.error("E004", position, "unknown directive '" + name + "'");
        return false;
    }

    bool Parser::addLabel(const Token &token, TOOL::SourcePosition position)
    {
        sawStatement = true;
        if (current == nullptr)
        {
            diagnostics.error("E018", position, "labels are only allowed inside .func");
            return false;
        }
        const std::string name = token.text.substr(0, token.text.size() - 1);
        const bool local = isLocalLabelName(name);
        if (!local && !isName(name))
        {
            diagnostics.error("E001", position, "invalid label name '" + name + "'");
            return false;
        }
        for (const Label &label : current->labels)
        {
            if (label.local == local && label.name == name)
            {
                diagnostics.error("E030", position, "duplicate label '" + name + "'");
                return false;
            }
        }
        current->labels.push_back({name, local, static_cast<dword>(current->code.size()), position});
        return true;
    }


    bool Parser::hasMetaKey(word key) const
    {
        for (const MetaEntry &entry : module.meta)
            if (entry.key == key)
                return true;
        return false;
    }

    void Parser::setSourceFile(const std::vector<byte> &bytes, TOOL::SourcePosition position)
    {
        const std::string value(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (sourceFileSet)
        {
            if (sourceFile != value)
                diagnostics.error("E033", position, "multiple source file names are not supported");
            return;
        }
        sourceFileSet = true;
        sourceFile = value;
        if (std::filesystem::path(value).is_absolute())
            diagnostics.warning("W006", position,
                                 "absolute source path reduces build reproducibility");
        if (!hasMetaKey(PARSE::META_SOURCE_FILE))
            module.meta.push_back({PARSE::META_SOURCE_FILE, value, position});
    }

    void Parser::recordLocation(dword pc)
    {
        if (!locationSet || current == nullptr)
            return;
        if (current->lines.empty())
        {
            if (pc != 0)
            {
                if (!missingFirstLocation)
                {
                    diagnostics.error("E034", locationPos,
                                       "the first .loc must precede the first instruction");
                    missingFirstLocation = true;
                }
                return;
            }
            current->lines.push_back({0, location.line, location.column});
            return;
        }
        const LineRecord &last = current->lines.back();
        if (pc <= last.pc)
            return;
        if (last.line == location.line && last.column == location.column)
            return;
        current->lines.push_back({pc, location.line, location.column});
    }

    Function *Parser::findFunction(std::string_view name)
    {
        for (Function &function : module.functions)
        {
            if (function.name.kind == NameRef::Kind::Name && function.name.name == name)
                return &function;
        }
        return nullptr;
    }
}


namespace
{
    bool Parser::handleFunc(const std::vector<Token> &tokens, std::size_t index, TOOL::SourcePosition position)
    {
        if (current != nullptr)
        {
            diagnostics.error("E027", position, "nested .func is not allowed");
            return false;
        }
        if (index >= tokens.size())
        {
            diagnostics.error("E013", position, ".func requires a name");
            return false;
        }

        NameRef name;
        if (!parseNameRef(tokens[index], true, name))
            return false;
        ++index;
        if (name.kind == NameRef::Kind::Name && findFunction(name.name) != nullptr)
            diagnostics.error("E030", name.position, "duplicate function name '" + name.name + "'");

        dword arity = 0;
        dword regs = 0;
        bool autoRegs = false;
        dword flags = 0;
        bool flagsSeen = false;
        std::vector<std::uint64_t> numbers;

        for (; index < tokens.size(); ++index)
        {
            const Token &token = tokens[index];
            const std::string lower = TOOL::toLowerAscii(token.text);
            if (lower.rfind("flags=", 0) == 0)
            {
                if (flagsSeen)
                {
                    diagnostics.error("E013", token.position, "duplicate flags=");
                    return false;
                }
                flagsSeen = true;
                std::uint64_t value = 0;
                if (!TOOL::parseUInt64(std::string_view(token.text).substr(6), value) ||
                    value > 0xFFu)
                {
                    diagnostics.error("E033", token.position, "invalid flags value");
                    return false;
                }
                flags = static_cast<dword>(value);
                continue;
            }
            if (lower == "auto")
            {
                autoRegs = true;
                continue;
            }
            std::uint64_t number = 0;
            if (!TOOL::parseUInt64(token.text, number))
            {
                diagnostics.error("E013", token.position, "unexpected .func operand '" + token.text +
                                                       "'");
                return false;
            }
            numbers.push_back(number);
        }

        if (numbers.size() == 2 && !autoRegs)
        {
            arity = static_cast<dword>(numbers[0]);
            regs = static_cast<dword>(numbers[1]);
        }
        else if (numbers.size() == 1)
        {
            if (autoRegs)
                arity = static_cast<dword>(numbers[0]);
            else
                regs = static_cast<dword>(numbers[0]); // .func name regs: arity defaults to 0
        }
        else if (numbers.empty() && autoRegs)
        {
            // .func name auto: arity defaults to 0
        }
        else
        {
            diagnostics.error("E013", position, ".func expects <arity> <regs|auto>");
            return false;
        }

        if (arity > 16)
            diagnostics.error("E021", position, "arity must be in [0, 16]");
        if (!autoRegs && (regs < 1 || regs > ISA::REGISTER_COUNT))
            diagnostics.error("E020", position, "regs must be in [1, 256] or 'auto'");
        if ((flags & ~1u) != 0)
            diagnostics.error("E033", position, "only FUNCS flag bit0 (exported) is defined");

        Function function;
        function.name = name;
        function.arity = arity;
        function.regs = regs;
        function.autoRegs = autoRegs;
        function.flags = flags;
        function.entryPc = totalInstructions;
        function.position = position;
        function.displayName = name.kind == NameRef::Kind::Name ? name.name : "anonymous";
        module.functions.push_back(std::move(function));
        current = &module.functions.back();
        phase = 4;
        locationSet = false;
        locationPos = TOOL::SourcePosition{};
        missingFirstLocation = false;
        return true;
    }

    bool Parser::handleEnd(const std::vector<Token> &tokens, std::size_t index, TOOL::SourcePosition position)
    {
        if (index != tokens.size())
        {
            diagnostics.error("E013", position, ".end takes no operands");
            return false;
        }
        if (current == nullptr)
        {
            diagnostics.error("E027", position, ".end without .func");
            return false;
        }
        if (current->code.empty())
        {
            diagnostics.error("E019", position, "function body is empty");
        }
        else
        {
            const PARSE::OpInfo *info = PARSE::opcodeInfo(current->code.back().opcode);
            if (info == nullptr || !info->terminator)
                diagnostics.error("E019", position,
                                   "last instruction must be RET, HALT, TRAP or JMP");
        }
        current = nullptr;
        locationSet = false;
        return true;
    }

    bool Parser::handleEntry(const std::vector<Token> &tokens, std::size_t index)
    {
        const TOOL::SourcePosition position = tokens.empty() ? TOOL::SourcePosition{} : tokens[0].position;
        if (phase > 3)
        {
            diagnostics.error("E018", position, ".entry must appear before .func");
            return false;
        }
        if (module.hasEntry)
        {
            diagnostics.error("E030", position, "duplicate .entry");
            return false;
        }
        if (index + 1 != tokens.size())
        {
            diagnostics.error("E013", position, ".entry expects one value");
            return false;
        }
        if (!parseNameRef(tokens[index], false, module.entry))
            return false;
        module.hasEntry = true;
        phase = 3;
        return true;
    }


    bool Parser::handleLoc(const std::vector<Token> &tokens, std::size_t index)
    {
        if (current == nullptr)
        {
            diagnostics.error("E018", tokens.empty() ? TOOL::SourcePosition{} : tokens[0].position,
                               ".loc is only allowed inside .func");
            return false;
        }
        TOOL::SourcePosition position = tokens.empty() ? current->position : tokens[0].position;
        const std::vector<byte> *file = nullptr;
        if (index < tokens.size() && tokens[index].isString)
        {
            file = &tokens[index].bytes;
            position = tokens[index].position;
            ++index;
        }
        if (index >= tokens.size())
        {
            diagnostics.error("E013", position, ".loc requires a line number");
            return false;
        }
        std::uint64_t line = 0;
        if (!TOOL::parseUInt64(tokens[index].text, line) || line < 1 || line > 0x7FFFFFFFu)
        {
            diagnostics.error("E023", tokens[index].position, "line must be in [1, 2^31-1]");
            return false;
        }
        ++index;
        std::uint64_t column = 0;
        if (index < tokens.size())
        {
            if (tokens[index].isString || !TOOL::parseUInt64(tokens[index].text, column) ||
                column > 0xFFFFFFFFull)
            {
                diagnostics.error("E023", tokens[index].position, "invalid column");
                return false;
            }
            ++index;
        }
        if (index != tokens.size())
        {
            diagnostics.error("E013", tokens[index].position, ".loc has too many operands");
            return false;
        }
        if (file != nullptr)
            setSourceFile(*file, position);
        location = LineRecord{static_cast<dword>(current->code.size()),
                               static_cast<dword>(line), static_cast<dword>(column)};
        locationPos = position;
        locationSet = true;
        return true;
    }

    bool Parser::handleLocal(const std::vector<Token> &tokens, std::size_t index)
    {
        if (current == nullptr)
        {
            diagnostics.error("E018", tokens.empty() ? TOOL::SourcePosition{} : tokens[0].position,
                               ".local is only allowed inside .func");
            return false;
        }
        if (index + 2 > tokens.size())
        {
            diagnostics.error("E013", tokens.empty() ? current->position : tokens[0].position,
                               ".local expects <name>, <reg>");
            return false;
        }

        Local local;
        if (!parseNameRef(tokens[index], false, local.name))
            return false;
        ++index;
        Operand slot;
        if (!parseRegister(tokens[index], slot))
            return false;
        local.slot = slot.value;
        local.position = tokens[index - 1].position;
        ++index;

        for (; index < tokens.size(); ++index)
        {
            const Token &token = tokens[index];
            const std::string lower = TOOL::toLowerAscii(token.text);
            if (lower == "param")
            {
                local.param = true;
                continue;
            }
            const bool isStart = lower.rfind("start=", 0) == 0;
            const bool isEnd = lower.rfind("end=", 0) == 0;
            if (!isStart && !isEnd)
            {
                diagnostics.error("E013", token.position, "unexpected .local operand '" + token.text +
                                                       "'");
                return false;
            }
            if ((isStart && local.hasStart) || (isEnd && local.hasEnd))
            {
                diagnostics.error("E013", token.position, "duplicate start=/end=");
                return false;
            }
            const std::string value = token.text.substr(isStart ? 6 : 4);
            std::uint64_t number = 0;
            const bool numeric = TOOL::parseUInt64(value, number) && number <= 0xFFFFFFFFull;
            const bool label = isName(value) || isLocalLabelName(value);
            if (!numeric && !label)
            {
                diagnostics.error("E023", token.position, "invalid start=/end= value");
                return false;
            }
            if (isStart)
            {
                local.hasStart = true;
                local.startIsLabel = !numeric;
                local.startPc = numeric ? static_cast<dword>(number) : 0;
                local.startLabel = numeric ? std::string() : value;
            }
            else
            {
                local.hasEnd = true;
                local.endIsLabel = !numeric;
                local.endPc = numeric ? static_cast<dword>(number) : 0;
                local.endLabel = numeric ? std::string() : value;
            }
        }

        current->locals.push_back(std::move(local));
        return true;
    }


    bool Parser::handleMeta(const std::vector<Token> &tokens, std::size_t index, bool sourceAlias)
    {
        if (phase != 0)
        {
            diagnostics.error("E018", tokens.empty() ? TOOL::SourcePosition{} : tokens[0].position,
                               ".meta must appear before constants and functions");
            return false;
        }
        TOOL::SourcePosition position = tokens.empty() ? TOOL::SourcePosition{} : tokens[0].position;
        word key = PARSE::META_SOURCE_FILE;
        std::size_t valueIndex = index;
        if (!sourceAlias)
        {
            if (index + 2 != tokens.size())
            {
                diagnostics.error("E013", position, ".meta expects <key>, <string>");
                return false;
            }
            std::uint64_t parsedKey = 0;
            if (!TOOL::parseUInt64(tokens[index].text, parsedKey) || parsedKey == 0 ||
                parsedKey > 0xFFFFu)
            {
                diagnostics.error("E023", tokens[index].position, "meta key must be in [1, 65535]");
                return false;
            }
            key = static_cast<word>(parsedKey);
            valueIndex = index + 1;
        }
        else if (index + 1 != tokens.size())
        {
            diagnostics.error("E013", position, ".source expects one value");
            return false;
        }

        const Token &valueToken = tokens[valueIndex];
        std::vector<byte> bytes;
        if (valueToken.isString)
        {
            bytes = valueToken.bytes;
        }
        else
        {
            bytes.assign(reinterpret_cast<const byte *>(valueToken.text.data()),
                         reinterpret_cast<const byte *>(valueToken.text.data()) +
                             valueToken.text.size());
        }
        if (bytes.size() > 0xFFFFu)
        {
            diagnostics.error("E023", valueToken.position, "meta value exceeds 65535 bytes");
            return false;
        }
        const std::string value(reinterpret_cast<const char *>(bytes.data()), bytes.size());

        if (key == PARSE::META_MODULE_NAME)
        {
            if (module.hasModule)
            {
                diagnostics.error("E030", valueToken.position, "duplicate module name");
                return false;
            }
            module.hasModule = true;
            module.modulePos = valueToken.position;
            module.moduleName = value;
            return true;
        }
        if (hasMetaKey(key))
        {
            diagnostics.error("E030", valueToken.position, "duplicate meta key");
            return false;
        }
        if (key == PARSE::META_SOURCE_FILE)
            setSourceFile(bytes, valueToken.position);
        else
        {
            module.meta.push_back({key, value, valueToken.position});
            if (key > PARSE::META_COMMENT)
                diagnostics.warning("W003", valueToken.position, "unknown meta key preserved");
        }
        return true;
    }


    bool Parser::handleConstant(const std::string &directive, const std::vector<Token> &tokens,
                                std::size_t index)
    {
        const TOOL::SourcePosition position = tokens.empty() ? TOOL::SourcePosition{} : tokens[0].position;
        if (directive == ".const")
        {
            if (module.mode == ConstMode::Named)
            {
                diagnostics.error("E016", position, "named and explicit constant declarations mixed");
                return false;
            }
            module.mode = ConstMode::Explicit;
            if (phase > 1)
            {
                diagnostics.error("E018", position, "constants must appear before imports/globals");
                return false;
            }
            phase = 1;
            if (index + 1 != tokens.size())
            {
                diagnostics.error("E013", position, ".const expects exactly one literal");
                return false;
            }
            Constant constant;
            constant.named = false;
            constant.position = tokens[index].position;
            if (!parseConstantLiteral(tokens[index], constant.value))
                return false;
            module.constants.push_back(std::move(constant));
            return true;
        }

        if (module.mode == ConstMode::Explicit)
        {
            diagnostics.error("E016", position, "named and explicit constant declarations mixed");
            return false;
        }
        module.mode = ConstMode::Named;
        if (phase > 1)
        {
            diagnostics.error("E018", position, "constants must appear before imports/globals");
            return false;
        }
        phase = 1;
        if (index >= tokens.size() || tokens[index].isString)
        {
            diagnostics.error("E014", position, "constant name must be an identifier");
            return false;
        }
        const std::string name = tokens[index].text;
        if (isReserved(TOOL::toLowerAscii(name)))
        {
            diagnostics.error("E014", tokens[index].position,
                               "reserved word '" + name + "' cannot name a constant");
            return false;
        }
        if (!TOOL::isIdentifier(name))
        {
            diagnostics.error("E001", tokens[index].position, "invalid constant name");
            return false;
        }
        for (const Constant &existing : module.constants)
        {
            if (existing.named && existing.name == name)
            {
                diagnostics.error("E030", tokens[index].position, "duplicate constant '" + name + "'");
                return false;
            }
        }
        ++index;

        Constant constant;
        constant.named = true;
        constant.name = name;
        constant.position = tokens[index - 1].position;

        if (directive == ".string")
        {
            if (index + 1 != tokens.size() || !tokens[index].isString)
            {
                diagnostics.error("E013", position, ".string expects <name> <string>");
                return false;
            }
            constant.value.kind = PARSE::ConstKind::string;
            constant.value.bytes.assign(reinterpret_cast<const char *>(tokens[index].bytes.data()),
                                        tokens[index].bytes.size());
        }
        else if (directive == ".int")
        {
            if (index + 1 != tokens.size())
            {
                diagnostics.error("E013", position, ".int expects <name> <integer>");
                return false;
            }
            std::int64_t value = 0;
            if (!TOOL::parseInt64(tokens[index].text, value))
            {
                diagnostics.error(hasNumericShape(tokens[index].text) ? "E006" : "E005",
                                   tokens[index].position, "invalid integer literal");
                return false;
            }
            constant.value.kind = PARSE::ConstKind::integer;
            constant.value.bits = static_cast<std::uint64_t>(value);
        }
        else if (directive == ".float")
        {
            if (index + 1 != tokens.size())
            {
                diagnostics.error("E013", position, ".float expects <name> <number>");
                return false;
            }
            std::uint64_t bits = 0;
            const std::string literal = TOOL::toLowerAscii(tokens[index].text);
            if (literal == "nan" || literal == "+nan" || literal == "-nan")
                diagnostics.warning("W005", tokens[index].position,
                                     "NaN constants are generated from a fixed bit pattern");
            if (TOOL::parseFloatBits(tokens[index].text, bits))
            {
                constant.value.kind = PARSE::ConstKind::real;
                constant.value.bits = bits;
            }
            else
            {
                std::int64_t value = 0;
                if (!TOOL::parseInt64(tokens[index].text, value))
                {
                    diagnostics.error("E005", tokens[index].position, "invalid float literal");
                    return false;
                }
                constant.value.kind = PARSE::ConstKind::real;
                constant.value.bits = std::bit_cast<std::uint64_t>(static_cast<double>(value));
            }
        }
        else if (directive == ".bool")
        {
            if (index + 1 != tokens.size())
            {
                diagnostics.error("E013", position, ".bool expects <name> true|false");
                return false;
            }
            const std::string lower = TOOL::toLowerAscii(tokens[index].text);
            if (lower != "true" && lower != "false")
            {
                diagnostics.error("E005", tokens[index].position, "expected true or false");
                return false;
            }
            constant.value.kind = PARSE::ConstKind::boolean;
            constant.value.bits = lower == "true" ? 1u : 0u;
        }
        else // .null
        {
            if (index != tokens.size())
            {
                diagnostics.error("E013", position, ".null expects <name>");
                return false;
            }
            constant.value.kind = PARSE::ConstKind::null;
            constant.value.bits = 0;
        }

        module.constants.push_back(std::move(constant));
        return true;
    }


    bool Parser::handleImport(const std::vector<Token> &tokens, std::size_t index)
    {
        const TOOL::SourcePosition position = tokens.empty() ? TOOL::SourcePosition{} : tokens[0].position;
        if (phase > 2)
        {
            diagnostics.error("E018", position, ".import must appear before .entry/.func");
            return false;
        }
        phase = 2;
        if (index + 2 > tokens.size())
        {
            diagnostics.error("E013", position, ".import expects <name|#n> <arity>");
            return false;
        }
        Import import;
        if (!parseImportNameRef(tokens[index], import.name))
            return false;
        ++index;
        std::uint64_t arity = 0;
        if (!TOOL::parseUInt64(tokens[index].text, arity) || arity > 16)
        {
            diagnostics.error("E021", tokens[index].position, "import arity must be in [0, 16]");
            return false;
        }
        import.arity = static_cast<dword>(arity);
        import.position = tokens[index - 1].position;
        ++index;
        if (index < tokens.size())
        {
            const std::string lower = TOOL::toLowerAscii(tokens[index].text);
            if (lower.rfind("flags=", 0) != 0)
            {
                diagnostics.error("E013", tokens[index].position, "unexpected .import operand");
                return false;
            }
            std::uint64_t flags = 0;
            if (!TOOL::parseUInt64(std::string_view(tokens[index].text).substr(6), flags) ||
                flags > 0xFFu)
            {
                diagnostics.error("E033", tokens[index].position, "invalid flags value");
                return false;
            }
            if (flags != 0)
            {
                diagnostics.error("E033", tokens[index].position,
                                   "IMPORTS flags are reserved and must be 0");
                return false;
            }
            import.flags = static_cast<dword>(flags);
            ++index;
        }
        if (index != tokens.size())
        {
            diagnostics.error("E013", tokens[index].position, "too many .import operands");
            return false;
        }
        module.imports.push_back(std::move(import));
        return true;
    }

    bool Parser::handleGlobal(const std::vector<Token> &tokens, std::size_t index)
    {
        const TOOL::SourcePosition position = tokens.empty() ? TOOL::SourcePosition{} : tokens[0].position;
        if (phase > 2)
        {
            diagnostics.error("E018", position, ".global must appear before .entry/.func");
            return false;
        }
        phase = 2;
        if (index >= tokens.size())
        {
            diagnostics.error("E013", position, ".global expects a name");
            return false;
        }
        Global global;
        if (!parseNameRef(tokens[index], true, global.name))
            return false;
        global.position = tokens[index].position;
        ++index;
        if (index < tokens.size() && TOOL::toLowerAscii(tokens[index].text) == "const")
        {
            global.isConst = true;
            ++index;
        }
        if (index < tokens.size())
        {
            if (!parseInitValue(tokens[index], global))
                return false;
            if (!global.initRaw && global.init.kind == PARSE::ConstKind::null)
                diagnostics.warning("W007", global.initPos,
                                     "Null constant init differs from the omitted-init sentinel");
            ++index;
        }
        if (index != tokens.size())
        {
            diagnostics.error("E013", tokens[index].position, "too many .global operands");
            return false;
        }
        module.globals.push_back(std::move(global));
        return true;
    }

    bool Parser::parseInitValue(const Token &token, Global &global)
    {
        if (token.isString)
        {
            global.hasInit = true;
            global.initRaw = false;
            global.init.kind = PARSE::ConstKind::string;
            global.init.bytes.assign(reinterpret_cast<const char *>(token.bytes.data()),
                                     token.bytes.size());
            global.initPos = token.position;
            return true;
        }
        if (!token.text.empty() && token.text[0] == '#')
        {
            Operand index;
            if (!parseIndex(token, index))
                return false;
            global.hasInit = true;
            global.initRaw = true;
            global.initIndex = index.value;
            global.initPos = token.position;
            return true;
        }
        ConstantValue value;
        if (!parseConstantLiteral(token, value))
            return false;
        global.hasInit = true;
        global.initRaw = false;
        global.init = std::move(value);
        global.initPos = token.position;
        return true;
    }

    bool Parser::parseRegister(const Token &token, Operand &operand)
    {
        if (token.isString || token.text.size() < 2 ||
            (token.text[0] != 'r' && token.text[0] != 'R'))
        {
            diagnostics.error("E007", token.position, "expected a register r0..r255");
            return false;
        }
        std::uint64_t value = 0;
        if (!TOOL::parseUInt64(std::string_view(token.text).substr(1), value) ||
            value >= static_cast<std::uint64_t>(ISA::REGISTER_COUNT))
        {
            diagnostics.error("E007", token.position, "register number must be in [0, 255]");
            return false;
        }
        operand.kind = Operand::Kind::Reg;
        operand.value = static_cast<dword>(value);
        operand.position = token.position;
        return true;
    }

    bool Parser::parseImmediate(const Token &token, std::int64_t minimum,
                                std::int64_t maximum, Operand &operand, const char *code)
    {
        std::int64_t value = 0;
        if (token.isString || !TOOL::parseInt64(token.text, value) || value < minimum ||
            value > maximum)
        {
            diagnostics.error(code, token.position, "immediate out of range");
            return false;
        }
        operand.kind = Operand::Kind::Imm;
        operand.value = static_cast<dword>(static_cast<std::uint64_t>(value));
        operand.position = token.position;
        return true;
    }

    bool Parser::parseIndex(const Token &token, Operand &operand)
    {
        if (token.isString || token.text.empty() || token.text[0] != '#')
        {
            diagnostics.error("E024", token.position, "expected a raw index #n");
            return false;
        }
        std::uint64_t value = 0;
        if (!TOOL::parseUInt64(std::string_view(token.text).substr(1), value) || value > 0xFFFFu)
        {
            diagnostics.error("E024", token.position, "raw index must be in [0, 65535]");
            return false;
        }
        operand.kind = Operand::Kind::Index;
        operand.value = static_cast<dword>(value);
        operand.position = token.position;
        return true;
    }

    bool Parser::parseNameRef(const Token &token, bool allowAnonymous, NameRef &ref)
    {
        ref = NameRef{};
        ref.position = token.position;
        if (token.isString)
        {
            diagnostics.error("E014", token.position, "a name must not be a string literal");
            return false;
        }
        if (!token.text.empty() && token.text[0] == '#')
        {
            Operand index;
            if (!parseIndex(token, index))
                return false;
            ref.kind = NameRef::Kind::Raw;
            ref.index = index.value;
            return true;
        }
        const std::string lower = TOOL::toLowerAscii(token.text);
        if (allowAnonymous && lower == "anonymous")
        {
            ref.kind = NameRef::Kind::Anonymous;
            ref.index = NO_INDEX;
            return true;
        }
        if (isReserved(lower))
        {
            diagnostics.error("E014", token.position,
                               "reserved word '" + token.text + "' cannot be a name");
            return false;
        }
        if (!TOOL::isIdentifier(token.text))
        {
            diagnostics.error("E001", token.position, "invalid name '" + token.text + "'");
            return false;
        }
        ref.kind = NameRef::Kind::Name;
        ref.name = token.text;
        return true;
    }

    bool Parser::parseImportNameRef(const Token &token, NameRef &ref)
    {
        ref = NameRef{};
        ref.position = token.position;
        if (token.isString)
        {
            diagnostics.error("E014", token.position, "an import name must not be a string literal");
            return false;
        }
        if (!token.text.empty() && token.text[0] == '#')
        {
            Operand index;
            if (!parseIndex(token, index))
                return false;
            ref.kind = NameRef::Kind::Raw;
            ref.index = index.value;
            return true;
        }
        if (!isImportName(token.text))
        {
            diagnostics.error("E001", token.position, "invalid import name '" + token.text + "'");
            return false;
        }
        ref.kind = NameRef::Kind::Name;
        ref.name = token.text;
        return true;
    }


    bool Parser::parseOperandName(const Token &token, Operand &operand)
    {
        if (!token.isString && !token.text.empty() && token.text[0] == '#')
            return parseIndex(token, operand);
        if (token.isString || (!isName(token.text) && !isLocalLabelName(token.text)))
        {
            diagnostics.error("E011", token.position, "expected a function, import or label name");
            return false;
        }
        operand.kind = Operand::Kind::Name;
        operand.text = token.text;
        operand.position = token.position;
        return true;
    }

    bool Parser::parseImportOperand(const Token &token, Operand &operand)
    {
        if (!token.isString && !token.text.empty() && token.text[0] == '#')
            return parseIndex(token, operand);
        if (token.isString || !isImportName(token.text))
        {
            diagnostics.error("E011", token.position, "expected an import name or #index");
            return false;
        }
        operand.kind = Operand::Kind::Name;
        operand.text = token.text;
        operand.position = token.position;
        return true;
    }

    bool Parser::parseTarget(const Token &token, Operand &operand)
    {
        if (token.isString)
        {
            diagnostics.error("E011", token.position, "expected a label or relative offset");
            return false;
        }
        const std::string &text = token.text;
        if (text.size() > 1 && (text[0] == '+' || text[0] == '-'))
        {
            std::int64_t offset = 0;
            if (TOOL::parseInt64(text, offset) && offset % ISA::INSTRUCTION_SIZE == 0)
            {
                operand.kind = Operand::Kind::Relative;
                operand.offset = offset;
                operand.position = token.position;
                return true;
            }
            diagnostics.error("E012", token.position,
                               "relative offset must be a multiple of 4 bytes");
            return false;
        }
        if (!isName(text) && !isLocalLabelName(text))
        {
            diagnostics.error("E011", token.position, "unknown label '" + text + "'");
            return false;
        }
        operand.kind = Operand::Kind::Name;
        operand.text = text;
        operand.position = token.position;
        return true;
    }

    bool Parser::parseTrap(const Token &token, Operand &operand)
    {
        dword code = 0;
        std::int64_t numeric = 0;
        bool numericOk = false;
        if (!token.isString && hasNumericShape(token.text))
            numericOk = TOOL::parseInt64(token.text, numeric);
        if (numericOk)
        {
            if (numeric < 1 || numeric > 16)
            {
                diagnostics.error("E023", token.position, "trap code must be in [1, 16]");
                return false;
            }
            code = static_cast<dword>(numeric);
        }
        else if (token.isString || !TOOL::trapCode(token.text, code))
        {
            diagnostics.error("E023", token.position, "unknown trap code");
            return false;
        }
        operand.kind = Operand::Kind::Trap;
        operand.value = code;
        operand.text = token.text;
        operand.position = token.position;
        return true;
    }

    bool Parser::parseConstantLiteral(const Token &token, ConstantValue &value)
    {
        value = ConstantValue{};
        if (token.isString)
        {
            value.kind = PARSE::ConstKind::string;
            value.bytes.assign(reinterpret_cast<const char *>(token.bytes.data()),
                               token.bytes.size());
            return true;
        }
        const std::string lower = TOOL::toLowerAscii(token.text);
        if (lower == "nan" || lower == "+nan" || lower == "-nan")
            diagnostics.warning("W005", token.position,
                                 "NaN constants are generated from a fixed bit pattern");
        if (lower == "true" || lower == "false")
        {
            value.kind = PARSE::ConstKind::boolean;
            value.bits = lower == "true" ? 1u : 0u;
            return true;
        }
        if (lower == "null")
        {
            value.kind = PARSE::ConstKind::null;
            value.bits = 0;
            return true;
        }
        const bool floatShape = lower.find('.') != std::string::npos ||
                                lower.find('e') != std::string::npos ||
                                lower.rfind("nan", 0) == 0 || lower.rfind("inf", 0) == 0 ||
                                lower.rfind("-nan", 0) == 0 || lower.rfind("-inf", 0) == 0 ||
                                lower.rfind("f64=", 0) == 0;
        std::uint64_t bits = 0;
        if (floatShape && TOOL::parseFloatBits(token.text, bits))
        {
            value.kind = PARSE::ConstKind::real;
            value.bits = bits;
            return true;
        }
        std::int64_t integer = 0;
        if (!floatShape && TOOL::parseInt64(token.text, integer))
        {
            value.kind = PARSE::ConstKind::integer;
            value.bits = static_cast<std::uint64_t>(integer);
            return true;
        }
        if (TOOL::parseFloatBits(token.text, bits))
        {
            value.kind = PARSE::ConstKind::real;
            value.bits = bits;
            return true;
        }
        diagnostics.error(hasNumericShape(token.text) ? "E006" : "E005", token.position,
                           "invalid literal '" + token.text + "'");
        return false;
    }

    bool Parser::handleInstruction(const std::string &name, const std::vector<Token> &tokens,
                                   std::size_t index, TOOL::SourcePosition position)
    {
        const auto found = mnemonics().find(TOOL::toLowerAscii(name));
        if (found == mnemonics().end())
        {
            diagnostics.error("E003", position, "unknown mnemonic '" + name + "'");
            return false;
        }
        return addInstruction(found->second, tokens, index, position, currentSource);
    }

    bool Parser::addInstruction(byte opcode, const std::vector<Token> &tokens, std::size_t index,
                                TOOL::SourcePosition position, std::string sourceText)
    {
        sawStatement = true;
        if (current == nullptr)
        {
            diagnostics.error("E018", position, "instructions are only allowed inside .func");
            return false;
        }
        const PARSE::OpInfo *info = PARSE::opcodeInfo(opcode);
        if (info == nullptr)
        {
            diagnostics.error("E003", position, "reserved opcode");
            return false;
        }

        Instruction instr;
        instr.opcode = opcode;
        instr.position = position;
        instr.source = std::move(sourceText);
        const std::size_t count = tokens.size() - index;
        const auto expect = [&count, this, position](std::size_t wanted) {
            if (count != wanted)
            {
                diagnostics.error("E013", position, "wrong number of instruction operands");
                return false;
            }
            return true;
        };

        switch (info->format)
        {
        case PARSE::InstrFormat::op:
            if (!expect(0))
                return false;
            break;
        case PARSE::InstrFormat::r:
            if (!expect(1) || !parseRegister(tokens[index], instr.a))
                return false;
            break;
        case PARSE::InstrFormat::rr:
            if (!expect(2) || !parseRegister(tokens[index], instr.a) ||
                !parseRegister(tokens[index + 1], instr.b))
                return false;
            break;
        case PARSE::InstrFormat::rr8:
            if (!expect(3) || !parseRegister(tokens[index], instr.a) ||
                !parseRegister(tokens[index + 1], instr.b) ||
                !parseImmediate(tokens[index + 2], 0, 255, instr.c, "E010"))
                return false;
            break;
        case PARSE::InstrFormat::rrr:
            if (!expect(3) || !parseRegister(tokens[index], instr.a) ||
                !parseRegister(tokens[index + 1], instr.b) ||
                !parseRegister(tokens[index + 2], instr.c))
                return false;
            break;
        case PARSE::InstrFormat::ri16:
            if (opcode == ISA::jz || opcode == ISA::jnz)
            {
                if (!expect(2) || !parseRegister(tokens[index], instr.a) ||
                    !parseTarget(tokens[index + 1], instr.b))
                    return false;
            }
            else
            {
                if (!expect(2) || !parseRegister(tokens[index], instr.a) ||
                    !parseImmediate(tokens[index + 1], -32768, 65535, instr.b, "E010"))
                    return false;
            }
            break;
        case PARSE::InstrFormat::i24:
            if (!expect(1))
                return false;
            if (opcode == ISA::jmp)
            {
                if (!parseTarget(tokens[index], instr.a))
                    return false;
            }
            else if (opcode == ISA::trap)
            {
                if (!parseTrap(tokens[index], instr.a))
                    return false;
            }
            else
            {
                diagnostics.error("E003", position, "unexpected i24 instruction");
                return false;
            }
            break;
        case PARSE::InstrFormat::call16:
            if (!expect(2) || !parseRegister(tokens[index], instr.a))
                return false;
            if (opcode == ISA::call)
            {
                if (!parseOperandName(tokens[index + 1], instr.b))
                    return false;
            }
            else if (!parseImportOperand(tokens[index + 1], instr.b))
                return false;
            break;
        }

        recordLocation(static_cast<dword>(current->code.size()));
        current->code.push_back(std::move(instr));
        ++totalInstructions;
        return true;
    }
}

bool ASM::parseSource(const std::vector<byte> &source, Module &module,
                      TOOL::Diagnostics &diagnostics)
{
    module = Module{};
    Parser parser(source, module, diagnostics);
    return parser.run();
}

