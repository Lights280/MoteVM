#pragma once
#include <string>

namespace TOOL
{
    // Consumes the next argv token as an option value. Returns false when the
    // value is missing or empty.
    inline bool takeValue(int argc, char *argv[], int &index, std::string &value)
    {
        if (index + 1 >= argc || argv[index + 1] == nullptr)
            return false;
        value = argv[++index];
        return !value.empty();
    }
}
