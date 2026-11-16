#pragma once
// Archive entry names are logical relative paths, not host paths.
// Rules are intentionally stricter than POSIX/Windows so that unpacking the
// same archive on every platform cannot escape the destination directory.

#include <cstddef>
#include <string>
#include <string_view>

#include "define.hpp"

namespace ARCHIVE
{
    // Validates a UTF-8 relative path; appends a human-readable reason on failure.
    bool validName(std::string_view name, std::string *error = nullptr);

    // ASCII-only case fold, used to reject names that collide on Windows.
    std::string foldAscii(std::string_view name);

    // True when the last path segment ends with ".mbc" (case-insensitive) and
    // has at least one character before the suffix.
    bool hasModuleSuffix(std::string_view name);
}
