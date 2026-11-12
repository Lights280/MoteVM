#include "cli.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "archive/archive.hpp"
#include "archive/manifest.hpp"
#include "archive/util.hpp"
#include "archive/writer.hpp"
#include "common/cli.hpp"
#include "common/io.hpp"
#include "common/path.hpp"
#include "common/text.hpp"
#include "define.hpp"

namespace
{
    bool parseFileSpec(const std::string &text, CLI::FileSpec &spec, std::string &error)
    {
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos)
        {
            spec.path = text;
            spec.name.clear();
        }
        else
        {
            spec.path = text.substr(0, equals);
            spec.name = text.substr(equals + 1);
        }
        if (spec.path.empty())
        {
            error = "empty input path";
            return false;
        }
        if (equals != std::string::npos && spec.name.empty())
        {
            error = "empty archive name after '='";
            return false;
        }
        return true;
    }

    std::string defaultName(const std::string &path)
    {
        return std::filesystem::path(path).filename().string();
    }

    // Checks the input size before reading, so a huge local file cannot be
    // pulled into memory only to be rejected by the writer.
    int readInputFile(const std::string &path, std::size_t limit, TOOL::Bytes &data,
                      std::string &error)
    {
        std::error_code sizeError;
        const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
        if (sizeError)
        {
            error = "cannot read '" + path + "': " + sizeError.message();
            return 3;
        }
        if (size > static_cast<std::uintmax_t>(limit))
        {
            error = "'" + path + "' exceeds the 64 MiB entry limit";
            return 2;
        }
        if (!TOOL::readFile(path, data, error))
            return 3;
        return 0;
    }

    const char *kindName(ARCHIVE::EntryKind kind)
    {
        return kind == ARCHIVE::EntryKind::module ? "module" : "resource";
    }

    std::string crcHex(dword value)
    {
        std::ostringstream out;
        out << std::hex << std::setw(8) << std::setfill('0') << value;
        return out.str();
    }

    const char *manifestKeyName(word key)
    {
        switch (key)
        {
        case ARCHIVE::META_PACKAGE_NAME:
            return "package";
        case ARCHIVE::META_PACKAGE_VERSION:
            return "version";
        case ARCHIVE::META_ENTRY_MODULE:
            return "entry";
        case ARCHIVE::META_DESCRIPTION:
            return "description";
        case ARCHIVE::META_AUTHOR:
            return "author";
        case ARCHIVE::META_LICENSE:
            return "license";
        case ARCHIVE::META_BUILD_ID:
            return "build-id";
        case ARCHIVE::META_CREATED_BY:
            return "created-by";
        default:
            return "unknown";
        }
    }

    std::size_t countKind(const ARCHIVE::Archive &archive, ARCHIVE::EntryKind kind)
    {
        std::size_t count = 0;
        for (const ARCHIVE::Entry &entry : archive.entries())
            if (entry.kind == kind)
                ++count;
        return count;
    }

    int loadArchive(const CLI::Options &options, bool verifyModules,
                    ARCHIVE::Archive &archive, std::ostream &err)
    {
        int result = archive.load(options.input);
        if (result != ARCHIVE::OK)
        {
            err << "motear: " << options.input << ": " << ARCHIVE::errorText(result) << "\n";
            return result == ARCHIVE::FAIL_OPEN ? 3 : 2;
        }

        result = archive.parse(verifyModules);
        if (result != ARCHIVE::OK)
        {
            err << "motear: " << options.input << ": " << ARCHIVE::errorText(result);
            if (!archive.detail().empty())
                err << " (" << archive.detail() << ")";
            err << "\n";
            return 2;
        }
        return 0;
    }

    bool underRoot(const std::filesystem::path &root,
                   const std::filesystem::path &candidate)
    {
        const std::filesystem::path relative = candidate.lexically_relative(root);
        if (relative.empty())
            return false;
        for (const std::filesystem::path &part : relative)
            if (part == "..")
                return false;
        return true;
    }

    bool missingFile(const std::error_code &error)
    {
        return error == std::make_error_condition(std::errc::no_such_file_or_directory);
    }

    bool checkTarget(const std::filesystem::path &root,
                     const std::filesystem::path &target, bool force, std::string &error)
    {
        const std::filesystem::path relative = target.lexically_relative(root);
        if (relative.empty())
        {
            error = "target is outside the destination directory";
            return false;
        }

        std::vector<std::filesystem::path> parts;
        for (const std::filesystem::path &part : relative)
            parts.push_back(part);
        if (parts.empty())
        {
            error = "target is outside the destination directory";
            return false;
        }

        std::filesystem::path current = root;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i)
        {
            current /= parts[i];
            std::error_code statusError;
            const std::filesystem::file_status status =
                std::filesystem::symlink_status(current, statusError);
            if (statusError)
            {
                if (missingFile(statusError))
                    break;
                error = "cannot inspect '" + current.string() + "': " + statusError.message();
                return false;
            }
            if (std::filesystem::is_symlink(status))
            {
                error = "path component is a symbolic link";
                return false;
            }
            if (std::filesystem::exists(status) &&
                !std::filesystem::is_directory(status))
            {
                error = "path component is not a directory";
                return false;
            }
        }

        std::error_code canonicalError;
        const std::filesystem::path canonicalParent =
            std::filesystem::weakly_canonical(target.parent_path(), canonicalError);
        if (canonicalError)
        {
            error = "cannot resolve the target directory: " + canonicalError.message();
            return false;
        }
        if (!underRoot(root, canonicalParent))
        {
            error = "target is outside the destination directory";
            return false;
        }

        std::error_code statusError;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(target, statusError);
        if (statusError)
        {
            if (!missingFile(statusError))
            {
                error = "cannot inspect target: " + statusError.message();
                return false;
            }
        }
        else if (std::filesystem::is_symlink(status))
        {
            error = "target is a symbolic link";
            return false;
        }
        else if (std::filesystem::exists(status))
        {
            if (!std::filesystem::is_regular_file(status))
            {
                error = "target exists and is not a regular file";
                return false;
            }
            if (!force)
            {
                error = "target already exists (use --force)";
                return false;
            }
        }

        if (!underRoot(root, target.lexically_normal()))
        {
            error = "target is outside the destination directory";
            return false;
        }

        // TOOL::writeFileAtomic uses "<target>.tmp". Refuse to start when that
        // path already exists, otherwise an unrelated file would be removed.
        std::filesystem::path temporary = target;
        temporary += ".tmp";
        std::error_code temporaryError;
        const std::filesystem::file_status temporaryStatus =
            std::filesystem::symlink_status(temporary, temporaryError);
        if (!temporaryError && std::filesystem::exists(temporaryStatus))
        {
            error = "temporary path '" + temporary.string() +
                    "' already exists; refusing to overwrite it";
            return false;
        }
        if (temporaryError && !missingFile(temporaryError))
        {
            error = "cannot inspect temporary path: " + temporaryError.message();
            return false;
        }
        return true;
    }

    void printManifest(std::ostream &out, const ARCHIVE::Manifest &manifest)
    {
        for (const ARCHIVE::ManifestItem &item : manifest.items())
        {
            out << "  " << manifestKeyName(item.key) << " (" << item.key << "): ";
            if (item.key >= ARCHIVE::META_PACKAGE_NAME &&
                item.key <= ARCHIVE::META_CREATED_BY)
                out << manifest.text(item.key);
            else
                out << TOOL::escapeBytes(item.value);
            out << "\n";
        }
    }

    std::string jsonEscape(std::string_view text)
    {
        static const char *HEX = "0123456789abcdef";
        std::string out;
        out.reserve(text.size() + 2);
        for (char value : text)
        {
            const unsigned char byte = static_cast<unsigned char>(value);
            switch (byte)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20)
                {
                    out += "\\u00";
                    out += HEX[(byte >> 4) & 0x0F];
                    out += HEX[byte & 0x0F];
                }
                else
                {
                    out.push_back(static_cast<char>(byte));
                }
                break;
            }
        }
        return out;
    }

    int runPack(const CLI::Options &options, std::ostream &out, std::ostream &err)
    {
        if (!options.entry.empty() && options.modules.empty())
        {
            err << "motear: --entry requires at least one --module\n";
            return 2;
        }
        if (options.entry.empty() && options.modules.size() > 1)
        {
            err << "motear: --entry is required when packing multiple modules\n";
            return 2;
        }

        for (const CLI::FileSpec &spec : options.modules)
            if (TOOL::samePath(options.output, spec.path))
            {
                err << "motear: output must not overwrite input '" << spec.path
                    << "'\n";
                return 2;
            }
        for (const CLI::FileSpec &spec : options.resources)
            if (TOOL::samePath(options.output, spec.path))
            {
                err << "motear: output must not overwrite input '" << spec.path
                    << "'\n";
                return 2;
            }

        ARCHIVE::Writer writer;
        std::string error;

        for (const CLI::FileSpec &spec : options.modules)
        {
            TOOL::Bytes data;
            std::string ioError;
            const int read = readInputFile(
                spec.path, static_cast<std::size_t>(ARCHIVE::MAX_ENTRY_BYTES), data,
                ioError);
            if (read != 0)
            {
                err << "motear: " << ioError << "\n";
                return read;
            }

            const std::string name = spec.name.empty() ? defaultName(spec.path) : spec.name;
            if (name.empty())
            {
                err << "motear: " << spec.path << ": cannot derive a module name\n";
                return 2;
            }
            if (!writer.addModule(name, std::span<const byte>(data), error))
            {
                err << "motear: " << spec.path << ": " << error << "\n";
                return 2;
            }
        }

        for (const CLI::FileSpec &spec : options.resources)
        {
            TOOL::Bytes data;
            std::string ioError;
            const int read = readInputFile(
                spec.path, static_cast<std::size_t>(ARCHIVE::MAX_ENTRY_BYTES), data,
                ioError);
            if (read != 0)
            {
                err << "motear: " << ioError << "\n";
                return read;
            }

            const std::string name =
                spec.name.empty() ? defaultName(spec.path) : spec.name;
            if (name.empty())
            {
                err << "motear: " << spec.path << ": cannot derive a resource name\n";
                return 2;
            }
            if (!writer.addResource(name, std::span<const byte>(data), error))
            {
                err << "motear: " << spec.path << ": " << error << "\n";
                return 2;
            }
        }

        std::string packageName = options.packageName;
        if (packageName.empty())
            packageName = std::filesystem::path(options.output).stem().string();
        if (packageName.empty())
            packageName = "package";
        const std::string packageVersion =
            options.packageVersion.empty() ? "0.0.0" : options.packageVersion;

        if (!writer.setMetadata(ARCHIVE::META_PACKAGE_NAME, packageName, error) ||
            !writer.setMetadata(ARCHIVE::META_PACKAGE_VERSION, packageVersion, error))
        {
            err << "motear: " << error << "\n";
            return 2;
        }

        if (!options.entry.empty() &&
            !writer.setMetadata(ARCHIVE::META_ENTRY_MODULE, options.entry, error))
        {
            err << "motear: " << error << "\n";
            return 2;
        }
        if (!options.description.empty() &&
            !writer.setMetadata(ARCHIVE::META_DESCRIPTION, options.description, error))
        {
            err << "motear: " << error << "\n";
            return 2;
        }
        if (!options.author.empty() &&
            !writer.setMetadata(ARCHIVE::META_AUTHOR, options.author, error))
        {
            err << "motear: " << error << "\n";
            return 2;
        }
        if (!options.license.empty() &&
            !writer.setMetadata(ARCHIVE::META_LICENSE, options.license, error))
        {
            err << "motear: " << error << "\n";
            return 2;
        }
        if (!options.buildId.empty() &&
            !writer.setMetadata(ARCHIVE::META_BUILD_ID, options.buildId, error))
        {
            err << "motear: " << error << "\n";
            return 2;
        }
        if (!writer.setMetadata(ARCHIVE::META_CREATED_BY,
                                std::string("motear ") + MOTEVM_VERSION, error))
        {
            err << "motear: " << error << "\n";
            return 2;
        }

        std::vector<byte> archiveBytes;
        if (!writer.build(archiveBytes, error))
        {
            err << "motear: " << error << "\n";
            return 2;
        }

        std::string ioError;
        if (!TOOL::writeFileAtomic(options.output, archiveBytes, ioError))
        {
            err << "motear: " << ioError << "\n";
            return 3;
        }

        out << "wrote " << options.output << " (" << options.modules.size()
            << " module(s), " << options.resources.size() << " resource(s), "
            << archiveBytes.size() << " bytes)\n";
        return 0;
    }

    int runList(const CLI::Options &options, std::ostream &out, std::ostream &err)
    {
        ARCHIVE::Archive archive;
        const int loaded = loadArchive(options, options.verify, archive, err);
        if (loaded != 0)
            return loaded;

        const ARCHIVE::Header &header = archive.header();
        out << "mtr " << header.versionMajor << "." << header.versionMinor
            << " entries=" << header.entryCount
            << " modules=" << countKind(archive, ARCHIVE::EntryKind::module)
            << " resources=" << countKind(archive, ARCHIVE::EntryKind::resource)
            << " size=" << header.fileSize << "\n";

        for (const ARCHIVE::Entry &entry : archive.entries())
        {
            out << kindName(entry.kind) << " " << std::setw(10) << entry.size << "  "
                << crcHex(entry.crc32c) << "  " << entry.name;
            if (options.verbose)
                out << "  offset=" << entry.offset;
            out << "\n";
        }

        if (options.verbose)
            printManifest(out, archive.manifest());
        return 0;
    }

    int runInfo(const CLI::Options &options, std::ostream &out, std::ostream &err)
    {
        ARCHIVE::Archive archive;
        const int loaded = loadArchive(options, false, archive, err);
        if (loaded != 0)
            return loaded;

        const ARCHIVE::Header &header = archive.header();
        const ARCHIVE::Manifest &manifest = archive.manifest();
        const std::string packageName = manifest.text(ARCHIVE::META_PACKAGE_NAME);
        const std::string packageVersion = manifest.text(ARCHIVE::META_PACKAGE_VERSION);
        const std::string entryModule = archive.entryModuleName();
        const std::string description = manifest.text(ARCHIVE::META_DESCRIPTION);
        const std::string author = manifest.text(ARCHIVE::META_AUTHOR);
        const std::string license = manifest.text(ARCHIVE::META_LICENSE);
        const std::string buildId = manifest.text(ARCHIVE::META_BUILD_ID);
        const std::string createdBy = manifest.text(ARCHIVE::META_CREATED_BY);

        if (options.json)
        {
            out << "{\n";
            out << "  \"file\": \"" << jsonEscape(options.input) << "\",\n";
            out << "  \"format\": \"MTR " << ARCHIVE::MTR_VERSION << "\",\n";
            out << "  \"size\": " << header.fileSize << ",\n";
            out << "  \"entries\": " << header.entryCount << ",\n";
            out << "  \"modules\": "
                << countKind(archive, ARCHIVE::EntryKind::module) << ",\n";
            out << "  \"resources\": "
                << countKind(archive, ARCHIVE::EntryKind::resource) << ",\n";
            out << "  \"package_name\": \"" << jsonEscape(packageName) << "\",\n";
            out << "  \"package_version\": \"" << jsonEscape(packageVersion)
                << "\",\n";
            out << "  \"entry_module\": \"" << jsonEscape(entryModule) << "\",\n";
            out << "  \"description\": \"" << jsonEscape(description) << "\",\n";
            out << "  \"author\": \"" << jsonEscape(author) << "\",\n";
            out << "  \"license\": \"" << jsonEscape(license) << "\",\n";
            out << "  \"build_id\": \"" << jsonEscape(buildId) << "\",\n";
            out << "  \"created_by\": \"" << jsonEscape(createdBy) << "\"\n";
            out << "}\n";
            return 0;
        }

        out << "file: " << options.input << "\n";
        out << "format: MTR " << ARCHIVE::MTR_VERSION << "\n";
        out << "size: " << header.fileSize << "\n";
        out << "entries: " << header.entryCount
            << " (modules=" << countKind(archive, ARCHIVE::EntryKind::module)
            << ", resources=" << countKind(archive, ARCHIVE::EntryKind::resource)
            << ")\n";
        out << "package: " << packageName << "\n";
        out << "version: " << packageVersion << "\n";
        out << "entry: " << entryModule << "\n";
        out << "description: " << description << "\n";
        out << "author: " << author << "\n";
        out << "license: " << license << "\n";
        out << "build-id: " << buildId << "\n";
        out << "created-by: " << createdBy << "\n";
        return 0;
    }

    int runVerify(const CLI::Options &options, std::ostream &out, std::ostream &err)
    {
        ARCHIVE::Archive archive;
        const int loaded = loadArchive(options, !options.noModules, archive, err);
        if (loaded != 0)
            return loaded;

        if (options.strict)
        {
            if (archive.manifest().text(ARCHIVE::META_PACKAGE_NAME).empty())
            {
                err << "motear: " << options.input
                    << ": strict: manifest package name is missing\n";
                return 2;
            }
            if (archive.manifest().text(ARCHIVE::META_PACKAGE_VERSION).empty())
            {
                err << "motear: " << options.input
                    << ": strict: manifest package version is missing\n";
                return 2;
            }
            std::string entryName;
            std::string entryError;
            if (!archive.resolveEntry(entryName, entryError))
            {
                err << "motear: " << options.input << ": strict: " << entryError
                    << "\n";
                return 2;
            }
        }

        out << "ok: " << options.input << " (" << archive.entries().size()
            << " entries, " << countKind(archive, ARCHIVE::EntryKind::module)
            << " modules, " << countKind(archive, ARCHIVE::EntryKind::resource)
            << " resources)\n";
        return 0;
    }

    int runUnpack(const CLI::Options &options, std::ostream &out, std::ostream &err)
    {
        ARCHIVE::Archive archive;
        const int loaded = loadArchive(options, true, archive, err);
        if (loaded != 0)
            return loaded;

        std::error_code errorCode;
        const std::filesystem::path requested(options.directory);
        std::filesystem::create_directories(requested, errorCode);
        if (errorCode)
        {
            err << "motear: cannot create '" << options.directory
                << "': " << errorCode.message() << "\n";
            return 3;
        }
        const std::filesystem::path root =
            std::filesystem::weakly_canonical(requested, errorCode);
        if (errorCode || !std::filesystem::is_directory(root, errorCode))
        {
            err << "motear: '" << options.directory
                << "' is not a directory\n";
            return 3;
        }

        struct Target
        {
            const ARCHIVE::Entry *entry = nullptr;
            std::filesystem::path path;
        };

        for (const std::string &name : options.moduleFilter)
        {
            const ARCHIVE::Entry *entry = archive.find(name);
            if (entry == nullptr || entry->kind != ARCHIVE::EntryKind::module)
            {
                err << "motear: no module entry named '" << name << "'\n";
                return 2;
            }
        }
        for (const std::string &name : options.resourceFilter)
        {
            const ARCHIVE::Entry *entry = archive.find(name);
            if (entry == nullptr || entry->kind != ARCHIVE::EntryKind::resource)
            {
                err << "motear: no resource entry named '" << name << "'\n";
                return 2;
            }
        }

        const bool anyFilter =
            !options.moduleFilter.empty() || !options.resourceFilter.empty();
        std::vector<Target> targets;

        for (const ARCHIVE::Entry &entry : archive.entries())
        {
            bool selected = !anyFilter;
            if (!selected && entry.kind == ARCHIVE::EntryKind::module &&
                std::find(options.moduleFilter.begin(), options.moduleFilter.end(),
                          entry.name) != options.moduleFilter.end())
                selected = true;
            if (!selected && entry.kind == ARCHIVE::EntryKind::resource &&
                std::find(options.resourceFilter.begin(),
                          options.resourceFilter.end(),
                          entry.name) != options.resourceFilter.end())
                selected = true;
            if (!selected)
                continue;

            std::filesystem::path target = root;
            std::size_t start = 0;
            while (start <= entry.name.size())
            {
                const std::size_t slash = entry.name.find('/', start);
                const std::string part =
                    entry.name.substr(start, slash == std::string::npos
                                                 ? std::string::npos
                                                 : slash - start);
                target /= std::filesystem::path(part);
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }

            std::string targetError;
            if (!checkTarget(root, target, options.force, targetError))
            {
                err << "motear: " << entry.name << ": " << targetError << "\n";
                return 2;
            }
            targets.push_back({&entry, target});
        }

        for (const Target &target : targets)
        {
            std::error_code directoryError;
            std::filesystem::create_directories(target.path.parent_path(),
                                                directoryError);
            if (directoryError)
            {
                err << "motear: cannot create '" << target.path.parent_path().string()
                    << "': " << directoryError.message() << "\n";
                return 3;
            }

            std::string writeError;
            if (!TOOL::writeFileAtomic(target.path.string(), target.entry->data,
                                       writeError))
            {
                err << "motear: " << writeError << "\n";
                return 3;
            }
        }

        out << "extracted " << targets.size() << " entry(ies) to " << root.string()
            << "\n";
        return 0;
    }
}

