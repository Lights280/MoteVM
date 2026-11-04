#include "cli.hpp"

#include "core.hpp"
#include "debug.hpp"
#include "define.hpp"
#include "disasm.hpp"
#include "host.hpp"
#include "memory.hpp"
#include "parse.hpp"
#include "workspace.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
    enum Stage
    {
        STAGE_NONE = 0,
        STAGE_LOAD = 1,
        STAGE_INIT = 2,
        STAGE_RUN = 3
    };

    bool isModulePath(const std::string &value)
    {
        const std::string suffix = ".mbc";
        if (value.size() <= suffix.size())
            return false;
        for (std::size_t i = 0; i < suffix.size(); ++i)
        {
            char c = value[value.size() - suffix.size() + i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c != suffix[i])
                return false;
        }
        return true;
    }

    const char *sectionName(dword id)
    {
        switch (id)
        {
        case PARSE::SECTION_META:
            return "META";
        case PARSE::SECTION_CONST:
            return "CONST";
        case PARSE::SECTION_FUNCS:
            return "FUNCS";
        case PARSE::SECTION_CODE:
            return "CODE";
        case PARSE::SECTION_GLOBALS:
            return "GLOBALS";
        case PARSE::SECTION_IMPORTS:
            return "IMPORTS";
        case PARSE::SECTION_DEBUG:
            return "DEBUG";
        default:
            return "UNKNOWN";
        }
    }

    std::string formatFlags(dword flags)
    {
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(2) << std::setfill('0') << flags;
        return out.str();
    }

    std::string sectionLayout(const PARSE::Image &image)
    {
        std::ostringstream out;
        for (std::size_t i = 0; i < image.sections.size(); ++i)
        {
            if (i != 0)
                out << ' ';
            out << sectionName(image.sections[i].id) << '=' << image.sections[i].size;
        }
        return out.str();
    }

    // Shared state for every CLI built-in host function (docs/self/cli.md §10).
    // The transitional value model has no Value/String (ADR-019), so byte strings
    // are passed as (address, length) pairs into the 4 GiB Memory space (ADR-024).
    struct HostState
    {
        Host *host = nullptr;
        Workspace *workspace = nullptr;
        std::ostream *out = nullptr;
        std::ostream *err = nullptr;
        dword module = 0;     // module inspected by const_addr/const_len
        qword lines = 0;      // lines written by print/print_str/eprint_str
        dword heapCursor = 0; // bump allocator cursor (mem_alloc)
        bool heapReady = false;
    };

    // transitional ABI limits (docs/self/cli.md §10)
    constexpr dword MAX_HOST_PATH_BYTES = 4096;
    constexpr dword MAX_HOST_IO_BYTES = 16u << 20; // = PARSE::MAX_STRING_BYTES
    constexpr dword MAX_HOST_HEAP_BYTES = 256u << 20;
    constexpr qword HOST_ADDRESS_LIMIT = 0x100000000ull; // 4 GiB linear memory

    // negative results reuse the error codes of docs/05 §8.7
    constexpr int HOST_SYSTEM_ERROR = -3; // open/read/write/remove failure
    constexpr int HOST_DENIED = -4;       // outside the workspace / not granted
    constexpr int HOST_BAD_ARGUMENT = -5; // bad range, size or path bytes

    dword hostError(int code)
    {
        return static_cast<dword>(code);
    }

    bool memoryRead(HostState &state, dword address, dword size, std::string &out)
    {
        if (state.host == nullptr || !state.host->hasMemory())
            return false;
        if (size == 0)
        {
            out.clear();
            return true;
        }
        std::string buffer(size, '\0');
        const int result = state.host->memory().reads(
            std::span<byte>(reinterpret_cast<byte *>(buffer.data()), buffer.size()), address, size);
        if (result != MEMORY::OK)
            return false;
        out = std::move(buffer);
        return true;
    }

    bool memoryWrite(HostState &state, dword address, std::string_view data)
    {
        if (state.host == nullptr || !state.host->hasMemory())
            return false;
        if (data.empty())
            return true;
        const int result = state.host->memory().writes(
            std::span<const byte>(reinterpret_cast<const byte *>(data.data()), data.size()),
            address, static_cast<dword>(data.size()));
        return result == MEMORY::OK;
    }

    bool pathFromMemory(HostState &state, dword address, dword length, std::string &path)
    {
        if (length == 0 || length > MAX_HOST_PATH_BYTES)
            return false;
        std::string bytes;
        if (!memoryRead(state, address, length, bytes))
            return false;
        if (bytes.find('\0') != std::string::npos)
            return false;
        path = std::move(bytes);
        return true;
    }

    int mapWorkspaceCode(int code)
    {
        switch (code)
        {
        case WORKSPACE::OK:
            return 0;
        case WORKSPACE::BAD_PATH:
            return HOST_BAD_ARGUMENT;
        case WORKSPACE::OUTSIDE:
        case WORKSPACE::NO_ROOT:
            return HOST_DENIED;
        default: // BAD_ROOT / NOT_DIRECTORY / NO_PARENT
            return HOST_SYSTEM_ERROR;
        }
    }

    // CLI built-in "print" (docs/05 §8.6). The transitional value model keeps raw
    // 32-bit registers (ADR-019), so the argument is printed as a signed 32-bit
    // integer; full TOSTR rendering of Value arrives with M3.
    dword printFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->out == nullptr || args == nullptr || arity != 1)
            return 0;
        *state->out << std::bit_cast<int>(args[0]) << '\n';
        state->out->flush(); // line buffered (docs/05 §8.5)
        ++state->lines;
        return 0;
    }

    // print_str(addr, len): raw bytes + '\n' (docs/self/cli.md §10).
    dword printStrFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->out == nullptr || args == nullptr || arity != 2)
            return hostError(HOST_BAD_ARGUMENT);
        try
        {
            std::string text;
            if (!memoryRead(*state, args[0], args[1], text))
                return hostError(HOST_BAD_ARGUMENT);
            if (!text.empty())
                state->out->write(text.data(), static_cast<std::streamsize>(text.size()));
            *state->out << '\n';
            state->out->flush();
            ++state->lines;
            return 0;
        }
        catch (...)
        {
            return hostError(HOST_SYSTEM_ERROR);
        }
    }

    dword eprintStrFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->err == nullptr || args == nullptr || arity != 2)
            return hostError(HOST_BAD_ARGUMENT);
        try
        {
            std::string text;
            if (!memoryRead(*state, args[0], args[1], text))
                return hostError(HOST_BAD_ARGUMENT);
            if (!text.empty())
                state->err->write(text.data(), static_cast<std::streamsize>(text.size()));
            *state->err << '\n';
            state->err->flush();
            ++state->lines;
            return 0;
        }
        catch (...)
        {
            return hostError(HOST_SYSTEM_ERROR);
        }
    }

    dword totalPrintsFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        (void)args;
        if (state == nullptr || arity != 0)
            return 0;
        return static_cast<dword>(state->lines);
    }

    // const_addr(index) / const_len(index): address and size of the current
    // module's constant payload in Memory (ADR-024).
    dword constAddrFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->host == nullptr || args == nullptr || arity != 1)
            return NO_INDEX;
        return state->host->constantAddress(state->module, args[0]);
    }

    dword constLenFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->host == nullptr || args == nullptr || arity != 1)
            return NO_INDEX;
        if (state->module >= state->host->moduleCount())
            return NO_INDEX;
        const PARSE::Image &image = state->host->module(state->module).image();
        if (args[0] >= image.constants.size())
            return NO_INDEX;
        return static_cast<dword>(image.constants[args[0]].payload.size());
    }

    // mem_alloc(size): 8-byte aligned bump allocation over the Memory blocks that
    // follow the constant pool. mem_free is a no-op; everything is released at exit.
    dword memAllocFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->host == nullptr || !state->host->hasMemory() ||
            args == nullptr || arity != 1)
            return NO_INDEX;

        const dword size = args[0];
        if (size == 0 || size > MAX_HOST_HEAP_BYTES)
            return NO_INDEX;

        if (!state->heapReady)
        {
            state->heapCursor = static_cast<dword>(align8(state->host->stats().constantBytes));
            state->heapReady = true;
        }

        const qword start = state->heapCursor;
        const qword end = start + size;
        if (end > HOST_ADDRESS_LIMIT)
            return NO_INDEX;

        const dword firstBlock = static_cast<dword>(start / MEMORY::BLOCK_SIZE);
        const dword lastBlock = static_cast<dword>((end - 1) / MEMORY::BLOCK_SIZE);
        for (dword block = firstBlock; block <= lastBlock; ++block)
            if (state->host->memory().allocate(static_cast<int>(block)) != MEMORY::OK)
                return NO_INDEX;

        state->heapCursor = static_cast<dword>(align8(end));
        return static_cast<dword>(start);
    }

    dword memFreeFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || args == nullptr || arity != 1 || args[0] == NO_INDEX)
            return hostError(HOST_BAD_ARGUMENT);
        return 0; // bump allocator: no per-call release
    }

    // ---------------------------------------------------------------------
    // file.* (registered only when -w grants a workspace, docs/self/cli.md §10)
    // ---------------------------------------------------------------------

    dword fileReadFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->workspace == nullptr || state->host == nullptr ||
            args == nullptr || arity != 4)
            return hostError(HOST_BAD_ARGUMENT);

        try
        {
            if (args[3] > MAX_HOST_IO_BYTES)
                return hostError(HOST_BAD_ARGUMENT);

            std::string path;
            if (!pathFromMemory(*state, args[0], args[1], path))
                return hostError(HOST_BAD_ARGUMENT);

            std::filesystem::path resolved;
            const int mapped = mapWorkspaceCode(state->workspace->resolveRead(path, resolved));
            if (mapped != 0)
                return hostError(mapped);

            std::ifstream input(resolved, std::ios::binary);
            if (!input)
                return hostError(HOST_SYSTEM_ERROR);

            std::string data(args[3], '\0');
            if (args[3] != 0)
                input.read(data.data(), static_cast<std::streamsize>(args[3]));
            const std::streamsize got = input.gcount();
            if (input.bad())
                return hostError(HOST_SYSTEM_ERROR);

            data.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
            if (!memoryWrite(*state, args[2], data))
                return hostError(HOST_BAD_ARGUMENT);
            return static_cast<dword>(data.size());
        }
        catch (...)
        {
            return hostError(HOST_SYSTEM_ERROR);
        }
    }

    dword fileWriteCommon(HostState &state, const dword *args, bool append)
    {
        try
        {
            if (args[3] > MAX_HOST_IO_BYTES)
                return hostError(HOST_BAD_ARGUMENT);

            std::string path;
            if (!pathFromMemory(state, args[0], args[1], path))
                return hostError(HOST_BAD_ARGUMENT);

            std::string data;
            if (!memoryRead(state, args[2], args[3], data))
                return hostError(HOST_BAD_ARGUMENT);

            std::filesystem::path resolved;
            const int mapped = mapWorkspaceCode(state.workspace->resolveWrite(path, resolved));
            if (mapped != 0)
                return hostError(mapped);

            const std::ios::openmode mode =
                std::ios::binary | (append ? std::ios::app : std::ios::trunc);
            std::ofstream output(resolved, mode);
            if (!output)
                return hostError(HOST_SYSTEM_ERROR);
            if (!data.empty())
                output.write(data.data(), static_cast<std::streamsize>(data.size()));
            output.flush();
            if (!output)
                return hostError(HOST_SYSTEM_ERROR);
            return static_cast<dword>(data.size());
        }
        catch (...)
        {
            return hostError(HOST_SYSTEM_ERROR);
        }
    }

    dword fileWriteFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->workspace == nullptr || args == nullptr || arity != 4)
            return hostError(HOST_BAD_ARGUMENT);
        return fileWriteCommon(*state, args, false);
    }

    dword fileAppendFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->workspace == nullptr || args == nullptr || arity != 4)
            return hostError(HOST_BAD_ARGUMENT);
        return fileWriteCommon(*state, args, true);
    }

    dword fileExistsFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->workspace == nullptr || args == nullptr || arity != 2)
            return hostError(HOST_BAD_ARGUMENT);
        try
        {
            std::string path;
            if (!pathFromMemory(*state, args[0], args[1], path))
                return hostError(HOST_BAD_ARGUMENT);

            std::filesystem::path resolved;
            const int mapped = mapWorkspaceCode(state->workspace->resolveRead(path, resolved));
            if (mapped != 0)
                return hostError(mapped);

            std::error_code error;
            const bool exists = std::filesystem::exists(resolved, error);
            if (error)
                return hostError(HOST_SYSTEM_ERROR);
            return exists ? 1 : 0;
        }
        catch (...)
        {
            return hostError(HOST_SYSTEM_ERROR);
        }
    }

    dword fileSizeFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->workspace == nullptr || args == nullptr || arity != 2)
            return hostError(HOST_BAD_ARGUMENT);
        try
        {
            std::string path;
            if (!pathFromMemory(*state, args[0], args[1], path))
                return hostError(HOST_BAD_ARGUMENT);

            std::filesystem::path resolved;
            const int mapped = mapWorkspaceCode(state->workspace->resolveRead(path, resolved));
            if (mapped != 0)
                return hostError(mapped);

            std::error_code error;
            if (!std::filesystem::is_regular_file(resolved, error) || error)
                return hostError(HOST_SYSTEM_ERROR);
            const std::uintmax_t size = std::filesystem::file_size(resolved, error);
            if (error || size > 0xFFFFFFFFull)
                return hostError(HOST_SYSTEM_ERROR);
            return static_cast<dword>(size);
        }
        catch (...)
        {
            return hostError(HOST_SYSTEM_ERROR);
        }
    }

    dword fileRemoveFunction(void *user, const dword *args, int arity)
    {
        HostState *state = static_cast<HostState *>(user);
        if (state == nullptr || state->workspace == nullptr || args == nullptr || arity != 2)
            return hostError(HOST_BAD_ARGUMENT);
        try
        {
            std::string path;
            if (!pathFromMemory(*state, args[0], args[1], path))
                return hostError(HOST_BAD_ARGUMENT);

            std::filesystem::path resolved;
            const int mapped = mapWorkspaceCode(state->workspace->resolveWrite(path, resolved));
            if (mapped != 0)
                return hostError(mapped);

            std::error_code error;
            if (!std::filesystem::is_regular_file(resolved, error) || error)
                return hostError(HOST_SYSTEM_ERROR);
            std::filesystem::remove(resolved, error);
            if (error)
                return hostError(HOST_SYSTEM_ERROR);
            return 0;
        }
        catch (...)
        {
            return hostError(HOST_SYSTEM_ERROR);
        }
    }

    struct Registration
    {
        const char *name;
        int arity;
        HOST::Function function;
    };

    // Always available: output, constant addressing and the memory bump allocator.
    constexpr Registration CORE_FUNCTIONS[] = {
        {"print", 1, &printFunction},
        {"print_str", 2, &printStrFunction},
        {"eprint_str", 2, &eprintStrFunction},
        {"total_prints", 0, &totalPrintsFunction},
        {"const_addr", 1, &constAddrFunction},
        {"const_len", 1, &constLenFunction},
        {"mem_alloc", 1, &memAllocFunction},
        {"mem_free", 1, &memFreeFunction},
    };

    // Capability-gated: only registered when -w grants a workspace root, so a
    // module that imports file.* fails to load without -w (UNLINKED_IMPORT).
    constexpr Registration FILE_FUNCTIONS[] = {
        {"file.read", 4, &fileReadFunction},
        {"file.write", 4, &fileWriteFunction},
        {"file.append", 4, &fileAppendFunction},
        {"file.exists", 2, &fileExistsFunction},
        {"file.size", 2, &fileSizeFunction},
        {"file.remove", 2, &fileRemoveFunction},
    };

    template <std::size_t N>
    bool registerFunctions(Host &host, void *user, const Registration (&table)[N])
    {
        for (const Registration &entry : table)
            if (host.registerFunction(entry.name, entry.arity, entry.function, user) != HOST::OK)
                return false;
        return true;
    }
}

