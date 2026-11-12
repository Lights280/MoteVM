#include "asm/assembler.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "asm/emitter.hpp"
#include "asm/parser.hpp"
#include "common/text.hpp"
#include "parse.hpp"
#include "verifier.hpp"

namespace
{
    bool sameValue(const ASM::ConstantValue &left, const ASM::ConstantValue &right)
    {
        if (left.kind != right.kind)
            return false;
        if (left.kind == PARSE::ConstKind::string)
            return left.bytes == right.bytes;
        return left.bits == right.bits;
    }

    ASM::ConstantValue stringValue(const std::string &text)
    {
        ASM::ConstantValue value;
        value.kind = PARSE::ConstKind::string;
        value.bytes = text;
        return value;
    }

    bool findConstant(const ASM::Module &module, const ASM::ConstantValue &value, dword &index)
    {
        for (std::size_t i = 0; i < module.constants.size(); ++i)
        {
            if (sameValue(module.constants[i].value, value))
            {
                index = static_cast<dword>(i);
                return true;
            }
        }
        return false;
    }

    bool internConstant(ASM::Module &module, const ASM::ConstantValue &value, dword &index,
                        TOOL::Diagnostics &diagnostics, TOOL::SourcePosition position)
    {
        if (findConstant(module, value, index))
            return true;
        if (module.mode == ASM::ConstMode::Explicit)
        {
            diagnostics.error("E017", position, "constant is not declared in explicit mode");
            return false;
        }
        if (module.constants.size() >= PARSE::MAX_STATIC_COUNT)
        {
            diagnostics.error("E015", position, "constant pool exceeds 65536 entries");
            return false;
        }
        ASM::Constant constant;
        constant.named = false;
        constant.value = value;
        constant.position = position;
        module.constants.push_back(std::move(constant));
        index = static_cast<dword>(module.constants.size() - 1);
        return true;
    }

    bool resolveName(ASM::Module &module, ASM::NameRef &ref, TOOL::Diagnostics &diagnostics)
    {
        if (ref.kind == ASM::NameRef::Kind::Anonymous)
        {
            ref.index = NO_INDEX;
            return true;
        }
        if (ref.kind == ASM::NameRef::Kind::Raw)
        {
            if (ref.index >= module.constants.size() ||
                module.constants[ref.index].value.kind != PARSE::ConstKind::string)
            {
                diagnostics.error("E023", ref.position,
                                  "raw name index must reference a String constant");
                return false;
            }
            return true;
        }
        return internConstant(module, stringValue(ref.name), ref.index, diagnostics, ref.position);
    }

    std::string nameOf(const ASM::Module &module, const ASM::NameRef &ref)
    {
        if (ref.index == NO_INDEX || ref.index >= module.constants.size())
            return std::string();
        return module.constants[ref.index].value.bytes;
    }

    bool isTerminator(byte opcode)
    {
        const PARSE::OpInfo *info = PARSE::opcodeInfo(opcode);
        return info != nullptr && info->terminator;
    }

    ASM::Label *findLabel(ASM::Function &function, std::string_view name)
    {
        for (ASM::Label &label : function.labels)
            if (label.name == name)
                return &label;
        return nullptr;
    }

    bool labelExistsElsewhere(const ASM::Module &module, std::size_t functionIndex,
                              std::string_view name)
    {
        for (std::size_t i = 0; i < module.functions.size(); ++i)
        {
            if (i == functionIndex)
                continue;
            for (const ASM::Label &label : module.functions[i].labels)
                if (label.name == name)
                    return true;
        }
        return false;
    }

    void collectMaxRegister(const ASM::Operand &operand, dword &maximum)
    {
        if (operand.kind == ASM::Operand::Kind::Reg && operand.value > maximum)
            maximum = operand.value;
    }
}

