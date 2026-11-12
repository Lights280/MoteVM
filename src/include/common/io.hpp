#pragma once
#include <cstdint>
#include <span>
#include <string>

#include "common/types.hpp"

namespace TOOL
{
    // Whole-file / stdin reads. Returns false and fills error on failure.
    // The optional cap is checked before allocating for files and while
    // reading for stdin; the default accepts any size.
    inline constexpr std::uintmax_t NO_READ_LIMIT = static_cast<std::uintmax_t>(-1);
    bool readFile(const std::string &path, Bytes &data, std::string &error,
                  std::uintmax_t maxBytes = NO_READ_LIMIT);
    bool readStdin(Bytes &data, std::string &error,
                   std::uintmax_t maxBytes = NO_READ_LIMIT);

    // Writes bytes to path through a same-directory temporary file and an
    // atomic replacement. On failure the temporary file is removed and the
    // destination is left untouched (plan/05 §6.3 step 7).
    bool writeFileAtomic(const std::string &path, std::span<const byte> data, std::string &error);

    // Writes raw bytes to stdout (no text translation).
    bool writeStdout(std::span<const byte> data, std::string &error);

    // Switches stdin/stdout to binary mode on Windows.
    void setBinaryStdio(void);
}
