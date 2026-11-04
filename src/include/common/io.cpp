#include "common/io.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace
{
    std::string systemMessage(const std::error_code &error)
    {
        return error ? error.message() : std::string("unknown error");
    }

    std::string errnoMessage(int code)
    {
#if defined(_WIN32)
        char buffer[128] = {};
        strerror_s(buffer, sizeof(buffer), code);
        return std::string(buffer);
#else
        return std::string(std::strerror(code));
#endif
    }
}

bool TOOL::readFile(const std::string &path, Bytes &data, std::string &error)
{
    data.clear();
    std::error_code code;
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    if (code)
    {
        error = "cannot read '" + path + "': " + systemMessage(code);
        return false;
    }
    if (size > static_cast<std::uintmax_t>(~static_cast<std::size_t>(0)))
    {
        error = "cannot read '" + path + "': file is too large";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "cannot open '" + path + "'";
        return false;
    }

    data.resize(static_cast<std::size_t>(size));
    if (size != 0)
        file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
    if (!file || static_cast<std::uintmax_t>(file.gcount()) != size)
    {
        error = "cannot read '" + path + "': unexpected end of file";
        data.clear();
        return false;
    }
    return true;
}

bool TOOL::readStdin(Bytes &data, std::string &error)
{
    data.clear();
    std::array<char, 65536> buffer{};
    while (true)
    {
        std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = std::cin.gcount();
        if (count < 0)
        {
            error = "cannot read stdin";
            return false;
        }
        data.insert(data.end(), reinterpret_cast<const byte *>(buffer.data()),
                    reinterpret_cast<const byte *>(buffer.data()) + count);
        if (!std::cin)
        {
            if (std::cin.eof())
                break;
            error = "cannot read stdin";
            return false;
        }
        if (count == 0)
            break;
    }
    std::cin.clear();
    return true;
}

bool TOOL::writeFileAtomic(const std::string &path, std::span<const byte> data, std::string &error)
{
    const std::filesystem::path target(path);
    std::filesystem::path temporary = target;
    temporary += ".tmp";

    std::error_code code;
    std::filesystem::remove(temporary, code);
    code.clear();

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            error = "cannot create temporary file for '" + path + "': " + errnoMessage(errno);
            return false;
        }
        if (!data.empty())
            file.write(reinterpret_cast<const char *>(data.data()),
                       static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file)
        {
            file.close();
            std::filesystem::remove(temporary, code);
            error = "cannot write '" + path + "'";
            return false;
        }
    }

#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        std::filesystem::remove(temporary, code);
        error = "cannot replace '" + path + "'";
        return false;
    }
#else
    std::filesystem::rename(temporary, target, code);
    if (code)
    {
        std::filesystem::remove(temporary, code);
        error = "cannot replace '" + path + "'";
        return false;
    }
#endif
    return true;
}

bool TOOL::writeStdout(std::span<const byte> data, std::string &error)
{
    if (!data.empty())
        std::cout.write(reinterpret_cast<const char *>(data.data()),
                        static_cast<std::streamsize>(data.size()));
    std::cout.flush();
    if (!std::cout)
    {
        error = "cannot write stdout";
        return false;
    }
    return true;
}

void TOOL::setBinaryStdio(void)
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}
