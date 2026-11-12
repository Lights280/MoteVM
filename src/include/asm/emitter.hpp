#pragma once
#include <vector>

#include "asm/assembler.hpp"
#include "asm/assembly.hpp"
#include "common/diagnostics.hpp"

namespace ASM
{
    // Builds the section table, section payloads and header of a MOTEBC 1.0
    // image from a finalized Module. Fills the encoded word stream/listing.
    bool emitImage(Module &module, const Options &options, std::vector<byte> &image,
                   std::vector<ListingEntry> &listing, TOOL::Diagnostics &diagnostics);
}
