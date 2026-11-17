#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace TOOL
{
    // Internal text is UTF-8 on every platform. std::filesystem::path's narrow
    // constructors use the native encoding (ANSI on Windows), so go through
    // char8_t explicitly there; POSIX paths are byte strings and need no change.
    inline std::filesystem::path pathFromUtf8(std::string_view text)
    {
#if defined(_WIN32)
        try
        {
            const std::u8string utf8(reinterpret_cast<const char8_t *>(text.data()),
                                     text.size());
            return std::filesystem::path(utf8);
        }
        catch (const std::filesystem::filesystem_error &)
        {
            try
            {
                return std::filesystem::path(std::string(text)); // ANSI fallback
            }
            catch (const std::filesystem::filesystem_error &)
            {
                return std::filesystem::path();
            }
        }
#else
        return std::filesystem::path(std::string(text));
#endif
    }

    inline std::string pathToUtf8(const std::filesystem::path &path)
    {
#if defined(_WIN32)
        try
        {
            const std::u8string utf8 = path.u8string();
            return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
        }
        catch (const std::filesystem::filesystem_error &)
        {
            try
            {
                return path.string(); // ANSI fallback
            }
            catch (const std::filesystem::filesystem_error &)
            {
                return std::string();
            }
        }
#else
        return path.string();
#endif
    }

    // True when both paths denote the same file after making them absolute and
    // (on Windows) case-insensitive. The files need not exist.
    bool samePath(const std::string &left, const std::string &right);
}