namespace
{
    const char *hostErrorText(int code)
    {
        switch (code)
        {
        case HOST::UNLINKED_IMPORT:
            return "import not linked (no host function with matching name and arity)";
        case HOST::BAD_ARGUMENT:
            return "invalid host function registration";
        case HOST::NO_MODULE:
            return "host has no module";
        case HOST::NO_MEMORY:
            return "host has no memory";
        default:
            return "unknown host error";
        }
    }

    const char *memoryErrorText(int code)
    {
        switch (code)
        {
        case MEMORY::BLOCK_OCCUPIED:
            return "block already occupied";
        case MEMORY::MEMORY_UNAVAILABLE:
            return "block unavailable";
        case MEMORY::MEMORY_OVERFLOW:
            return "constant pool exceeds the 4 GiB address space";
        default:
            return "unknown memory error";
        }
    }

    const char *workspaceErrorText(int code)
    {
        switch (code)
        {
        case WORKSPACE::NO_ROOT:
            return "workspace is not configured";
        case WORKSPACE::BAD_ROOT:
            return "no such directory";
        case WORKSPACE::NOT_DIRECTORY:
            return "not a directory";
        case WORKSPACE::BAD_PATH:
            return "invalid path";
        case WORKSPACE::OUTSIDE:
            return "outside workspace";
        case WORKSPACE::NO_PARENT:
            return "parent directory is missing or outside workspace";
        default:
            return "unknown workspace error";
        }
    }