int CLI::parse(int argc, char *argv[], Options &options)
{
    int index = 1;
    if (index < argc && argv[index] != nullptr && argv[index][0] != '-')
    {
        const std::string command = argv[index++];
        if (command == "pack")
            options.command = Command::pack;
        else if (command == "list")
            options.command = Command::list;
        else if (command == "info")
            options.command = Command::info;
        else if (command == "verify")
            options.command = Command::verify;
        else if (command == "unpack")
            options.command = Command::unpack;
        else
            return 1;
    }

    bool endOfOptions = false;
    for (; index < argc; ++index)
    {
        if (argv[index] == nullptr)
            return 1;
        const std::string argument = argv[index];

        if (!endOfOptions && TOOL::isEndOfOptions(argument))
        {
            endOfOptions = true;
            continue;
        }
        if (!endOfOptions && (argument == "-h" || argument == "--help"))
        {
            options.help = true;
            continue;
        }
        if (!endOfOptions && (argument == "-v" || argument == "--version"))
        {
            options.version = true;
            continue;
        }

        if (!endOfOptions && TOOL::isOptionToken(argument))
        {
            std::string value;

            if (TOOL::matchesOption(argument, "-o") || TOOL::matchesOption(argument, "--output"))
            {
                if (options.command != Command::pack ||
                    !TOOL::optionValue(argument, argument == "-o" ? "-o" : "--output",
                                       argc, argv, index, value))
                    return 1;
                options.output = value;
                continue;
            }
            if (TOOL::matchesOption(argument, "-C") || TOOL::matchesOption(argument, "--directory"))
            {
                if (options.command != Command::unpack ||
                    !TOOL::optionValue(argument, argument == "-C" ? "-C" : "--directory",
                                       argc, argv, index, value))
                    return 1;
                options.directory = value;
                continue;
            }
            if (TOOL::matchesOption(argument, "--module"))
            {
                if (!TOOL::optionValue(argument, "--module", argc, argv, index, value))
                    return 1;
                if (options.command == Command::pack)
                {
                    FileSpec spec;
                    std::string error;
                    if (!parseFileSpec(value, spec, error))
                        return 1;
                    options.modules.push_back(std::move(spec));
                }
                else if (options.command == Command::unpack)
                {
                    options.moduleFilter.push_back(value);
                }
                else
                {
                    return 1;
                }
                continue;
            }
            if (TOOL::matchesOption(argument, "--resource"))
            {
                if (!TOOL::optionValue(argument, "--resource", argc, argv, index, value))
                    return 1;
                if (options.command == Command::pack)
                {
                    FileSpec spec;
                    std::string error;
                    if (!parseFileSpec(value, spec, error))
                        return 1;
                    options.resources.push_back(std::move(spec));
                }
                else if (options.command == Command::unpack)
                {
                    options.resourceFilter.push_back(value);
                }
                else
                {
                    return 1;
                }
                continue;
            }
            if (TOOL::matchesOption(argument, "--entry"))
            {
                if (options.command != Command::pack ||
                    !TOOL::optionValue(argument, "--entry", argc, argv, index, value))
                    return 1;
                options.entry = value;
                continue;
            }
            if (TOOL::matchesOption(argument, "--name"))
            {
                if (options.command != Command::pack ||
                    !TOOL::optionValue(argument, "--name", argc, argv, index, value))
                    return 1;
                options.packageName = value;
                continue;
            }
            if (TOOL::matchesOption(argument, "--package-version"))
            {
                if (options.command != Command::pack ||
                    !TOOL::optionValue(argument, "--package-version", argc, argv, index,
                                       value))
                    return 1;
                options.packageVersion = value;
                continue;
            }
            if (TOOL::matchesOption(argument, "--description"))
            {
                if (options.command != Command::pack ||
                    !TOOL::optionValue(argument, "--description", argc, argv, index, value))
                    return 1;
                options.description = value;
                continue;
            }
            if (TOOL::matchesOption(argument, "--author"))
            {
                if (options.command != Command::pack ||
                    !TOOL::optionValue(argument, "--author", argc, argv, index, value))
                    return 1;
                options.author = value;
                continue;
            }
            if (TOOL::matchesOption(argument, "--license"))
            {
                if (options.command != Command::pack ||
                    !TOOL::optionValue(argument, "--license", argc, argv, index, value))
                    return 1;
                options.license = value;
                continue;
            }
            if (TOOL::matchesOption(argument, "--build-id"))
            {
                if (options.command != Command::pack ||
                    !TOOL::optionValue(argument, "--build-id", argc, argv, index, value))
                    return 1;
                options.buildId = value;
                continue;
            }
            if (argument == "--verbose")
            {
                if (options.command != Command::list)
                    return 1;
                options.verbose = true;
                continue;
            }
            if (argument == "--verify")
            {
                if (options.command != Command::list)
                    return 1;
                options.verify = true;
                continue;
            }
            if (argument == "--strict")
            {
                if (options.command != Command::verify)
                    return 1;
                options.strict = true;
                continue;
            }
            if (argument == "--no-modules")
            {
                if (options.command != Command::verify)
                    return 1;
                options.noModules = true;
                continue;
            }
            if (argument == "--json")
            {
                if (options.command != Command::info)
                    return 1;
                options.json = true;
                continue;
            }
            if (argument == "--force")
            {
                if (options.command != Command::unpack)
                    return 1;
                options.force = true;
                continue;
            }
            return 1;
        }

        if (options.command == Command::none || options.command == Command::pack)
            return 1;
        if (!options.input.empty())
            return 1;
        options.input = argument;
    }

    if (options.help || options.version)
        return 0;
    if (options.command == Command::none)
        return 1;

    if (options.command == Command::pack)
    {
        if (options.output.empty())
            return 1;
        if (options.modules.empty() && options.resources.empty())
            return 1;
    }
    else
    {
        if (options.input.empty())
            return 1;
        if (options.command == Command::unpack && options.directory.empty())
            return 1;
    }
    return 0;
}

