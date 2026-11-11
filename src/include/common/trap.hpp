#pragma once
#include <string_view>

#include "common/types.hpp"

namespace TOOL
{
    // TrapCode names from plan/05 §4.4 (1..16). nullptr for 0/17+.
    const char *trapName(dword code);

    // Case-insensitive name lookup; false when unknown.
    bool trapCode(std::string_view name, dword &code);
}
