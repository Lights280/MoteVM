#pragma once
#include <span>
#include <vector>

#include "asm/assembly.hpp"
#include "common/diagnostics.hpp"

namespace ASM
{
    struct Options
    {
        bool noDebug = false;    // --no-debug
        bool forceDebug = false; // -g / --debug
    };

    // Parses, validates, encodes and emits a MOTEBC 1.0 image, then verifies it
    // through the same Parse/Verifier used by motevm (E031 on mismatch).
    // status: 0 ok, 2 input (source) error, 4 internal error.
    AssembleResult assemble(std::span<const byte> source, const Options &options,
                    TOOL::Diagnostics &diagnostics);
}
