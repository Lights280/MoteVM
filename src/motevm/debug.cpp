#include "debug.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <iomanip>
#include <istream>
#include <ostream>
#include <span>
#include <sstream>

#include "disasm.hpp"

namespace
{
    word merge(byte high, byte low)
    {
        return static_cast<word>((static_cast<word>(high) << 8) | low);
    }

    std::string spanText(std::span<const byte> bytes)
    {
        if (bytes.empty())
            return {};
        return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }

    std::string hex(dword value, int width)
    {
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(width) << std::setfill('0') << value;
        return out.str();
    }

    std::string trim(const std::string &text)
    {
        std::size_t begin = 0;
        std::size_t end = text.size();
        while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
            ++begin;
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
            --end;
        return text.substr(begin, end - begin);
    }

    bool parseNumber(const std::string &text, dword &value)
    {
        if (text.empty())
            return false;
        try
        {
            std::size_t used = 0;
            const unsigned long long parsed = std::stoull(text, &used, 0);
            if (used != text.size() || parsed > 0xFFFFFFFFull)
                return false;
            value = static_cast<dword>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool findFunction(const Parse &module, const std::string &name, dword &index)
    {
        const std::vector<PARSE::Function> &functions = module.image().functions;
        if (parseNumber(name, index))
            return index < functions.size();
        for (dword i = 0; i < static_cast<dword>(functions.size()); ++i)
            if (DEBUG::functionName(module, i) == name)
            {
                index = i;
                return true;
            }
        return false;
    }

    const char *traceStatus(int error)
    {
        switch (error)
        {
        case CORE::OK:
            return "halted";
        case CORE::TRAP:
            return "trap";
        case CORE::MEMORY_FAULT:
            return "memory-fault";
        case CORE::FUEL_EXHAUSTED:
            return "fuel-exhausted";
        case CORE::INTERRUPTED:
            return "interrupted";
        case CORE::DEBUG_BREAK:
            return "debug-stop";
        default:
            return "error";
        }
    }

    // Breakpoint grammar (plan/self/debug.md):
    //   #N          instruction index in the module CODE
    //   N           byte offset (must be 4-byte aligned)
    //   name[:off]  function entry, or entry + instruction offset
    bool parseBreakSpec(const Parse &module, const std::string &spec, dword &pc,
                        std::string &error)
    {
        const bool indexed = !spec.empty() && spec[0] == '#';
        std::string text = indexed ? spec.substr(1) : spec;
        text = trim(text);
        if (text.empty())
        {
            error = "empty breakpoint";
            return false;
        }

        const std::size_t colon = text.find(':');
        if (colon != std::string::npos)
        {
            const std::string left = trim(text.substr(0, colon));
            const std::string right = trim(text.substr(colon + 1));
            dword function = NO_INDEX;
            dword offset = 0;
            if (!findFunction(module, left, function))
            {
                error = "unknown function '" + left + "'";
                return false;
            }
            if (!parseNumber(right, offset))
            {
                error = "invalid instruction offset '" + right + "'";
                return false;
            }
            const PARSE::Function &fn = module.image().functions[function];
            if (offset >= fn.instr)
            {
                error = "instruction offset outside the function";
                return false;
            }
            pc = (fn.entry + offset) * ISA::INSTRUCTION_SIZE;
        }
        else if (indexed)
        {
            dword index = 0;
            if (!parseNumber(text, index))
            {
                error = "invalid instruction index '" + text + "'";
                return false;
            }
            pc = index * ISA::INSTRUCTION_SIZE;
        }
        else
        {
            dword value = 0;
            dword function = NO_INDEX;
            if (parseNumber(text, value))
            {
                if (value % ISA::INSTRUCTION_SIZE != 0)
                {
                    error = "byte offset must be a multiple of 4";
                    return false;
                }
                pc = value;
            }
            else if (findFunction(module, text, function))
            {
                pc = module.image().functions[function].entry * ISA::INSTRUCTION_SIZE;
            }
            else
            {
                error = "unknown breakpoint '" + text + "'";
                return false;
            }
        }

        const dword count = DEBUG::codeCount(module);
        if (pc % ISA::INSTRUCTION_SIZE != 0)
        {
            error = "instruction address must be a multiple of 4";
            return false;
        }
        if (pc / ISA::INSTRUCTION_SIZE >= count)
        {
            error = "instruction index outside CODE";
            return false;
        }
        return true;
    }
}

dword DEBUG::codeCount(const Parse &module)
{
    return module.image().codeCount;
}

dword DEBUG::functionAt(const Parse &module, dword instructionIndex)
{
    const std::vector<PARSE::Function> &functions = module.image().functions;
    for (dword i = 0; i < static_cast<dword>(functions.size()); ++i)
    {
        const PARSE::Function &fn = functions[i];
        if (instructionIndex >= fn.entry && instructionIndex - fn.entry < fn.instr)
            return i;
    }
    return NO_INDEX;
}

std::string DEBUG::constantText(const Parse &module, dword constantIndex)
{
    const std::vector<PARSE::Constant> &constants = module.image().constants;
    if (constantIndex >= constants.size())
        return {};
    if (constants[constantIndex].kind != PARSE::ConstKind::string)
        return {};
    return spanText(constants[constantIndex].payload);
}

std::string DEBUG::functionName(const Parse &module, dword function)
{
    const std::vector<PARSE::Function> &functions = module.image().functions;
    if (function >= functions.size())
        return {};
    return constantText(module, functions[function].name);
}

DEBUG::SourceLocation DEBUG::locate(const Parse &module, dword instructionIndex)
{
    SourceLocation location;
    const PARSE::Image &image = module.image();
    location.moduleName = spanText(image.moduleName);
    location.sourceFile = spanText(image.sourceFile);

    const dword function = functionAt(module, instructionIndex);
    if (function == NO_INDEX)
        return location;

    location.known = true;
    location.function = function;
    location.functionName = functionName(module, function);
    location.relativePc = instructionIndex - image.functions[function].entry;

    if (function >= image.debug.size())
        return location;

    const std::vector<PARSE::LineEntry> &lines = image.debug[function].lines;
    if (lines.empty())
        return location;

    // Last entry whose pc_offset <= relativePc (V73 keeps them strictly increasing).
    const auto it = std::upper_bound(
        lines.begin(), lines.end(), location.relativePc,
        [](dword value, const PARSE::LineEntry &entry) { return value < entry.pc; });
    if (it == lines.begin())
        return location;
    const PARSE::LineEntry &entry = *(it - 1);
    location.line = entry.line;
    location.column = entry.column;
    return location;
}

std::vector<DEBUG::LocalValue> DEBUG::localsAt(const Parse &module, const Core &core,
                                               dword instructionIndex)
{
    std::vector<LocalValue> values;
    const dword function = functionAt(module, instructionIndex);
    if (function == NO_INDEX)
        return values;

    const PARSE::Image &image = module.image();
    if (function >= image.debug.size())
        return values;

    const std::span<const StackFrame> frames = core.stack();
    if (frames.empty())
        return values;
    const StackFrame &frame = frames.back();
    const dword relativePc = instructionIndex - image.functions[function].entry;

    for (const PARSE::LocalEntry &entry : image.debug[function].locals)
    {
        if (relativePc < entry.startPc || relativePc >= entry.endPc)
            continue;
        LocalValue value;
        value.name = constantText(module, entry.name);
        value.slot = entry.slot;
        value.parameter = (entry.flags & PARSE::LOCAL_FLAG_PARAMETER) != 0;
        if (entry.slot < ISA::REGISTER_COUNT)
            value.value = frame.registers[entry.slot];
        values.push_back(std::move(value));
    }
    return values;
}

std::string DEBUG::instructionText(const Parse &module, dword instructionIndex)
{
    if (instructionIndex >= module.image().codeCount)
        return "<out of range>";
    return DISASM::format(codeWord(module, instructionIndex));
}

dword DEBUG::codeWord(const Parse &module, dword instructionIndex)
{
    const PARSE::Image &image = module.image();
    if (instructionIndex >= image.codeCount)
        return 0;
    const std::size_t offset = static_cast<std::size_t>(instructionIndex) * ISA::INSTRUCTION_SIZE;
    const std::span<const byte> code = image.code;
    if (offset + ISA::INSTRUCTION_SIZE > code.size())
        return 0;
    return static_cast<dword>(code[offset]) | (static_cast<dword>(code[offset + 1]) << 8) |
           (static_cast<dword>(code[offset + 2]) << 16) |
           (static_cast<dword>(code[offset + 3]) << 24);
}

// ---------------------------------------------------------------- TraceWriter

DEBUG::TraceWriter::TraceWriter(const Parse &module, TraceLevel level, std::ostream &out,
                                dword limit)
    : module_(module), level_(level), out_(out), limit_(limit)
{
}

void DEBUG::TraceWriter::emit(const std::string &line)
{
    if (limited_)
        return;
    if (!headerWritten_)
    {
        out_ << "# motevm trace v1\n";
        headerWritten_ = true;
    }
    if (limit_ != 0 && count_ >= limit_)
    {
        limited_ = true;
        return;
    }
    out_ << line << '\n';
    ++count_;
}

std::string DEBUG::TraceWriter::functionField(dword instructionIndex) const
{
    const dword function = functionAt(module_, instructionIndex);
    if (function == NO_INDEX)
        return "?";
    return std::to_string(function);
}

void DEBUG::TraceWriter::instruction(const Core &core, dword pc, dword encoded)
{
    const byte opcode = static_cast<byte>(encoded & ISA::OPCODE_MASK);
    const byte a = static_cast<byte>((encoded >> ISA::OPERAND1_SHIFT) & ISA::OPERAND_MASK);
    const byte b = static_cast<byte>((encoded >> ISA::OPERAND2_SHIFT) & ISA::OPERAND_MASK);
    const byte c = static_cast<byte>((encoded >> ISA::OPERAND3_SHIFT) & ISA::OPERAND_MASK);
    const dword instructionIndex = pc / ISA::INSTRUCTION_SIZE;

    if (level_ == TraceLevel::exec || level_ == TraceLevel::full)
    {
        std::ostringstream line;
        line << "E seq=" << count_ << " f=" << functionField(instructionIndex) << " pc=" << pc
             << " op=" << DISASM::format(encoded);
        const SourceLocation location = locate(module_, instructionIndex);
        if (location.known && !location.sourceFile.empty() && location.line != 0)
            line << " src=\"" << location.sourceFile << ':' << location.line << ':' << location.column
                 << '"';
        emit(line.str());
    }

    switch (opcode)
    {
    case ISA::call:
    {
        const dword callee = merge(c, b);
        std::ostringstream line;
        line << "C seq=" << count_ << " f=" << functionField(instructionIndex) << " pc=" << pc
             << " callee=#" << callee;
        const std::vector<PARSE::Function> &functions = module_.image().functions;
        if (callee < functions.size())
            line << " arity=" << static_cast<int>(functions[callee].arity);
        line << " dest=r" << static_cast<int>(a);
        emit(line.str());
        break;
    }
    case ISA::ret:
    {
        const std::span<const StackFrame> frames = core.stack();
        const dword value = frames.empty() ? 0 : frames.back().registers[a];
        std::ostringstream line;
        line << "R seq=" << count_ << " f=" << functionField(instructionIndex) << " pc=" << pc
             << " value=" << std::bit_cast<int>(value);
        emit(line.str());
        break;
    }
    case ISA::trap:
    {
        const dword code = static_cast<dword>(a) | (static_cast<dword>(b) << 8) |
                           (static_cast<dword>(c) << 16);
        std::ostringstream line;
        line << "T seq=" << count_ << " f=" << functionField(instructionIndex) << " pc=" << pc
             << " code=" << code;
        emit(line.str());
        break;
    }
    default:
        break;
    }

    if (level_ == TraceLevel::full)
    {
        const std::span<const StackFrame> frames = core.stack();
        if (!frames.empty())
        {
            const dword function = functionAt(module_, instructionIndex);
            dword registers = ISA::REGISTER_COUNT;
            if (function != NO_INDEX)
                registers = module_.image().functions[function].regcnt;
            registers = std::min<dword>(registers, 16);
            std::ostringstream line;
            line << "G seq=" << count_ << " f=" << functionField(instructionIndex) << " pc=" << pc;
            for (dword i = 0; i < registers; ++i)
                line << " r" << i << '=' << hex(frames.back().registers[i], 8);
            emit(line.str());
        }
    }
}

void DEBUG::TraceWriter::snapshot(const Core &core, dword pc)
{
    std::ostringstream line;
    line << "S depth=" << core.callDepth() << " f=" << functionField(pc / ISA::INSTRUCTION_SIZE)
         << " pc=" << pc;
    emit(line.str());
}

void DEBUG::TraceWriter::stopped(const Core &core, int error)
{
    if (limited_)
    {
        out_ << "X status=trace-limit\n";
        return;
    }
    if (error != CORE::DEBUG_BREAK)
        snapshot(core, core.errorPc());
    if (error == CORE::TRAP)
        out_ << "X status=" << traceStatus(error) << " trap=" << core.trapCode() << '\n';
    else if (error == CORE::OK)
        out_ << "X status=halted\n";
    else
        out_ << "X status=" << traceStatus(error) << " code=" << error << '\n';
}

// ------------------------------------------------------------------- Session

DEBUG::Session::Session(const Parse &module, Memory &memory, std::istream &in,
                        std::ostream &diag)
    : module_(module), memory_(memory), in_(in), diag_(diag)
{
}

bool DEBUG::Session::addBreakpoint(const std::string &spec, std::string &error)
{
    dword pc = 0;
    if (!parseBreakSpec(module_, spec, pc, error))
        return false;
    if (breakSet_.count(pc) != 0)
    {
        error = "breakpoint already set at pc=" + std::to_string(pc);
        return false;
    }
    breaks_.push_back({nextBreakId_++, pc});
    breakSet_.insert(pc);
    return true;
}

void DEBUG::Session::setTrace(TraceLevel level, std::ostream &out, dword limit)
{
    if (level == TraceLevel::off)
        trace_.reset();
    else
        trace_ = std::make_unique<TraceWriter>(module_, level, out, limit);
}

bool DEBUG::Session::hook(void *user, const Core &core, dword pc, dword encoded)
{
    return static_cast<Session *>(user)->onInstruction(core, pc, encoded);
}

bool DEBUG::Session::onInstruction(const Core &core, dword pc, dword encoded)
{
    if (trace_ != nullptr)
        trace_->instruction(core, pc, encoded);
    if (!interactive_)
        return false;
    if (quit_ || abort_)
        return true;

    if (!entered_)
    {
        entered_ = true;
        if (startStopped_)
            return stopAt(core, pc);
    }

    switch (mode_)
    {
    case Mode::Step:
        if (stepSkips_ > 0)
        {
            --stepSkips_;
            return false;
        }
        return stopAt(core, pc);
    case Mode::Next:
        return core.callDepth() <= stopDepth_ ? stopAt(core, pc) : false;
    case Mode::Finish:
        return core.callDepth() < stopDepth_ ? stopAt(core, pc) : false;
    case Mode::Continue:
        break;
    }

    if (skipPc_ == pc)
    {
        skipPc_ = NO_INDEX;
        return false;
    }
    if (breakSet_.count(pc) != 0)
        return stopAt(core, pc);
    return false;
}

bool DEBUG::Session::stopAt(const Core &core, dword pc)
{
    stopPc_ = pc;
    if (trace_ != nullptr)
        trace_->snapshot(core, pc);
    if (prompt_)
        diag_ << "(motevm) " << std::flush;

    std::string line;
    while (std::getline(in_, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::istringstream stream(line);
        if (!dispatch(core, stream, pc))
            return quit_ || abort_;
        if (prompt_)
            diag_ << "(motevm) " << std::flush;
    }

    quit_ = true; // EOF ends the session
    return true;
}

bool DEBUG::Session::dispatch(const Core &core, std::istringstream &line, dword pc)
{
    std::string command;
    if (!(line >> command))
        return true; // empty line: keep prompting
    for (char &character : command)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

    if (command == "h" || command == "help" || command == "?")
    {
        printHelp(diag_);
        return true;
    }
    if (command == "c" || command == "continue")
    {
        mode_ = Mode::Continue;
        skipPc_ = breakSet_.count(pc) != 0 ? pc : NO_INDEX;
        return false;
    }
    if (command == "s" || command == "step")
    {
        dword count = 1;
        if (!(line >> count) || count == 0)
            count = 1;
        if (count > 1000000)
            count = 1000000;
        mode_ = Mode::Step;
        stepSkips_ = count - 1;
        return false;
    }
    if (command == "n" || command == "next")
    {
        stopDepth_ = static_cast<dword>(core.callDepth());
        const dword next = codeWord(module_, pc / ISA::INSTRUCTION_SIZE);
        if ((next & ISA::OPCODE_MASK) == ISA::call)
        {
            mode_ = Mode::Next; // step over: run until the callee returns
        }
        else
        {
            mode_ = Mode::Step; // nothing to step over: behave like "step 1"
            stepSkips_ = 0;
        }
        return false;
    }
    if (command == "f" || command == "finish")
    {
        mode_ = Mode::Finish;
        stopDepth_ = static_cast<dword>(core.callDepth());
        return false;
    }
    if (command == "q" || command == "quit")
    {
        quit_ = true;
        return false;
    }
    if (command == "abort" || command == "kill")
    {
        abort_ = true;
        return false;
    }
    if (command == "b" || command == "break")
    {
        std::string spec;
        if (!(line >> spec))
        {
            diag_ << "error: usage: break <#index|offset|function[:offset]>\n";
            return true;
        }
        std::string error;
        if (!addBreakpoint(spec, error))
        {
            diag_ << "error: breakpoint '" << spec << "': " << error << '\n';
            return true;
        }
        diag_ << "breakpoint " << breaks_.back().id << " at pc=" << breaks_.back().pc << '\n';
        return true;
    }
    if (command == "d" || command == "delete")
    {
        std::string argument;
        if (!(line >> argument))
        {
            diag_ << "error: usage: delete <id|*>\n";
            return true;
        }
        if (argument == "*")
        {
            breaks_.clear();
            breakSet_.clear();
            diag_ << "all breakpoints deleted\n";
            return true;
        }
        dword id = 0;
        if (!parseNumber(argument, id))
        {
            diag_ << "error: invalid breakpoint id '" << argument << "'\n";
            return true;
        }
        if (deleteBreakpoint(id, diag_))
            diag_ << "breakpoint " << id << " deleted\n";
        return true;
    }
    if (command == "info" || command == "i")
    {
        std::string topic;
        line >> topic;
        if (topic == "breakpoints" || topic == "break" || topic == "b")
        {
            printBreakpoints(diag_);
            return true;
        }
        diag_ << "error: usage: info breakpoints\n";
        return true;
    }
    if (command == "bt" || command == "backtrace" || command == "where")
    {
        printBacktrace(core, pc, diag_);
        return true;
    }
    if (command == "regs" || command == "registers" || command == "r")
    {
        dword start = 0;
        dword count = 16;
        std::string first;
        if (line >> first)
        {
            if (first == "all")
                count = currentRegisterCount(core, pc);
            else if (!parseNumber(first, start))
            {
                diag_ << "error: invalid register index '" << first << "'\n";
                return true;
            }
            else if (!(line >> count))
                count = 16;
        }
        printRegisters(core, pc, start, count, diag_);
        return true;
    }
    if (command == "p" || command == "print")
    {
        std::string spec;
        if (!(line >> spec))
        {
            diag_ << "error: usage: print <rN>\n";
            return true;
        }
        if (spec.size() > 1 && (spec[0] == 'r' || spec[0] == 'R'))
            spec.erase(0, 1);
        dword index = 0;
        if (!parseNumber(spec, index) || index >= ISA::REGISTER_COUNT)
        {
            diag_ << "error: invalid register '" << spec << "'\n";
            return true;
        }
        const std::span<const StackFrame> frames = core.stack();
        if (frames.empty())
        {
            diag_ << "no active frame\n";
            return true;
        }
        const dword value = frames.back().registers[index];
        diag_ << 'r' << index << " = " << hex(value, 8) << " (" << std::bit_cast<int>(value)
              << ")\n";
        return true;
    }
    if (command == "x" || command == "mem" || command == "x/mem")
    {
        std::string addressText;
        dword address = 0;
        dword length = 16;
        if (!(line >> addressText) || !parseNumber(addressText, address))
        {
            diag_ << "error: usage: x <address> [length]\n";
            return true;
        }
        if (!(line >> length) || length == 0)
            length = 16;
        printMemory(address, length, diag_);
        return true;
    }
    if (command == "disasm" || command == "u")
    {
        dword start = pc;
        dword count = 8;
        std::string startText;
        if (line >> startText)
        {
            const bool indexed = !startText.empty() && startText[0] == '#';
            const std::string number = indexed ? startText.substr(1) : startText;
            dword value = 0;
            if (!parseNumber(number, value))
            {
                diag_ << "error: invalid instruction address '" << startText << "'\n";
                return true;
            }
            start = indexed ? value * ISA::INSTRUCTION_SIZE : value;
        }
        if (!(line >> count) || count == 0)
            count = 8;
        printDisassembly(pc, start, count, diag_);
        return true;
    }
    if (command == "locals")
    {
        printLocals(core, pc, diag_);
        return true;
    }
    if (command == "location" || command == "loc" || command == "line")
    {
        printLocation(core, pc, diag_);
        return true;
    }

    diag_ << "error: unknown command '" << command << "' (try help)\n";
    return true;
}

bool DEBUG::Session::deleteBreakpoint(dword id, std::ostream &out)
{
    for (auto it = breaks_.begin(); it != breaks_.end(); ++it)
    {
        if (it->id != id)
            continue;
        breakSet_.erase(it->pc);
        breaks_.erase(it);
        return true;
    }
    out << "error: no breakpoint " << id << '\n';
    return false;
}

dword DEBUG::Session::currentRegisterCount(const Core &core, dword pc) const
{
    (void)core;
    const dword function = functionAt(module_, pc / ISA::INSTRUCTION_SIZE);
    if (function == NO_INDEX)
        return ISA::REGISTER_COUNT;
    return std::min<dword>(module_.image().functions[function].regcnt, ISA::REGISTER_COUNT);
}

void DEBUG::Session::printHelp(std::ostream &out) const
{
    out << "commands:\n"
           "  help, h, ?             show this help\n"
           "  continue, c            resume execution\n"
           "  step, s [N]            execute N instruction(s), then stop\n"
           "  next, n                step over calls\n"
           "  finish, f              run until the current frame returns\n"
           "  break, b <spec>        set a breakpoint: #index, offset, function[:offset]\n"
           "  delete, d <id|*>       delete breakpoints\n"
           "  info breakpoints       list breakpoints\n"
           "  backtrace, bt, where   show the call stack\n"
           "  regs, r [start [n]]    show registers ('r all' for the whole window)\n"
           "  print, p <rN>          show one register in hex and signed\n"
           "  x <addr> [len]         dump memory (1..256 bytes)\n"
           "  disasm, u [#pc] [n]    disassemble n instructions\n"
           "  locals                 show named locals of the current frame\n"
           "  location, loc          show the current source location\n"
           "  quit, q                stop the program (exit 0)\n"
           "  abort, kill            stop the program (exit 3)\n";
}

void DEBUG::Session::printLocation(const Core &core, dword pc, std::ostream &out) const
{
    (void)core;
    const dword instructionIndex = pc / ISA::INSTRUCTION_SIZE;
    const SourceLocation location = locate(module_, instructionIndex);
    out << "pc=" << pc << " (inst #" << instructionIndex << ')';
    if (location.known)
    {
        out << " fn=#" << location.function;
        if (!location.functionName.empty())
            out << " '" << location.functionName << '\'';
        if (!location.sourceFile.empty() && location.line != 0)
            out << " src=" << location.sourceFile << ':' << location.line << ':'
                << location.column;
    }
    else
    {
        out << " (no debug info)";
    }
    out << '\n';
}

void DEBUG::Session::printBacktrace(const Core &core, dword pc, std::ostream &out) const
{
    const std::span<const StackFrame> frames = core.stack();
    if (frames.empty())
    {
        out << "no active frame\n";
        return;
    }

    out << "stack (innermost first):\n";
    for (std::size_t i = frames.size(); i-- > 0;)
    {
        const bool innermost = i + 1 == frames.size();
        dword framePc = 0;
        if (innermost)
            framePc = stopPc_ != NO_INDEX ? stopPc_ : pc;
        else if (frames[i + 1].returnAddress >= ISA::INSTRUCTION_SIZE)
            framePc = frames[i + 1].returnAddress - ISA::INSTRUCTION_SIZE;

        const dword instructionIndex = framePc / ISA::INSTRUCTION_SIZE;
        const SourceLocation location = locate(module_, instructionIndex);
        out << "  #" << (frames.size() - 1 - i);
        if (location.function == NO_INDEX)
            out << " fn=?";
        else
        {
            out << " fn=#" << location.function;
            if (!location.functionName.empty())
                out << " '" << location.functionName << '\'';
        }
        out << (innermost ? " pc=" : " call=") << framePc << " (inst #" << instructionIndex << ')';
        if (location.known && !location.sourceFile.empty() && location.line != 0)
            out << " src=" << location.sourceFile << ':' << location.line;
        out << '\n';
    }
}

void DEBUG::Session::printRegisters(const Core &core, dword pc, dword start, dword count,
                                    std::ostream &out) const
{
    (void)pc;
    const std::span<const StackFrame> frames = core.stack();
    if (frames.empty())
    {
        out << "no active frame\n";
        return;
    }
    const std::array<dword, ISA::REGISTER_COUNT> &registers = frames.back().registers;
    bool first = true;
    for (dword i = 0; i < count && start + i < ISA::REGISTER_COUNT; ++i)
    {
        if (!first)
            out << ' ';
        first = false;
        out << 'r' << (start + i) << '=' << hex(registers[start + i], 8);
    }
    out << '\n';
}

void DEBUG::Session::printMemory(dword address, dword length, std::ostream &out) const
{
    if (length == 0 || length > 256)
    {
        out << "error: length must be 1..256\n";
        return;
    }
    if (static_cast<qword>(address) + length > 0x100000000ull)
    {
        out << "error: range outside the 4 GiB memory\n";
        return;
    }

    for (dword offset = 0; offset < length;)
    {
        const dword rowAddress = address + offset;
        const dword remaining = length - offset;
        const dword chunk = remaining < 16 ? remaining : 16;
        std::vector<byte> row(chunk);
        const int result = memory_.reads(std::span<byte>(row.data(), row.size()), rowAddress, chunk);

        out << hex(rowAddress, 8) << "  ";
        if (result != MEMORY::OK)
        {
            out << "<unmapped>\n";
        }
        else
        {
            for (dword i = 0; i < chunk; ++i)
                out << hex(row[i], 2) << ' ';
            for (dword i = chunk; i < 16; ++i)
                out << "   ";
            out << " |";
            for (byte value : row)
                out << (value >= 32 && value < 127 ? static_cast<char>(value) : '.');
            out << "|\n";
        }
        offset += chunk;
    }
}

void DEBUG::Session::printDisassembly(dword pc, dword start, dword count, std::ostream &out) const
{
    (void)pc;
    const dword total = codeCount(module_);
    const dword instruction = start / ISA::INSTRUCTION_SIZE;
    if (instruction >= total)
    {
        out << "error: instruction index outside CODE\n";
        return;
    }
    if (count == 0 || count > 64)
        count = 8;

    const dword currentInstruction =
        stopPc_ != NO_INDEX ? stopPc_ / ISA::INSTRUCTION_SIZE : NO_INDEX;
    for (dword i = 0; i < count && instruction + i < total; ++i)
    {
        const dword index = instruction + i;
        out << (index == currentInstruction ? "> " : "  ") << hex(index * ISA::INSTRUCTION_SIZE, 8)
            << "  " << instructionText(module_, index) << '\n';
    }
}

void DEBUG::Session::printLocals(const Core &core, dword pc, std::ostream &out) const
{
    const std::vector<LocalValue> values = localsAt(module_, core, pc / ISA::INSTRUCTION_SIZE);
    if (values.empty())
    {
        out << "no named locals (debug info absent or outside their range)\n";
        return;
    }
    for (const LocalValue &value : values)
    {
        out << (value.parameter ? "param " : "local ") << value.name << " (r" << value.slot
            << ") = " << hex(value.value, 8) << " (" << std::bit_cast<int>(value.value) << ")\n";
    }
}

void DEBUG::Session::printBreakpoints(std::ostream &out) const
{
    if (breaks_.empty())
    {
        out << "no breakpoints\n";
        return;
    }
    for (const Breakpoint &entry : breaks_)
    {
        const dword instructionIndex = entry.pc / ISA::INSTRUCTION_SIZE;
        const SourceLocation location = locate(module_, instructionIndex);
        out << "  " << entry.id << "  pc=" << entry.pc << " (inst #" << instructionIndex << ')';
        if (location.known && location.function != NO_INDEX)
        {
            out << " fn=#" << location.function;
            if (!location.functionName.empty())
                out << " '" << location.functionName << '\'';
        }
        if (location.known && !location.sourceFile.empty() && location.line != 0)
            out << " src=" << location.sourceFile << ':' << location.line;
        out << '\n';
    }
}