    const char *coreErrorText(int code)
    {
        switch (code)
        {
        case CORE::UNKNOWN_INSTRUCTION:
            return "unknown instruction";
        case CORE::DIVIDED_BY_ZERO:
            return "integer division by zero";
        case CORE::INTEGER_OVERFLOW:
            return "integer overflow";
        case CORE::STACK_OVERFLOW:
            return "call stack overflow";
        case CORE::INVALID_TARGET:
            return "invalid jump or entry target";
        case CORE::INVALID_TEXT:
            return "invalid code text";
        case CORE::NO_ACTIVE_FRAME:
            return "no active frame";
        case CORE::NO_CALLER:
            return "return without caller";
        case CORE::NUMERIC_CONVERSION:
            return "numeric conversion failed";
        case CORE::INVALID_FUNCTION:
            return "function index out of range";
        case CORE::INVALID_IMPORT:
            return "import not bound";
        case CORE::FUEL_EXHAUSTED:
            return "fuel exhausted";
        case CORE::INTERRUPTED:
            return "interrupted";
        case CORE::TRAP:
            return "trap raised";
        case CORE::MEMORY_FAULT:
            return "memory access fault";
        case CORE::DEBUG_BREAK:
            return "debugger stop";
        default:
            return "unknown runtime error";
        }
    }
}

