#pragma once
// Shared types for the command-line tools.
// Tool-only code lives in src/include; it never enters the .mbc format.

#include <cstdint>
#include <string>
#include <vector>

#include "define.hpp"

namespace TOOL
{
    using Bytes = std::vector<byte>;

    // Process exit codes (plan/05 §8.5): 0 ok, 1 usage, 2 input, 4 internal.
    inline constexpr int OK = 0;
    inline constexpr int USAGE = 1;
    inline constexpr int INPUT = 2;
    inline constexpr int INTERNAL = 4;

    struct SourcePosition
    {
        int line = 0;   // 1-based logical source line; 0 = no position
        int column = 0; // 1-based Unicode code point column
    };

    enum class Severity
    {
        Error,
        Warning
    };
}
