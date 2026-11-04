#include "host.hpp"

#include <string_view>
#include <utility>

int Host::load(const std::string &path)
{
    return load(path, path);
}

int Host::load(const std::string &path, const std::string &displayPath)
{
    Parse parser;
    int result = parser.load(path);
    if (result == PARSE::OK)
        result = parser.parse();
    if (result != PARSE::OK)
    {
        lastError = result;
        hostStats.result = result;
        return result;
    }
    std::vector<CORE::HostSlot> slots;
    const int bound = bindImports(parser, slots);
    if (bound != HOST::OK)
    {
        lastError = bound;
        hostStats.result = bound;
        return bound;
    }

    modules.push_back(std::move(parser));
    modulePaths.push_back(displayPath);
    imports.push_back(std::move(slots));
    ++hostStats.modules;
    hostStats.constants += modules.back().image().constants.size();
    hostStats.result = HOST::OK;
    lastError = HOST::OK;
    return HOST::OK;
}

int Host::load(const std::vector<std::string> &paths)
{
    for (const std::string &path : paths)
    {
        const int result = load(path);
        if (result != HOST::OK)
            return result;
    }
    return HOST::OK;
}

int Host::registerFunction(const std::string &name, int arity, HOST::Function function, void *user)
{
    if (name.empty() || function == nullptr || arity < 0 || arity > ISA::REGISTER_COUNT)
        return lastError = HOST::BAD_ARGUMENT;
    for (const HOST::Registered &entry : registry)
        if (entry.name == name)
            return lastError = HOST::BAD_ARGUMENT;
    registry.push_back({name, arity, function, user});
    return HOST::OK;
}

int Host::bindImports(const Parse &module, std::vector<CORE::HostSlot> &slots)
{
    const PARSE::Image &image = module.image();
    slots.assign(image.imports.size(), CORE::HostSlot{});
    for (std::size_t i = 0; i < image.imports.size(); ++i)
    {
        const PARSE::Import &import = image.imports[i];
        if (import.name >= image.constants.size())
            return lastError = HOST::UNLINKED_IMPORT;

        const std::span<const byte> payload = image.constants[import.name].payload;
        const std::string_view name(reinterpret_cast<const char *>(payload.data()), payload.size());
        const HOST::Registered *match = nullptr;
        for (const HOST::Registered &entry : registry)
        {
            if (std::string_view(entry.name) == name &&
                entry.arity == static_cast<int>(import.arity))
            {
                match = &entry;
                break;
            }
        }
        if (match == nullptr)
        {
            lastUnlinkedIndex = static_cast<dword>(i);
            lastUnlinkedName.assign(name);
            lastUnlinkedArity = static_cast<int>(import.arity);
            return lastError = HOST::UNLINKED_IMPORT;
        }

        slots[i].function = match->function;
        slots[i].user = match->user;
        slots[i].arity = match->arity;
    }
    return HOST::OK;
}

int Host::ensureBlocks(qword offset, qword size)
{
    if (size == 0)
        return HOST::OK;
    const qword first = offset / MEMORY::BLOCK_SIZE;
    const qword last = (offset + size - 1) / MEMORY::BLOCK_SIZE;
    for (qword block = first; block <= last; ++block)
    {
        const int result = mem->allocate(static_cast<int>(block));
        if (result != MEMORY::OK)
            return result;
    }
    return HOST::OK;
}

int Host::initMemory(void)
{
    if (modules.empty())
        return lastError = HOST::NO_MODULE;

    mem = std::make_unique<Memory>();
    constantAddr.assign(modules.size(), {});
    hostStats.constantBytes = 0;

    qword cursor = 0;
    for (std::size_t m = 0; m < modules.size(); ++m)
    {
        const std::vector<PARSE::Constant> &constants = modules[m].image().constants;
        constantAddr[m].reserve(constants.size());
        for (const PARSE::Constant &constant : constants)
        {
            const qword size = static_cast<qword>(constant.payload.size());
            cursor = (cursor + 7) & ~qword(7); // 8-byte aligned
            if (cursor > 0xFFFFFFFFull)
                return lastError = MEMORY::MEMORY_OVERFLOW;

            const int allocated = ensureBlocks(cursor, size);
            if (allocated != HOST::OK)
                return lastError = allocated;

            if (size != 0)
            {
                const int written = mem->writes(constant.payload, static_cast<dword>(cursor),
                                                static_cast<dword>(size));
                if (written != MEMORY::OK)
                    return lastError = written;
            }
            constantAddr[m].push_back(static_cast<dword>(cursor));
            cursor += size != 0 ? size : 4;
        }
    }

    lastError = HOST::OK;
    hostStats.constantBytes = cursor;
    hostStats.result = HOST::OK;
    return HOST::OK;
}

int Host::run(void)
{
    if (modules.empty())
        return lastError = HOST::NO_MODULE;
    if (!mem)
        return lastError = HOST::NO_MEMORY;

    for (std::size_t index = 0; index < modules.size(); ++index)
    {
        const int result = runModule(index);
        if (result != HOST::OK)
            return result;
    }

    lastError = HOST::OK;
    return HOST::OK;
}

int Host::runModule(std::size_t index)
{
    return runModule(index, nullptr, nullptr);
}

int Host::runModule(std::size_t index, CORE::DebugHook hook, void *user)
{
    if (modules.empty() || index >= modules.size())
        return lastError = HOST::NO_MODULE;
    if (!mem)
        return lastError = HOST::NO_MEMORY;

    Parse &parsed = modules[index];
    lastCore = std::make_unique<Core>(*mem);
    Core &core = *lastCore;
    int result = core.attachModule(parsed.text(), parsed.functions());
    if (result != CORE::OK)
    {
        hostStats.result = result;
        return lastError = result;
    }

    core.attachImports(imports[index]);
    core.setDebugHook(hook, user);
    core.run(parsed.entryFunction());
    core.setDebugHook(nullptr, nullptr); // do not keep a dangling user pointer

    ++hostStats.runs;
    hostStats.instructions += core.instructionCount();
    if (core.peakCallDepth() > hostStats.maxDepth)
        hostStats.maxDepth = core.peakCallDepth();
    hostStats.lastPc = core.currentPc();
    hostStats.trapCode = core.trapCode();
    hostStats.result = core.error();
    lastError = core.error();
    return lastError;
}

dword Host::constantAddress(dword module, dword index) const
{
    if (module >= constantAddr.size() || index >= constantAddr[module].size())
        return NO_INDEX;
    return constantAddr[module][index];
}