namespace
{
    const char *resultName(int code)
    {
        switch (code)
        {
        case CORE::OK:
            return "OK";
        case CORE::UNKNOWN_INSTRUCTION:
            return "UNKNOWN_INSTRUCTION";
        case CORE::DIVIDED_BY_ZERO:
            return "DIVIDED_BY_ZERO";
        case CORE::INTEGER_OVERFLOW:
            return "INTEGER_OVERFLOW";
        case CORE::STACK_OVERFLOW:
            return "STACK_OVERFLOW";
        case CORE::INVALID_TARGET:
            return "INVALID_TARGET";
        case CORE::INVALID_TEXT:
            return "INVALID_TEXT";
        case CORE::NO_ACTIVE_FRAME:
            return "NO_ACTIVE_FRAME";
        case CORE::NO_CALLER:
            return "NO_CALLER";
        case CORE::NUMERIC_CONVERSION:
            return "NUMERIC_CONVERSION";
        case CORE::INVALID_FUNCTION:
            return "INVALID_FUNCTION";
        case CORE::INVALID_IMPORT:
            return "INVALID_IMPORT";
        case CORE::FUEL_EXHAUSTED:
            return "FUEL_EXHAUSTED";
        case CORE::INTERRUPTED:
            return "INTERRUPTED";
        case CORE::TRAP:
            return "TRAP";
        case CORE::MEMORY_FAULT:
            return "MEMORY_FAULT";
        case CORE::DEBUG_BREAK:
            return "DEBUG_STOP";
        case HOST::NO_MODULE:
            return "NO_MODULE";
        case HOST::NO_MEMORY:
            return "NO_MEMORY";
        default:
            return "ERROR";
        }
    }

