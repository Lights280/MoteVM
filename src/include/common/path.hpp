#pragma once
#include <string>

namespace TOOL
{
    // True when both paths denote the same file after making them absolute and
    // (on Windows) case-insensitive. The files need not exist.
    bool samePath(const std::string &left, const std::string &right);
}