namespace
{
    bool finalizeModule(ASM::Module &module, const ASM::Options &options,
                        TOOL::Diagnostics &diagnostics, bool &debugSection)
    {
        debugSection = false;
        if (module.functions.empty())
        {
            diagnostics.error("E020", TOOL::SourcePosition{1, 1}, "module must contain at least one function");
            return false;
        }
        if (module.mode == ASM::ConstMode::Unset)
            module.mode = ASM::ConstMode::Named;
        if (module.constants.size() > PARSE::MAX_STATIC_COUNT)
        {
            diagnostics.error("E015", TOOL::SourcePosition{1, 1}, "constant pool exceeds 65536 entries");
            return false;
        }
        if (module.functions.size() > PARSE::MAX_STATIC_COUNT ||
            module.globals.size() > PARSE::MAX_STATIC_COUNT ||
            module.imports.size() > PARSE::MAX_STATIC_COUNT)
        {
            diagnostics.error("E015", TOOL::SourcePosition{1, 1},
                              "function, global or import table exceeds 65536 entries");
            return false;
        }

        // Constant pool order (plan/05 §6.4): named declarations, import names,
        // global names, function names, global init literals, local names.
        for (ASM::Import &import : module.imports)
            resolveName(module, import.name, diagnostics);
        for (ASM::Global &global : module.globals)
            resolveName(module, global.name, diagnostics);
        for (ASM::Function &function : module.functions)
            resolveName(module, function.name, diagnostics);
        for (ASM::Global &global : module.globals)
        {
            if (!global.hasInit)
                continue;
            if (global.initRaw)
            {
                if (global.initIndex >= module.constants.size())
                    diagnostics.error("E023", global.initPos, "global init index is out of range");
            }
            else
            {
                internConstant(module, global.init, global.initIndex, diagnostics, global.initPos);
            }
        }
        for (ASM::Function &function : module.functions)
        {
            for (ASM::Local &local : function.locals)
            {
                resolveName(module, local.name, diagnostics);
                if (local.name.index == NO_INDEX ||
                    local.name.index >= module.constants.size() ||
                    module.constants[local.name.index].value.kind != PARSE::ConstKind::string)
                    diagnostics.error("E023", local.name.position,
                                      "local name must reference a String constant");
            }
        }
        if (module.constants.size() > PARSE::MAX_STATIC_COUNT)
            diagnostics.error("E015", TOOL::SourcePosition{1, 1}, "constant pool exceeds 65536 entries");

        std::unordered_set<std::string> importNames;
        for (const ASM::Import &import : module.imports)
        {
            if (import.name.index == NO_INDEX || import.name.index >= module.constants.size() ||
                module.constants[import.name.index].value.kind != PARSE::ConstKind::string)
            {
                diagnostics.error("E023", import.name.position,
                                  "import name must reference a String constant");
                continue;
            }
            if (import.arity > 16)
                diagnostics.error("E021", import.position, "import arity must be in [0, 16]");
            if (import.flags != 0)
                diagnostics.error("E033", import.position, "IMPORTS flags must be 0");
            if (!importNames.insert(nameOf(module, import.name)).second)
                diagnostics.error("E030", import.name.position, "duplicate import name");
        }

        std::unordered_set<std::string> globalNames;
        for (const ASM::Global &global : module.globals)
        {
            if (global.name.index != NO_INDEX &&
                (global.name.index >= module.constants.size() ||
                 module.constants[global.name.index].value.kind != PARSE::ConstKind::string))
            {
                diagnostics.error("E023", global.name.position,
                                  "global name must reference a String constant");
            }
            if (global.hasInit && global.initIndex >= module.constants.size())
                diagnostics.error("E023", global.initPos, "global init index is out of range");
            if (global.name.index != NO_INDEX &&
                !globalNames.insert(nameOf(module, global.name)).second)
                diagnostics.error("E030", global.name.position, "duplicate global name");
        }

        std::unordered_set<std::string> functionNames;
        dword entryPc = 0;
        for (ASM::Function &function : module.functions)
        {
            if (function.name.index != NO_INDEX &&
                (function.name.index >= module.constants.size() ||
                 module.constants[function.name.index].value.kind != PARSE::ConstKind::string))
            {
                diagnostics.error("E023", function.name.position,
                                  "function name must reference a String constant");
            }
            if (function.name.index != NO_INDEX &&
                !functionNames.insert(nameOf(module, function.name)).second)
                diagnostics.error("E030", function.name.position, "duplicate function name");
            if ((function.flags & ~1u) != 0)
                diagnostics.error("E033", function.position, "only FUNCS flag bit0 is defined");
            if (function.code.empty())
                diagnostics.error("E019", function.position, "function body is empty");
            else if (!isTerminator(function.code.back().opcode))
                diagnostics.error("E019", function.position,
                                  "last instruction must be RET, HALT, TRAP or JMP");
            function.entryPc = entryPc;
            entryPc += static_cast<dword>(function.code.size());
        }


        // Entry function (FUNCS index, arity must be 0).
        module.entryFunction = 0;
        if (module.hasEntry)
        {
            if (module.entry.kind == ASM::NameRef::Kind::Raw)
            {
                if (module.entry.index >= module.functions.size())
                    diagnostics.error("E026", module.entry.position,
                                      "entry function index is out of range");
                else
                    module.entryFunction = module.entry.index;
            }
            else if (module.entry.kind == ASM::NameRef::Kind::Name)
            {
                bool found = false;
                for (std::size_t i = 0; i < module.functions.size(); ++i)
                {
                    if (module.functions[i].name.kind == ASM::NameRef::Kind::Name &&
                        module.functions[i].name.name == module.entry.name)
                    {
                        module.entryFunction = static_cast<dword>(i);
                        found = true;
                        break;
                    }
                }
                if (!found)
                    diagnostics.error("E026", module.entry.position,
                                      "entry function '" + module.entry.name + "' not found");
            }
            else
            {
                diagnostics.error("E026", module.entry.position, "entry function cannot be anonymous");
            }
        }
        if (module.entryFunction < module.functions.size() &&
            module.functions[module.entryFunction].arity != 0)
            diagnostics.error("E026",
                              module.hasEntry ? module.entry.position : module.functions[0].position,
                              "entry function must have arity 0");

        bool anyDebug = false;
        for (std::size_t i = 0; i < module.functions.size(); ++i)
        {
            ASM::Function &function = module.functions[i];
            if (function.locals.size() > PARSE::MAX_STATIC_COUNT)
                diagnostics.error("E015", function.position,
                                  "local table exceeds 65536 entries");

            // W001: follow real control flow (including branch targets) so an
            // early TERMINATOR does not mark the rest of the function dead.
            const auto targetPc = [&function](const ASM::Operand &operand, std::size_t pc,
                                              std::int64_t &target) {
                if (operand.kind == ASM::Operand::Kind::Name)
                {
                    const ASM::Label *label = findLabel(function, operand.text);
                    if (label == nullptr)
                        return false;
                    target = static_cast<std::int64_t>(label->pc);
                    return true;
                }
                if (operand.kind == ASM::Operand::Kind::Relative)
                {
                    target = static_cast<std::int64_t>(pc) + 1 +
                             operand.offset / ISA::INSTRUCTION_SIZE;
                    return true;
                }
                return false;
            };
            std::vector<bool> reachable(function.code.size(), false);
            std::vector<dword> pending;
            if (!function.code.empty())
            {
                reachable[0] = true;
                pending.push_back(0);
            }
            const auto pushPc = [&function, &reachable, &pending](std::int64_t pc) {
                if (pc >= 0 && pc < static_cast<std::int64_t>(function.code.size()))
                {
                    const std::size_t target = static_cast<std::size_t>(pc);
                    if (!reachable[target])
                    {
                        reachable[target] = true;
                        pending.push_back(static_cast<dword>(target));
                    }
                }
            };
            while (!pending.empty())
            {
                const dword pc = pending.back();
                pending.pop_back();
                const ASM::Instruction &instruction = function.code[pc];
                std::int64_t target = 0;
                switch (instruction.opcode)
                {
                case ISA::jmp:
                    if (targetPc(instruction.a, pc, target))
                        pushPc(target);
                    break;
                case ISA::jz:
                case ISA::jnz:
                    if (targetPc(instruction.b, pc, target))
                        pushPc(target);
                    pushPc(static_cast<std::int64_t>(pc) + 1);
                    break;
                case ISA::ret:
                case ISA::halt:
                case ISA::trap:
                    break; // no fall-through
                default:
                    pushPc(static_cast<std::int64_t>(pc) + 1);
                    break;
                }
            }
            for (std::size_t pc = 0; pc < function.code.size(); ++pc)
                if (!reachable[pc])
                    diagnostics.warning("W001", function.code[pc].position,
                                        "unreachable instruction");

            dword maximum = 0;
            for (const ASM::Instruction &instruction : function.code)
            {
                collectMaxRegister(instruction.a, maximum);
                collectMaxRegister(instruction.b, maximum);
                collectMaxRegister(instruction.c, maximum);
            }
            for (const ASM::Local &local : function.locals)
                if (local.slot > maximum)
                    maximum = local.slot;

            if (function.autoRegs)
            {
                function.regs = maximum + 1;
                if (function.regs < 1)
                    function.regs = 1;
                if (function.regs > ISA::REGISTER_COUNT)
                    diagnostics.error("E020", function.position, "auto register count exceeds 256");
            }
            else
            {
                if (function.regs < 1 || function.regs > ISA::REGISTER_COUNT)
                    diagnostics.error("E020", function.position, "regs must be in [1, 256]");
                if (maximum >= function.regs)
                    diagnostics.error("E022", function.position, "register number is >= regs");
            }
            if (function.arity > function.regs)
                diagnostics.error("E022", function.position, "arity must be <= regs");


            std::unordered_set<word> slots;
            for (ASM::Local &local : function.locals)
            {
                if (!local.hasStart)
                {
                    local.startPc = 0;
                    local.startIsLabel = false;
                }
                else if (local.startIsLabel)
                {
                    ASM::Label *label = findLabel(function, local.startLabel);
                    if (label != nullptr)
                        local.startPc = label->pc;
                    else
                        diagnostics.error(
                            labelExistsElsewhere(module, i, local.startLabel) ? "E025" : "E011",
                            local.position, "unknown local start label");
                }
                if (!local.hasEnd)
                {
                    local.endPc = static_cast<dword>(function.code.size());
                    local.endIsLabel = false;
                }
                else if (local.endIsLabel)
                {
                    ASM::Label *label = findLabel(function, local.endLabel);
                    if (label != nullptr)
                        local.endPc = label->pc;
                    else
                        diagnostics.error(
                            labelExistsElsewhere(module, i, local.endLabel) ? "E025" : "E011",
                            local.position, "unknown local end label");
                }
                if (local.startPc > local.endPc ||
                    local.endPc > static_cast<dword>(function.code.size()))
                    diagnostics.error("E023", local.position, "invalid local pc range");
                if (local.slot >= function.regs)
                    diagnostics.error("E022", local.position, "local slot is >= regs");
                if (local.param && local.slot >= function.arity)
                    diagnostics.error("E023", local.position, "parameter slot must be < arity");
                slots.insert(static_cast<word>(local.slot));
            }
            std::sort(function.locals.begin(), function.locals.end(),
                      [](const ASM::Local &left, const ASM::Local &right) {
                          if (left.startPc != right.startPc)
                              return left.startPc < right.startPc;
                          return left.slot < right.slot;
                      });
            function.localCount = static_cast<word>(slots.size());

            const std::size_t instructionCount = function.code.size();
            for (std::size_t pc = 0; pc < function.lines.size(); ++pc)
            {
                const ASM::LineRecord &line = function.lines[pc];
                if (pc == 0 && line.pc != 0)
                    diagnostics.error("E034", function.position, "line table must start at pc 0");
                if (pc > 0 && line.pc <= function.lines[pc - 1].pc)
                    diagnostics.error("E034", function.position,
                                      "line table entries must be strictly increasing");
                if (line.pc >= instructionCount || line.line == 0 || line.line > 0x7FFFFFFFu)
                    diagnostics.error("E034", function.position, "invalid line table entry");
            }
            if (!function.lines.empty() || !function.locals.empty())
                anyDebug = true;
        }

        if (options.noDebug)
        {
            for (ASM::Function &function : module.functions)
            {
                function.lines.clear();
                function.locals.clear();
                function.localCount = 0;
                function.dbgOffset = NO_INDEX;
            }
            debugSection = false;
        }
        else
        {
            debugSection = options.forceDebug || anyDebug;
        }
        return diagnostics.errorCount() == 0;
    }