    int exitCodeOf(Stage stage, int code)
    {
        if (code == HOST::OK)
            return 0;
        if (stage == STAGE_LOAD)
            return code == HOST::NO_MODULE ? 4 : 2;
        if (stage == STAGE_INIT)
            return 4;
        if (code == HOST::NO_MODULE || code == HOST::NO_MEMORY)
            return 4;
        return 3;
    }
}

namespace
{
    bool consoleInput(void)
    {
#if defined(_WIN32)
        return _isatty(_fileno(stdin)) != 0;
#else
        return isatty(fileno(stdin)) != 0;
#endif
    }

    std::string hexWord(dword value)
    {
        std::ostringstream out;
        out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
        return out.str();
    }

    std::string locationText(const DEBUG::SourceLocation &location)
    {
        std::ostringstream out;
        out << "fn=#" << location.function;
        if (!location.functionName.empty())
            out << " '" << location.functionName << "'";
        if (!location.sourceFile.empty() && location.line != 0)
            out << " src=" << location.sourceFile << ':' << location.line << ':' << location.column;
        return out.str();
    }

    // -d post-mortem context. Uses Core::errorPc(), which is recorded on the error
    // path only, so -d never has to install a debug hook for ordinary runs.
    void printStopNotes(const Host &host, int failure, std::ostream &err)
    {
        const Core *core = host.core();
        if (core == nullptr || host.moduleCount() == 0)
            return;
        const Parse &module = host.module(0);
        const dword pc = core->errorPc();
        const dword instruction = pc / ISA::INSTRUCTION_SIZE;

        err << "note: stop: " << resultName(failure) << " at pc=" << pc << " (inst #" << instruction
            << ")\n";
        if (failure == CORE::TRAP)
            err << "note: trap code " << core->trapCode() << "\n";

        const DEBUG::SourceLocation location = DEBUG::locate(module, instruction);
        if (location.known)
            err << "note: location: " << locationText(location) << "\n";

        const std::span<const StackFrame> frames = core->stack();
        if (!frames.empty())
        {
            err << "note: stack (innermost first):\n";
            for (std::size_t i = frames.size(); i-- > 0;)
            {
                const bool innermost = i + 1 == frames.size();
                dword framePc = 0;
                if (innermost)
                    framePc = pc;
                else if (frames[i + 1].returnAddress >= ISA::INSTRUCTION_SIZE)
                    framePc = frames[i + 1].returnAddress - ISA::INSTRUCTION_SIZE;

                const dword frameInstruction = framePc / ISA::INSTRUCTION_SIZE;
                const DEBUG::SourceLocation frameLocation = DEBUG::locate(module, frameInstruction);
                err << "note:   #" << (frames.size() - 1 - i) << " pc=" << framePc << " (inst #"
                    << frameInstruction << ')';
                if (frameLocation.known)
                    err << ' ' << locationText(frameLocation);
                err << "\n";
            }

            if (location.known && location.function != NO_INDEX)
            {
                const dword count =
                    std::min<dword>(module.image().functions[location.function].regcnt, 16);
                err << "note: registers:";
                for (dword i = 0; i < count; ++i)
                    err << " r" << i << '=' << hexWord(frames.back().registers[i]);
                err << "\n";
            }
        }

        const dword total = DEBUG::codeCount(module);
        if (total != 0)
        {
            const dword fault = instruction < total ? instruction : total - 1;
            const dword first = fault > 2 ? fault - 2 : 0;
            const dword last = std::min<dword>(fault + 2, total - 1);
            err << "note: code:\n";
            for (dword index = first; index <= last; ++index)
            {
                std::ostringstream line;
                line << "note:   " << (index == fault ? "> " : "  ") << std::setw(6)
                     << (index * ISA::INSTRUCTION_SIZE) << "  "
                     << DEBUG::instructionText(module, index);
                err << line.str() << "\n";
            }
        }
    }

    void printModuleNotes(const Host &host, const CLI::Options &options, std::ostream &err)
    {
        for (std::size_t i = 0; i < host.moduleCount(); ++i)
        {
            const Parse &module = host.module(i);
            const PARSE::Image &image = module.image();
            err << "note: module[" << i << "] '" << host.path(i) << "': MOTEBC "
                << image.header.majorVer << '.' << image.header.minorVer
                << ", flags=" << formatFlags(image.header.flags)
                << ", entry=#" << image.header.entry
                << ", debug=" << (module.hasDebug() ? "yes" : "no") << "\n";
            err << "note:   sections " << sectionLayout(image) << "\n";
            err << "note:   counts const=" << image.constants.size()
                << " func=" << image.functions.size()
                << " global=" << image.globals.size()
                << " import=" << image.imports.size()
                << " instr=" << image.codeCount << "\n";
        }
        err << "note: constants placed in " << host.stats().constantBytes << " byte(s) of memory\n";
        if (!options.intern.empty())
            err << "note: " << options.intern.size() << " intern parameter(s) ignored\n";
    }

