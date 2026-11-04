#include "workspace.hpp"

#include <cstddef>
#include <cwctype>
#include <iterator>

namespace
{
    bool hasEmbeddedNul(const std::string &text)
    {
        return text.find('\0') != std::string::npos;
    }

#ifdef _WIN32
    bool sameComponent(const std::filesystem::path &left, const std::filesystem::path &right)
    {
        const std::wstring a = left.wstring();
        const std::wstring b = right.wstring();
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::towlower(a[i]) != std::towlower(b[i]))
                return false;
        return true;
    }
#else
    bool sameComponent(const std::filesystem::path &left, const std::filesystem::path &right)
    {
        return left.string() == right.string();
    }
#endif
}

int Workspace::setRoot(const std::string &directory)
{
    ready = false;
    rootPath.clear();

    if (directory.empty() || hasEmbeddedNul(directory))
        return WORKSPACE::BAD_ROOT;

    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(directory, error);
    if (error)
        return WORKSPACE::BAD_ROOT;

    const std::filesystem::file_status status = std::filesystem::status(canonical, error);
    if (error || !std::filesystem::exists(status))
        return WORKSPACE::BAD_ROOT;
    if (!std::filesystem::is_directory(status))
        return WORKSPACE::NOT_DIRECTORY;

    rootPath = canonical;
    ready = true;
    return WORKSPACE::OK;
}

std::string Workspace::rootText(void) const
{
    return rootPath.string();
}

// Component-wise containment: "C:\ws2" is not inside "C:\ws" (a plain string
// prefix would wrongly accept it). Both paths are already canonical here, so
// symlinks/junctions have been resolved and cannot escape the root.
bool Workspace::contains(const std::filesystem::path &candidate) const
{
    if (!ready)
        return false;

    const std::size_t rootCount =
        static_cast<std::size_t>(std::distance(rootPath.begin(), rootPath.end()));
    const std::size_t candidateCount =
        static_cast<std::size_t>(std::distance(candidate.begin(), candidate.end()));
    if (candidateCount < rootCount)
        return false;

    auto rootIt = rootPath.begin();
    auto candidateIt = candidate.begin();
    for (std::size_t i = 0; i < rootCount; ++i, ++rootIt, ++candidateIt)
        if (!sameComponent(*rootIt, *candidateIt))
            return false;
    return true;
}

int Workspace::resolve(const std::string &userPath, std::filesystem::path &out) const
{
    if (!ready)
        return WORKSPACE::NO_ROOT;
    if (userPath.empty() || hasEmbeddedNul(userPath))
        return WORKSPACE::BAD_PATH;

    const std::filesystem::path input(userPath);
    // Windows drive-relative ("C:foo") and root-relative ("\foo") forms are
    // ambiguous: they silently bind to a per-drive or per-process directory.
    if (!input.is_absolute() && (input.has_root_name() || input.has_root_directory()))
        return WORKSPACE::BAD_PATH;

    const std::filesystem::path combined = input.is_absolute() ? input : rootPath / input;

    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(combined, error);
    if (error)
        return WORKSPACE::BAD_PATH;
    if (!contains(canonical))
        return WORKSPACE::OUTSIDE;

    out = canonical;
    return WORKSPACE::OK;
}

int Workspace::resolveRead(const std::string &userPath, std::filesystem::path &out) const
{
    return resolve(userPath, out);
}

int Workspace::resolveWrite(const std::string &userPath, std::filesystem::path &out) const
{
    const int resolved = resolve(userPath, out);
    if (resolved != WORKSPACE::OK)
        return resolved;

    const std::filesystem::path parent = out.parent_path();
    if (parent.empty())
        return WORKSPACE::NO_PARENT;

    std::error_code error;
    const std::filesystem::path canonicalParent = std::filesystem::weakly_canonical(parent, error);
    if (error)
        return WORKSPACE::NO_PARENT;

    const std::filesystem::file_status status = std::filesystem::status(canonicalParent, error);
    if (error || !std::filesystem::is_directory(status))
        return WORKSPACE::NO_PARENT;
    if (!contains(canonicalParent))
        return WORKSPACE::NO_PARENT;
    return WORKSPACE::OK;
}
