#pragma once

#include <filesystem>
#include <string>

namespace WORKSPACE
{
    const int OK = 0;
    const int NO_ROOT = 1;      // no -w given: stdio only, no directory granted
    const int BAD_ROOT = 2;     // workspace does not exist / cannot be canonicalized
    const int NOT_DIRECTORY = 3; // workspace exists but is not a directory
    const int BAD_PATH = 4;     // empty, embedded NUL, drive/root relative path
    const int OUTSIDE = 5;      // canonical target escapes the workspace root
    const int NO_PARENT = 6;    // write target has no existing parent inside the root
}

// Sandbox root for every filesystem access made on behalf of a module.
// An unset Workspace means "stdio only": no directory is granted at all.
class Workspace
{
private:
    std::filesystem::path rootPath;
    bool ready = false;

private:
    int resolve(const std::string &userPath, std::filesystem::path &out) const;

public:
    Workspace() = default;

    // Canonicalizes and validates the sandbox root; call it once before resolve*().
    int setRoot(const std::string &directory);

    bool hasRoot(void) const { return ready; }
    std::string rootText(void) const;
    bool contains(const std::filesystem::path &candidate) const;

    // Maps a user supplied path to a canonical absolute path inside the root.
    int resolveRead(const std::string &userPath, std::filesystem::path &out) const;
    // Same as resolveRead, plus the parent directory must exist inside the root.
    int resolveWrite(const std::string &userPath, std::filesystem::path &out) const;
};