    void printStatus(const Host &host, bool initialized, int runCode, int exitCode, std::ostream &err)
    {
        err << "# motevm status v1\n";
        for (std::size_t i = 0; i < host.moduleCount(); ++i)
        {
            const Parse &module = host.module(i);
            const PARSE::Image &image = module.image();
            err << "module[" << i << "].file=" << host.path(i) << "\n";
            err << "module[" << i << "].format=MOTEBC " << image.header.majorVer << '.'
                << image.header.minorVer << "\n";
            err << "module[" << i << "].flags=" << formatFlags(image.header.flags) << "\n";
            err << "module[" << i << "].entry=#" << image.header.entry << "\n";
            err << "module[" << i << "].file_size=" << image.header.fileSize << "\n";
            err << "module[" << i << "].sections=" << sectionLayout(image) << "\n";
            err << "module[" << i << "].counts=const=" << image.constants.size()
                << " func=" << image.functions.size()
                << " global=" << image.globals.size()
                << " import=" << image.imports.size()
                << " instr=" << image.codeCount << "\n";
        }

        if (initialized)
        {
            const std::size_t blocks = host.memory().allocatedBlocks();
            err << "memory.allocated_blocks=" << blocks << "\n";
            err << "memory.allocated_bytes=" << blocks * MEMORY::BLOCK_SIZE << "\n";
        }

        if (initialized && host.stats().runs != 0)
        {
            const HOST::Stats &stats = host.stats();
            err << "execution.runs=" << stats.runs << "\n";
            err << "execution.instructions=" << stats.instructions << "\n";
            err << "execution.max_depth=" << stats.maxDepth << "\n";
            err << "execution.pc=" << stats.lastPc << "\n";
            err << "execution.result=" << resultName(runCode) << "\n";
            if (runCode == CORE::TRAP)
                err << "execution.trap_code=" << stats.trapCode << "\n";
        }
        else
        {
            err << "execution.result=NOT_RUN\n";
        }
        err << "status.exit=" << exitCode << "\n";
    }
}

std::vector<std::string> CLI::Options::files(void) const
{
    std::vector<std::string> all;
    if (!entry.empty())
        all.push_back(entry);
    all.insert(all.end(), modules.begin(), modules.end());
    return all;
}

namespace
{
    bool traceLevelValue(const std::string &value, int &level)
    {
        if (value == "off")
            level = 0;
        else if (value == "call")
            level = 1;
        else if (value == "exec")
            level = 2;
        else if (value == "full")
            level = 3;
        else
            return false;
        return true;
    }

    bool dwordValue(const std::string &text, dword &value)
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
}

int CLI::parse(int argc, char *argv[], Options &options)
{
    int index = 1;

    // 1) extern parameters (must come before the entry module)
    while (index < argc)
    {
        const std::string arg = argv[index];
        if (arg == "--")
        {
            ++index;
            break;
        }
        if (arg.empty() || arg[0] != '-')
            break;
        if (arg == "-v" || arg == "--version")
        {
            options.version = true;
            return OK;
        }
        if (arg == "-h" || arg == "--help")
        {
            options.help = true;
            return OK;
        }
        if (arg == "-d" || arg == "--debug")
            options.debug = true;
        else if (arg == "-r" || arg == "--release")
            options.debug = false;
        else if (arg == "-s" || arg == "--status")
            options.status = true;
        else if (arg == "-i" || arg == "--interactive")
            options.interactive = true;
        else if (arg == "--step")
            options.step = true;
        else if (arg.rfind("--trace=", 0) == 0)
        {
            if (!traceLevelValue(arg.substr(8), options.trace))
                return USAGE;
        }
        else if (arg == "--trace")
        {
            if (index + 1 >= argc || !traceLevelValue(argv[index + 1], options.trace))
                return USAGE;
            ++index;
        }
        else if (arg.rfind("--trace-file=", 0) == 0)
        {
            if (arg.size() == 13)
                return USAGE;
            options.traceFile = arg.substr(13);
        }
        else if (arg == "--trace-file")
        {
            if (index + 1 >= argc || argv[index + 1] == nullptr || argv[index + 1][0] == '\0')
                return USAGE;
            options.traceFile = argv[++index];
        }
        else if (arg.rfind("--trace-limit=", 0) == 0)
        {
            if (!dwordValue(arg.substr(14), options.traceLimit))
                return USAGE;
        }
        else if (arg == "--trace-limit")
        {
            if (index + 1 >= argc || !dwordValue(argv[index + 1], options.traceLimit))
                return USAGE;
            ++index;
        }
        else if (arg.rfind("--break=", 0) == 0)
        {
            if (arg.size() == 8)
                return USAGE;
            options.breakpoints.emplace_back(arg.substr(8));
        }
        else if (arg == "--break")
        {
            if (index + 1 >= argc || argv[index + 1] == nullptr || argv[index + 1][0] == '\0')
                return USAGE;
            options.breakpoints.emplace_back(argv[++index]);
        }
        else if (arg == "-w" || arg == "--workspace")
        {
            if (index + 1 >= argc || argv[index + 1] == nullptr || argv[index + 1][0] == '\0')
                return USAGE;
            options.workspace = argv[++index]; // the value is taken verbatim
        }
        else if (arg.rfind("--workspace=", 0) == 0)
        {
            if (arg.size() == 12)
                return USAGE;
            options.workspace = arg.substr(12);
        }
        else
            return USAGE;
        ++index;
    }

    // 2) entry module
    if (index >= argc)
        return USAGE;
    options.entry = argv[index++];

    // 3) external modules: tokens ending with .mbc
    while (index < argc && isModulePath(argv[index]))
        options.modules.emplace_back(argv[index++]);

    // 4) intern parameters: reserved for scripts, kept verbatim
    while (index < argc)
        options.intern.emplace_back(argv[index++]);

    return OK;
}

