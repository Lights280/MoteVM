#pragma once
#include <span>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace DISASM
{
    struct TextResult
    {
        int status = TOOL::OK;
        std::string text;
        std::vector<std::string> warnings;
        std::string error;
    };

    // Renders canonical .mta text. Input must already be a valid module; the
    // same Parse/Verifier as motevm is used and invalid input returns INPUT.
    TextResult renderCanonical(std::span<const byte> module, bool includeDebug, bool annotate);
}