    bool resolveTarget(const ASM::Module &module, ASM::Function &function,
                       std::size_t functionIndex, const ASM::Operand &operand, dword pc,
                       dword &target, TOOL::Diagnostics &diagnostics)
    {
        if (operand.kind == ASM::Operand::Kind::Relative)
        {
            const std::int64_t value =
                static_cast<std::int64_t>(pc) + 1 + operand.offset / ISA::INSTRUCTION_SIZE;
            if (value < 0 || value >= static_cast<std::int64_t>(function.code.size()))
            {
                diagnostics.error("E012", operand.position,
                                  "relative jump target is outside the function");
                return false;
            }
            target = static_cast<dword>(value);
            return true;
        }
        if (operand.kind != ASM::Operand::Kind::Name)
        {
            diagnostics.error("E011", operand.position, "expected a jump label");
            return false;
        }
        ASM::Label *label = findLabel(function, operand.text);
        if (label == nullptr)
        {
            diagnostics.error(labelExistsElsewhere(module, functionIndex, operand.text) ? "E025"
                                                                                        : "E011",
                              operand.position, "unknown label '" + operand.text + "'");
            return false;
        }
        target = label->pc;
        return true;
    }

    std::string functionName(const ASM::Module &module, std::size_t index)
    {
        return nameOf(module, module.functions[index].name);
    }