void CLI::printVersion(std::ostream &out)
{
    out << "motevm " << MOTEVM_VERSION << " (MOTEBC " << MOTEBC_VERSION << ")\n";
}

void CLI::printUsage(std::ostream &out)
{
    out << "motevm " << MOTEVM_VERSION << " (MOTEBC " << MOTEBC_VERSION << ")\n\n";
    out << "usage:\n";
    out << "  motevm [extern_parameters] <entry_mbc_module> [ext_mbc_modules] "
           "[intern_parameters]\n\n";
    out << "extern_parameters:\n";
    out << "  -v, --version    print version and exit\n";
    out << "  -h, --help       print this help and exit\n";
    out << "  -d, --debug      debug mode: verbose diagnostics, stop context and trace\n";
    out << "  -r, --release    release mode (default): errors only\n";
    out << "  -s, --status     print execution statistics after the run\n";
    out << "  -i, --interactive  stop at entry and read debugger commands from stdin\n";
    out << "      --step       stop before the first instruction (implies --interactive)\n";
    out << "      --break SPEC set a breakpoint (repeatable): #index, offset, function[:offset]\n";
    out << "      --trace LVL  instruction trace: off|call|exec|full (default: off)\n";
    out << "      --trace-file PATH  trace target (default: stderr, '-' = stdout)\n";
    out << "      --trace-limit N    stop recording after N trace lines (0 = unlimited)\n";
    out << "  -w, --workspace DIR\n";
    out << "                   grant read/write access inside DIR only (default: stdio only)\n\n";
    out << "notes:\n";
    out << "  - extern parameters must appear before the entry module\n";
    out << "  - external modules are tokens ending with .mbc; the rest are intern parameters\n";
    out << "  - intern parameters are reserved for scripts and currently ignored\n";
    out << "  - with -w, relative module paths resolve inside DIR and absolute paths must stay inside\n";
    out << "  - without -w no directory is granted: modules load as given, scripts get stdio only\n";
    out << "  - the debugger REPL reads stdin and writes prompts/notes to stderr\n";
    out << "  - with -w a --trace-file path must stay inside DIR\n\n";
    out << "exit codes:\n";
    out << "  0 success   1 usage   2 input/verify failure   3 runtime trap   4 internal error\n";
}

