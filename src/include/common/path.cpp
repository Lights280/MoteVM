#include "common/path.hpp"

#include <filesystem>
#include <system_error>

#include "common/text.hpp"

namespace
{
    std::string normalised(const std::string &text)
    {
        std::error_code code;
        const std::filesystem::path absolute =
            std::filesystem::absolute(TOOL::pathFromUtf8(text), code);
        const std::filesystem::path &result = code ? TOOL::pathFromUtf8(text) : absolute;
        return TOOL::pathToUtf8(result.lexically_normal());
    }
}

bool TOOL::samePath(const std::string &left, const std::string &right)
{
    std::string a = normalised(left);
    std::string b = normalised(right);
#if defined(_WIN32)
    a = TOOL::toLowerAscii(a);
    b = TOOL::toLowerAscii(b);
#endif
    return a == b;
}
