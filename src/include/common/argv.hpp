#pragma once
// Windows command lines arrive as UTF-16; the tools store text as UTF-8.
// WideArgv keeps the converted storage alive and exposes a mutable argv view
// for TOOL::runMain. POSIX builds use the process argv directly.
#include <string>
#include <vector>

#if defined(_WIN32)
#include <filesystem>

#include "common/path.hpp"

namespace TOOL
{
    class WideArgv
    {
    public:
        WideArgv(int argc, wchar_t *argv[]) : argc_(argc)
        {
            storage_.reserve(static_cast<std::size_t>(argc < 0 ? 0 : argc));
            pointers_.reserve(static_cast<std::size_t>(argc < 0 ? 0 : argc) + 1);
            for (int i = 0; i < argc; ++i)
                storage_.push_back(argv[i] == nullptr
                                       ? std::string()
                                       : pathToUtf8(std::filesystem::path(argv[i])));
            for (std::string &argument : storage_)
                pointers_.push_back(argument.data());
            pointers_.push_back(nullptr);
        }

        int argc() const { return argc_; }
        char **argv() { return pointers_.data(); }

    private:
        int argc_;
        std::vector<std::string> storage_;
        std::vector<char *> pointers_;
    };
}
#endif