    bool encodeInstructions(ASM::Module &module, TOOL::Diagnostics &diagnostics,
                            std::vector<ASM::ListingEntry> &listing)
    {
        std::unordered_map<std::string, dword> functions;
        for (std::size_t i = 0; i < module.functions.size(); ++i)
        {
            const ASM::Function &function = module.functions[i];
            if (function.name.kind == ASM::NameRef::Kind::Name)
                functions.emplace(functionName(module, i), static_cast<dword>(i));
        }
        std::unordered_map<std::string, dword> imports;
        for (std::size_t i = 0; i < module.imports.size(); ++i)
            imports.emplace(nameOf(module, module.imports[i].name), static_cast<dword>(i));

        for (std::size_t fi = 0; fi < module.functions.size(); ++fi)
        {
            ASM::Function &function = module.functions[fi];
            function.words.clear();
            for (std::size_t pc = 0; pc < function.code.size(); ++pc)
            {
                const ASM::Instruction &instruction = function.code[pc];
                const PARSE::OpInfo *info = PARSE::opcodeInfo(instruction.opcode);
                if (info == nullptr)
                {
                    diagnostics.error("E003", instruction.position, "reserved opcode");
                    return false;
                }
                byte a = 0;
                byte b = 0;
                byte c = 0;
                switch (info->format)
                {
                case PARSE::InstrFormat::op:
                    break;
                case PARSE::InstrFormat::r:
                    a = static_cast<byte>(instruction.a.value);
                    break;
                case PARSE::InstrFormat::rr:
                    a = static_cast<byte>(instruction.a.value);
                    b = static_cast<byte>(instruction.b.value);
                    break;
                case PARSE::InstrFormat::rr8:
                    a = static_cast<byte>(instruction.a.value);
                    b = static_cast<byte>(instruction.b.value);
                    c = static_cast<byte>(instruction.c.value & 0xFFu);
                    break;
                case PARSE::InstrFormat::rrr:
                    a = static_cast<byte>(instruction.a.value);
                    b = static_cast<byte>(instruction.b.value);
                    c = static_cast<byte>(instruction.c.value);
                    break;
                case PARSE::InstrFormat::ri16:
                    a = static_cast<byte>(instruction.a.value);
                    if (instruction.opcode == ISA::jz || instruction.opcode == ISA::jnz)
                    {
                        dword target = 0;
                        if (!resolveTarget(module, function, fi, instruction.b,
                                           static_cast<dword>(pc), target, diagnostics))
                            return false;
                        const std::int64_t offset =
                            (static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc) -
                             1) *
                            ISA::INSTRUCTION_SIZE;
                        if (offset < -32768 || offset > 32767)
                        {
                            diagnostics.error("E012", instruction.b.position,
                                              "branch offset does not fit in int16");
                            return false;
                        }
                        const word immediate = static_cast<word>(offset & 0xFFFF);
                        b = static_cast<byte>(immediate & 0xFFu);
                        c = static_cast<byte>((immediate >> 8) & 0xFFu);
                    }
                    else
                    {
                        const word immediate =
                            static_cast<word>(instruction.b.value & 0xFFFFu);
                        b = static_cast<byte>(immediate & 0xFFu);
                        c = static_cast<byte>((immediate >> 8) & 0xFFu);
                    }
                    break;


                case PARSE::InstrFormat::i24:
                {
                    dword bits = 0;
                    if (instruction.opcode == ISA::jmp)
                    {
                        dword target = 0;
                        if (!resolveTarget(module, function, fi, instruction.a,
                                           static_cast<dword>(pc), target, diagnostics))
                            return false;
                        const std::int64_t offset =
                            (static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc) -
                             1) *
                            ISA::INSTRUCTION_SIZE;
                        if (offset < -8388608 || offset > 8388607)
                        {
                            diagnostics.error("E012", instruction.a.position,
                                              "jump offset does not fit in int24");
                            return false;
                        }
                        bits = static_cast<dword>(offset) & 0x00FFFFFFu;
                    }
                    else
                    {
                        bits = instruction.a.value & 0x00FFFFFFu;
                    }
                    a = static_cast<byte>(bits & 0xFFu);
                    b = static_cast<byte>((bits >> 8) & 0xFFu);
                    c = static_cast<byte>((bits >> 16) & 0xFFu);
                    break;
                }
                case PARSE::InstrFormat::call16:
                {
                    a = static_cast<byte>(instruction.a.value);
                    dword index = 0;
                    if (instruction.b.kind == ASM::Operand::Kind::Name)
                    {
                        if (instruction.opcode == ISA::call)
                        {
                            const auto found = functions.find(instruction.b.text);
                            if (found == functions.end())
                            {
                                diagnostics.error("E011", instruction.b.position,
                                                  "unknown function '" + instruction.b.text + "'");
                                return false;
                            }
                            index = found->second;
                        }
                        else
                        {
                            const auto found = imports.find(instruction.b.text);
                            if (found == imports.end())
                            {
                                diagnostics.error("E011", instruction.b.position,
                                                  "unknown import '" + instruction.b.text + "'");
                                return false;
                            }
                            index = found->second;
                        }
                    }
                    else
                    {
                        index = instruction.b.value;
                    }
                    if (instruction.opcode == ISA::call)
                    {
                        if (index >= module.functions.size())
                        {
                            diagnostics.error("E023", instruction.b.position,
                                              "function index is out of range");
                            return false;
                        }
                        if (module.functions[index].arity > function.regs)
                        {
                            diagnostics.error("E022", instruction.b.position,
                                              "callee arity exceeds regs");
                            return false;
                        }
                    }
                    else
                    {
                        if (index >= module.imports.size())
                        {
                            diagnostics.error("E023", instruction.b.position,
                                              "import index is out of range");
                            return false;
                        }
                        if (module.imports[index].arity > function.regs)
                        {
                            diagnostics.error("E022", instruction.b.position,
                                              "import arity exceeds regs");
                            return false;
                        }
                    }
                    b = static_cast<byte>(index & 0xFFu);
                    c = static_cast<byte>((index >> 8) & 0xFFu);
                    break;
                }
                }
                const dword word = static_cast<dword>(instruction.opcode) |
                                   (static_cast<dword>(a) << 8) |
                                   (static_cast<dword>(b) << 16) |
                                   (static_cast<dword>(c) << 24);
                function.words.push_back(word);
                listing.push_back({function.displayName, static_cast<dword>(fi),
                                   static_cast<dword>(pc), word, instruction.source});
            }
        }
        return diagnostics.errorCount() == 0;
    }
}


