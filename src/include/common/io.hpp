#pragma once
#include <span>
#include <string>

#include "common/types.hpp"

namespace TOOL
{
    // Whole-file / stdin reads. Returns false and fills error on failure.
    bool readFile(const std::string &path, Bytes &data, std::string &error);
    bool readStdin(Bytes &data, std::string &error);

    // Writes bytes to path through a same-directory temporary file and an
    // atomic replacement. On failure the temporary file is removed and the
    // destination is left untouched (plan/05 §6.3 step 7).
    bool writeFileAtomic(const std::string &path, std::span<const byte> data, std::string &error);

    // Writes raw bytes to stdout (no text translation).
    bool writeStdout(std::span<const byte> data, std::string &error);

    // Switches stdin/stdout to binary mode on Windows.
    void setBinaryStdio(void);
}
