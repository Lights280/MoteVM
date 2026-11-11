#pragma once

#include <string>

#include "types.hpp"

namespace BUILD
{
    class CommandLineParser
    {
    public:
        CommandLineParser(int argc, char *argv[], Options &options);
        int parse(std::string &error);

    private:
        enum class OptionStatus
        {
            NotOption,
            Handled,
            Failed
        };

        OptionStatus parseOption(const std::string &argument, std::string &error);
        bool takeValue(std::string &value);
        void parsePositional(const std::string &argument);

        int argc_;
        char **argv_;
        Options &options_;
        int index_ = 1;
        bool endOfOptions_ = false;
        bool commandSeen_ = false;
    };
}