ASM::AssembleResult ASM::assemble(std::span<const byte> source, const Options &options,
                          TOOL::Diagnostics &diagnostics)
{
    AssembleResult AssembleResult;
    const std::vector<byte> bytes(source.begin(), source.end());
    Module module;
    if (!parseSource(bytes, module, diagnostics))
    {
        AssembleResult.status = TOOL::INPUT;
        return AssembleResult;
    }

    bool debugSection = false;
    if (!finalizeModule(module, options, diagnostics, debugSection))
    {
        AssembleResult.status = TOOL::INPUT;
        return AssembleResult;
    }

    std::vector<ListingEntry> listing;
    if (!encodeInstructions(module, diagnostics, listing))
    {
        AssembleResult.status = TOOL::INPUT;
        return AssembleResult;
    }

    std::vector<byte> image;
    if (!emitImage(module, options, image, listing, diagnostics))
    {
        // E035 (image too large) is user input; E031-style alignment failures
        // are internal invariants but are unreachable from valid models.
        AssembleResult.status = diagnostics.errorCount() != 0 ? TOOL::INPUT : TOOL::INTERNAL;
        return AssembleResult;
    }

    // Self-check: the produced bytes must load and verify through the same
    // implementation motevm uses (plan/05 §6.3 step 6).
    Parse parser;
    int code = parser.load(std::span<const byte>(image.data(), image.size()));
    if (code == PARSE::OK)
        code = parser.parse();
    if (code != PARSE::OK)
    {
        diagnostics.error("E031", TOOL::SourcePosition{},
                          "internal consistency check failed (Parse code " +
                              std::to_string(code) + ")");
        AssembleResult.status = TOOL::INTERNAL;
        return AssembleResult;
    }

    AssembleResult.status = TOOL::OK;
    AssembleResult.image = std::move(image);
    AssembleResult.listing = std::move(listing);
    AssembleResult.moduleName = module.moduleName;
    AssembleResult.constantCount = module.constants.size();
    AssembleResult.functionCount = module.functions.size();
    AssembleResult.globalCount = module.globals.size();
    AssembleResult.importCount = module.imports.size();
    for (const Function &function : module.functions)
        AssembleResult.instructionCount += static_cast<dword>(function.words.size());
    return AssembleResult;
}

