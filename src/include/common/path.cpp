#include "common/path.hpp"

#include <filesystem>
#include <system_error>

#include "common/text.hpp"

bool TOOL::samePath(const std::string &left, const std::string &right)
{
    std::error_code code;
    std::string a = std::filesystem::absolute(left, code).lexically_normal().string();
    code.clear();
    std::string b = std::filesystem::absolute(right, code).lexically_normal().string();
#if defined(_WIN32)
    a = TOOL::toLowerAscii(a);
    b = TOOL::toLowerAscii(b);
#endif
    return a == b;
}