int CLI::run(const Options &options, std::ostream &out, std::ostream &err)
{
    Host host;

    HostState hostState;
    hostState.host = &host;
    hostState.out = &out;
    hostState.err = &err;

    Stage failedAt = STAGE_NONE;
    std::string failedFile;
    int failure = HOST::OK;
    int runCode = CORE::OK;
    bool initialized = false;

    const std::vector<std::string> files = options.files();
    if (files.empty())
        return USAGE;

    // A workspace is the only directory grant: without -w the sandbox is unset,
    // so module files load exactly as given and scripts get stdio only. With -w
    // every path is canonicalized and must stay inside the granted root.
    Workspace workspace;
    int workspaceCode = WORKSPACE::OK;
    bool workspaceDenied = false;

    if (!options.workspace.empty())
    {
        workspaceCode = workspace.setRoot(options.workspace);
        if (workspaceCode != WORKSPACE::OK)
        {
            err << "error: workspace '" << options.workspace << "': "
                << workspaceErrorText(workspaceCode) << "\n";
            if (options.status)
                printStatus(host, initialized, runCode, 2, err);
            return 2;
        }
        if (options.debug)
            err << "note: workspace '" << workspace.rootText() << "'\n";
    }
    else if (options.debug)
        err << "note: no workspace (stdio only)\n";

    hostState.workspace = &workspace;
    if (!registerFunctions(host, &hostState, CORE_FUNCTIONS))
    {
        err << "internal error: cannot register the built-in host functions\n";
        return 4;
    }
    if (workspace.hasRoot() && !registerFunctions(host, &hostState, FILE_FUNCTIONS))
    {
        err << "internal error: cannot register the 'file.*' host functions\n";
        return 4;
    }

    for (const std::string &file : files)
    {
        std::filesystem::path resolved(file);
        if (workspace.hasRoot())
        {
            workspaceCode = workspace.resolveRead(file, resolved);
            if (workspaceCode != WORKSPACE::OK)
            {
                workspaceDenied = true;
                failedAt = STAGE_LOAD;
                failedFile = file;
                failure = workspaceCode; // non-zero => exitCodeOf(STAGE_LOAD) == 2
                break;
            }
        }
        failure = host.load(resolved.string(), file);
        if (failure != HOST::OK)
        {
            failedAt = STAGE_LOAD;
            failedFile = file;
            break;
        }
    }

    if (failure == HOST::OK)
    {
        failure = host.initMemory();
        if (failure != HOST::OK)
            failedAt = STAGE_INIT;
        else
        {
            initialized = true;
            hostState.heapCursor = static_cast<dword>(align8(host.stats().constantBytes));
            hostState.heapReady = true;
        }
    }

    std::unique_ptr<DEBUG::Session> session;
    std::ofstream traceFile;
    std::ostream *traceStream = &err;
    bool debuggerQuit = false;
    bool debuggerAbort = false;

    if (failure == HOST::OK)
    {
        if (options.debug)
            printModuleNotes(host, options, err);

        if (options.debugging() && host.moduleCount() != 0)
        {
            session = std::make_unique<DEBUG::Session>(host.module(0), host.memory(), std::cin, err);
            const bool repl = options.interactive || options.step || !options.breakpoints.empty();
            session->setInteractive(repl);
            session->setPrompt(repl && consoleInput());
            session->setStartStopped(options.step || options.interactive);

            if (options.tracing())
            {
                if (!options.traceFile.empty())
                {
                    if (options.traceFile == "-")
                        traceStream = &out;
                    else
                    {
                        std::filesystem::path resolved(options.traceFile);
                        if (workspace.hasRoot())
                        {
                            const int code = workspace.resolveWrite(options.traceFile, resolved);
                            if (code != WORKSPACE::OK)
                            {
                                err << "error: trace file '" << options.traceFile << "': "
                                    << workspaceErrorText(code) << "\n";
                                if (options.status)
                                    printStatus(host, initialized, runCode, 2, err);
                                return 2;
                            }
                        }
                        traceFile.open(resolved,
                                       std::ios::out | std::ios::binary | std::ios::trunc);
                        if (!traceFile)
                        {
                            err << "error: cannot open trace file '" << options.traceFile << "'\n";
                            if (options.status)
                                printStatus(host, initialized, runCode, 2, err);
                            return 2;
                        }
                        traceStream = &traceFile;
                    }
                }
                session->setTrace(static_cast<DEBUG::TraceLevel>(options.trace), *traceStream,
                                  options.traceLimit);
            }

            std::string breakpointError;
            for (const std::string &spec : options.breakpoints)
            {
                if (!session->addBreakpoint(spec, breakpointError))
                {
                    err << "error: breakpoint '" << spec << "': " << breakpointError << "\n";
                    if (options.status)
                        printStatus(host, initialized, runCode, 2, err);
                    return 2;
                }
            }

            failure = host.runModule(0, &DEBUG::Session::hook, session.get());
        }
        else
            failure = host.runModule(0);

        runCode = failure;
        if (failure != HOST::OK)
            failedAt = STAGE_RUN;

        debuggerQuit = failure == CORE::DEBUG_BREAK && session && !session->abortRequested();
        debuggerAbort = failure == CORE::DEBUG_BREAK && session && session->abortRequested();

        if (session && host.core() != nullptr)
            session->finishTrace(*host.core(), failure);
    }

    if (options.debug && failedAt == STAGE_RUN)
        printStopNotes(host, failure, err);

    if (options.debug && initialized && host.stats().runs != 0)
        err << "note: executed " << host.stats().instructions << " instruction(s), peak depth "
            << host.stats().maxDepth << ", pc=" << host.stats().lastPc << "\n";

    int exitCode = exitCodeOf(failedAt, failure);
    if (debuggerQuit)
        exitCode = 0;

    if (failedAt == STAGE_LOAD)
    {
        if (workspaceDenied)
            err << "error: '" << failedFile << "': " << workspaceErrorText(workspaceCode) << "\n";
        else if (failure == HOST::UNLINKED_IMPORT)
            err << "error: '" << failedFile << "': import #" << host.unlinkedImportIndex()
                << " '" << host.unlinkedImportName() << "'/" << host.unlinkedImportArity()
                << ": " << hostErrorText(failure) << "\n";
        else if (failure >= HOST::NO_MODULE)
            err << "error: '" << failedFile << "': " << hostErrorText(failure) << "\n";
        else
            err << "error: '" << failedFile << "': " << PARSE::errorText(failure) << "\n";
    }
    else if (failedAt == STAGE_INIT)
        err << "error: memory: " << memoryErrorText(failure) << "\n";
    else if (failedAt == STAGE_RUN)
    {
        const HOST::Stats &stats = host.stats();
        if (debuggerQuit)
        {
            // The user quit at a breakpoint: no error line, exit code 0.
        }
        else if (debuggerAbort)
            err << "error: aborted by debugger at pc=" << stats.lastPc << "\n";
        else if (failure == CORE::TRAP)
            err << "error: trap " << stats.trapCode << " (TRAP) at pc=" << stats.lastPc << "\n";
        else if (failure == HOST::NO_MODULE)
            err << "error: host has no module\n";
        else if (failure == HOST::NO_MEMORY)
            err << "error: host has no memory\n";
        else
            err << "error: " << coreErrorText(failure) << " at pc=" << stats.lastPc << "\n";
    }

    if (options.status)
        printStatus(host, initialized, runCode, exitCode, err);

    return exitCode;
}
