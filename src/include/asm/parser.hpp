#pragma once
#include <string>
#include <vector>

#include "asm/assembly.hpp"
#include "common/diagnostics.hpp"

namespace ASM
{
    // Parses UTF-8 .mta text into a Module (pass 1 layout model). Diagnostics
    // carry source positions; false means at least one error was recorded.
    bool parseSource(const std::vector<byte> &source, Module &module,
                     TOOL::Diagnostics &diagnostics);
}
