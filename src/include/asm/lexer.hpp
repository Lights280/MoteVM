#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "asm/assembly.hpp"
#include "common/diagnostics.hpp"
#include "common/types.hpp"

namespace ASM
{
    struct Token
    {
        std::string text;          // word text (strings hold the raw source form)
        bool isString = false;
        std::vector<byte> bytes;   // decoded string payload
        TOOL::SourcePosition position;
    };

    // Splits one logical line into words. Commas are treated as separators; a
    // ';' outside a string starts a comment. String escapes are decoded here.
    bool tokenizeLine(std::string_view line, int lineNumber, std::vector<Token> &tokens,
                      TOOL::Diagnostics &diagnostics);
}
