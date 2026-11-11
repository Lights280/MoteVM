#include "common/trap.hpp"

#include <array>

#include "common/text.hpp"

namespace
{
    struct TrapEntry
    {
        dword code;
        const char *name;
    };

    constexpr std::array<TrapEntry, 16> TRAPS = {{
        {1, "TypeMismatch"},
        {2, "DivByZero"},
        {3, "IntegerOverflow"},
        {4, "NumericConversion"},
        {5, "IndexOutOfBounds"},
        {6, "NullAccess"},
        {7, "StackOverflow"},
        {8, "InvalidBytecode"},
        {9, "UnlinkedImport"},
        {10, "OutOfMemory"},
        {11, "HostError"},
        {12, "FuelExhausted"},
        {13, "Interrupted"},
        {14, "NotImplemented"},
        {15, "InternalError"},
        {16, "InvalidArgument"},
    }};
}

const char *TOOL::trapName(dword code)
{
    for (const TrapEntry &entry : TRAPS)
        if (entry.code == code)
            return entry.name;
    return nullptr;
}

bool TOOL::trapCode(std::string_view name, dword &code)
{
    const std::string lowered = TOOL::toLowerAscii(name);
    for (const TrapEntry &entry : TRAPS)
    {
        if (lowered == TOOL::toLowerAscii(entry.name))
        {
            code = entry.code;
            return true;
        }
    }
    return false;
}