void CLI::printVersion(std::ostream &out)
{
    TOOL::printVersionLine(out, "motear", "MTR", ARCHIVE::MTR_VERSION);
}

void CLI::printUsage(std::ostream &out)
{
    TOOL::printUsageHeader(out, "motear", "MTR", ARCHIVE::MTR_VERSION);
    out << "  motear pack -o OUT.mtr [options] --module FILE[=NAME]... "
           "[--resource FILE[=NAME]]...\n";
    out << "  motear list FILE.mtr [--verbose] [--verify]\n";
    out << "  motear info FILE.mtr [--json]\n";
    out << "  motear verify FILE.mtr [--strict] [--no-modules]\n";
    out << "  motear unpack FILE.mtr -C DIR [--force] [--module NAME]... "
           "[--resource NAME]...\n\n";
    out << "pack options:\n";
    out << "  -o, --output FILE        output archive (required)\n";
    out << "      --module FILE[=NAME]  add a MoteBC module (repeatable)\n";
    out << "      --resource FILE[=NAME] add a resource (repeatable)\n";
    out << "      --entry NAME         entry module (required for multiple modules)\n";
    out << "      --name NAME          package name (default: output file stem)\n";
    out << "      --package-version V  package version (default: 0.0.0)\n";
    out << "      --description TEXT   package description\n";
    out << "      --author TEXT        package author\n";
    out << "      --license TEXT       package license\n";
    out << "      --build-id TEXT      build id / provenance\n\n";
    out << "other options:\n";
    out << "  --verbose  list: show offsets and manifest\n";
    out << "  --verify   list: also verify entry CRCs and modules\n";
    out << "  --strict   verify: require package name, version and entry module\n";
    out << "  --no-modules verify: skip MoteBC validation (container-only check)\n";
    out << "  --json     info: print JSON\n";
    out << "  --force    unpack: replace an existing regular file\n";
    out << "  -h, --help | -v, --version\n\n";
    out << "exit codes:\n";
    out << "  0 success   1 usage   2 input/verify error   3 I/O error   4 internal\n";
}

int CLI::run(const Options &options, std::ostream &out, std::ostream &err)
{
    switch (options.command)
    {
    case Command::pack:
        return runPack(options, out, err);
    case Command::list:
        return runList(options, out, err);
    case Command::info:
        return runInfo(options, out, err);
    case Command::verify:
        return runVerify(options, out, err);
    case Command::unpack:
        return runUnpack(options, out, err);
    case Command::none:
    default:
        return 1;
    }
}
