#include "Sha256.h"
#include "Platform.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
constexpr int kStateSchema = 1;
constexpr uint64_t kMaxManifestBytes = 4 * 1024 * 1024;
constexpr uint64_t kMaxFileBytes = 2ull * 1024 * 1024 * 1024;

struct FileEntry
{
    std::string path;
    uint64_t size = 0;
    std::string sha256;
};

struct Manifest
{
    std::string scope;
    std::string product;
    std::string channel;
    std::string version;
    std::string platform;
    int64_t sequence = 0;
    std::map<std::string, FileEntry> files;
};

struct Slot
{
    fs::path path;
    Manifest manifest;
    std::string manifestHash;
    bool seed = false;
};

struct SlotRef
{
    std::string path;
    std::string version;
    int64_t sequence = 0;
    std::string manifestHash;
};

struct LauncherState
{
    std::string channel = "stable";
    int64_t highestAccepted = -1;
    std::optional<SlotRef> active;
    std::optional<SlotRef> previous;
    std::vector<std::string> failedReleases;
    std::string lastError;
};

enum class Action
{
    Status,
    Run,
    InstallRelease,
    UpdateFrom,
    Rollback
};

struct Options
{
    Action action = Action::Status;
    bool actionSpecified = false;
    bool noUpdate = false;
    fs::path root;
    fs::path feed;
    fs::path manifest;
    fs::path signature;
    fs::path artifact;
    std::string channel = "stable";
    std::vector<std::string> compilerArgs;
};

std::string Trim(std::string value)
{
    size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' || value[first] == '\n'))
        ++first;
    size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r' || value[last - 1] == '\n'))
        --last;
    return value.substr(first, last - first);
}

bool IsHex64(std::string_view value)
{
    if (value.size() != 64)
        return false;
    for (char c : value)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

std::string LowerAscii(std::string value)
{
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

bool IsSafeRelativePath(std::string_view value)
{
    if (value.empty() || value.front() == '/' || value.front() == '\\' || value.find('\\') != std::string_view::npos ||
        value.find(':') != std::string_view::npos || value.find('\0') != std::string_view::npos)
        return false;

    size_t start = 0;
    while (start <= value.size())
    {
        size_t end = value.find('/', start);
        if (end == std::string_view::npos) end = value.size();
        std::string_view part = value.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == value.size()) break;
        start = end + 1;
    }
    return true;
}

bool IsSafeVersion(std::string_view value)
{
    return !value.empty() && value != "." && value != ".." && value.find('/') == std::string_view::npos &&
           value.find('\\') == std::string_view::npos && value.find(':') == std::string_view::npos;
}

bool ParseUnsigned(std::string_view value, uint64_t& result)
{
    if (value.empty()) return false;
    uint64_t parsed = 0;
    for (char c : value)
    {
        if (c < '0' || c > '9') return false;
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    result = parsed;
    return true;
}

bool ParseSigned(std::string_view value, int64_t& result)
{
    if (value.empty()) return false;
    bool negative = value.front() == '-';
    if (negative) value.remove_prefix(1);
    uint64_t unsignedValue = 0;
    if (!ParseUnsigned(value, unsignedValue) || unsignedValue > static_cast<uint64_t>(INT64_MAX))
        return false;
    result = negative ? -static_cast<int64_t>(unsignedValue) : static_cast<int64_t>(unsignedValue);
    return true;
}

bool ReadBytes(const fs::path& path, std::string& bytes, std::string& error)
{
    std::error_code ec;
    uint64_t fileSize = fs::file_size(path, ec);
    if (ec)
    {
        error = "could not stat " + path.string() + ": " + ec.message();
        return false;
    }
    if (fileSize > kMaxManifestBytes)
    {
        error = "metadata file is too large: " + path.string();
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "could not open " + path.string();
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail())
    {
        error = "could not read " + path.string();
        return false;
    }
    return true;
}

bool ParseManifest(const fs::path& path, Manifest& result, std::string& raw, std::string& error)
{
    if (!ReadBytes(path, raw, error)) return false;
    if (raw.empty() || raw.size() > kMaxManifestBytes || raw.front() == '\xef' || raw.find('\r') != std::string::npos)
    {
        error = "manifest must be non-empty, LF-only UTF-8 without a BOM: " + path.string();
        return false;
    }

    std::istringstream lines(raw);
    std::string line;
    bool first = true;
    bool ended = false;
    std::set<std::string> scalarKeys;
    std::set<std::string> foldedPaths;
    while (std::getline(lines, line))
    {
        if (line.empty())
        {
            error = "manifest contains an empty line";
            return false;
        }
        if (first)
        {
            first = false;
            if (line != "cflat-manifest-v1")
            {
                error = "unsupported manifest header";
                return false;
            }
            continue;
        }
        if (line == "end")
        {
            ended = true;
            continue;
        }
        if (ended)
        {
            error = "manifest has data after end";
            return false;
        }

        if (line.starts_with("file="))
        {
            std::istringstream fileLine(line.substr(5));
            FileEntry entry;
            std::string sizeText;
            if (!(fileLine >> entry.path >> sizeText >> entry.sha256) || (fileLine >> std::ws && !fileLine.eof()) ||
                !IsSafeRelativePath(entry.path) || !ParseUnsigned(sizeText, entry.size) || entry.size > kMaxFileBytes ||
                !IsHex64(entry.sha256))
            {
                error = "invalid file entry in manifest";
                return false;
            }
            std::string folded = LowerAscii(entry.path);
            if (!foldedPaths.insert(folded).second || result.files.contains(entry.path))
            {
                error = "duplicate file path in manifest: " + entry.path;
                return false;
            }
            result.files.emplace(entry.path, std::move(entry));
            continue;
        }

        size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            error = "invalid manifest line";
            return false;
        }
        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        if (key != "scope" && key != "product" && key != "channel" && key != "sequence" &&
            key != "version" && key != "platform")
        {
            error = "unknown manifest field: " + key;
            return false;
        }
        if (!scalarKeys.insert(key).second)
        {
            error = "duplicate manifest field: " + key;
            return false;
        }
        if (key == "scope") result.scope = value;
        else if (key == "product") result.product = value;
        else if (key == "channel") result.channel = value;
        else if (key == "version") result.version = value;
        else if (key == "platform") result.platform = value;
        else if (!ParseSigned(value, result.sequence))
        {
            error = "invalid manifest sequence";
            return false;
        }
    }

    if (!ended || result.scope.empty() || result.product.empty() || result.version.empty() || result.files.empty())
    {
        error = "manifest is missing required fields or final end";
        return false;
    }
    if (!result.files.contains("cflat.exe"))
    {
        error = "compiler manifest must list cflat.exe";
        return false;
    }
    return true;
}

bool VerifyDetachedSha256(const fs::path& manifestPath, const fs::path& signaturePath,
                          const std::string& manifestBytes, std::string& manifestHash, std::string& error)
{
    std::string signature;
    if (!ReadBytes(signaturePath, signature, error)) return false;
    std::string algorithm;
    std::string signedHash;
    std::string standinHash;
    std::istringstream lines(signature);
    std::string line;
    bool header = false;
    bool ended = false;
    while (std::getline(lines, line))
    {
        if (line == "cflat-signature-v1") { header = true; continue; }
        if (line == "end") { ended = true; continue; }
        size_t equals = line.find('=');
        if (equals == std::string::npos || ended)
        {
            error = "invalid signature metadata: " + signaturePath.string();
            return false;
        }
        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        if (key == "algorithm") algorithm = value;
        else if (key == "signed_sha256") signedHash = value;
        else if (key == "signature") standinHash = value;
        else if (key != "key_id")
        {
            error = "unknown signature field: " + key;
            return false;
        }
    }
    Sha256 hash;
    hash.Update(manifestBytes);
    manifestHash = Sha256::Hex(hash.Final());
    if (!header || !ended || algorithm != "sha256-standin" || !IsHex64(signedHash) || !IsHex64(standinHash) ||
        LowerAscii(signedHash) != manifestHash || LowerAscii(standinHash) != manifestHash)
    {
        error = "manifest signature stand-in does not match " + manifestPath.string();
        return false;
    }
    return true;
}

bool IsInside(const fs::path& root, const fs::path& candidate)
{
    fs::path normalRoot = root.lexically_normal();
    fs::path normalCandidate = candidate.lexically_normal();
    auto rootIt = normalRoot.begin();
    auto candidateIt = normalCandidate.begin();
    for (; rootIt != normalRoot.end() && candidateIt != normalCandidate.end(); ++rootIt, ++candidateIt)
    {
        if (!launcher_platform::PathComponentEqual(rootIt->string(), candidateIt->string())) return false;
    }
    return rootIt == normalRoot.end();
}

fs::path ResolveRelative(const fs::path& root, std::string_view relative)
{
    fs::path result = root / fs::path(std::string(relative));
    return result.lexically_normal();
}

bool VerifyFile(const fs::path& path, const FileEntry& expected, std::string& error)
{
    std::error_code ec;
    if (fs::is_symlink(path, ec) || ec || !fs::is_regular_file(path, ec) || ec)
    {
        error = "manifest file is not a regular file: " + path.string();
        return false;
    }
    std::array<uint8_t, 32> digest{};
    uint64_t size = 0;
    if (!Sha256::File(path, digest, size, error)) return false;
    std::string actual = Sha256::Hex(digest);
    if (size != expected.size || LowerAscii(actual) != LowerAscii(expected.sha256))
    {
        error = "hash mismatch for " + expected.path;
        return false;
    }
    return true;
}

bool VerifySlot(const fs::path& slotPath, Slot& result, std::string& error, bool allowSeed)
{
    std::error_code ec;
    if (fs::is_symlink(slotPath, ec) || ec || !fs::is_directory(slotPath, ec) || ec)
    {
        error = "compiler slot is not a directory: " + slotPath.string();
        return false;
    }
    fs::path executable = slotPath / "cflat.exe";
    fs::path manifestPath = slotPath / "manifest.cflat";
    fs::path signaturePath = slotPath / "manifest.cflat.sig";
    if (fs::is_symlink(manifestPath, ec) || fs::is_symlink(signaturePath, ec))
    {
        error = "compiler slot metadata must not be symlinks: " + slotPath.string();
        return false;
    }
    if (!fs::exists(manifestPath, ec))
    {
        if (allowSeed && fs::is_regular_file(executable, ec) && !fs::is_symlink(executable, ec))
        {
            result.path = slotPath;
            result.seed = true;
            result.manifest.scope = "compiler-release";
            result.manifest.product = "cflat";
            result.manifest.version = "seeded";
            result.manifest.files.emplace("cflat.exe", FileEntry{ "cflat.exe", fs::file_size(executable, ec), "" });
            return true;
        }
        error = "compiler slot has no manifest: " + slotPath.string();
        return false;
    }

    std::string raw;
    Manifest manifest;
    if (!ParseManifest(manifestPath, manifest, raw, error)) return false;
    std::string manifestHash;
    if (!VerifyDetachedSha256(manifestPath, signaturePath, raw, manifestHash, error)) return false;
    if (manifest.scope != "compiler-release" || manifest.product != "cflat")
    {
        error = "manifest is not a cflat compiler release";
        return false;
    }
    for (const auto& [relative, entry] : manifest.files)
    {
        fs::path filePath = ResolveRelative(slotPath, relative);
        if (!IsInside(slotPath, filePath))
        {
            error = "manifest path escapes compiler slot: " + relative;
            return false;
        }
        if (!VerifyFile(filePath, entry, error)) return false;
    }
    result.path = slotPath;
    result.manifest = std::move(manifest);
    result.manifestHash = std::move(manifestHash);
    result.seed = false;
    return true;
}

bool WriteAtomic(const fs::path& path, std::string_view contents, std::string& error)
{
    fs::path temporary = path;
    temporary += ".new";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "could not write " + temporary.string();
            return false;
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output)
        {
            error = "could not flush " + temporary.string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec)
    {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temporary, path, ec);
    }
    if (ec)
    {
        error = "could not atomically replace " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string RelativePath(const fs::path& root, const fs::path& path)
{
    fs::path relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    std::string value = relative.empty() ? "." : relative.generic_string();
    return value;
}

std::optional<SlotRef> MakeSlotRef(const fs::path& root, const Slot& slot)
{
    SlotRef ref;
    ref.path = RelativePath(root, slot.path);
    ref.version = slot.manifest.version;
    ref.sequence = slot.manifest.sequence;
    ref.manifestHash = slot.manifestHash;
    return ref;
}

std::optional<SlotRef> JsonSlotRef(const json& value)
{
    if (!value.is_object() || !value.contains("path")) return std::nullopt;
    SlotRef ref;
    ref.path = value.value("path", "");
    ref.version = value.value("version", "");
    ref.sequence = value.value("sequence", 0ll);
    ref.manifestHash = value.value("manifest_sha256", "");
    if (ref.path.empty() || (!IsSafeRelativePath(ref.path) && ref.path != ".")) return std::nullopt;
    return ref;
}

json ToJson(const SlotRef& ref)
{
    return json{
        { "version", ref.version },
        { "sequence", ref.sequence },
        { "path", ref.path },
        { "manifest_sha256", ref.manifestHash }
    };
}

bool LoadState(const fs::path& path, LauncherState& result)
{
    std::ifstream input(path);
    if (!input) return false;
    try
    {
        json state;
        input >> state;
        if (state.value("schema", 0) != kStateSchema) return false;
        result.channel = state.value("channel", "stable");
        if (state.contains("highest_accepted") && state["highest_accepted"].is_object())
        {
            const std::string key = "compiler-release:cflat:" + result.channel;
            if (state["highest_accepted"].contains(key) && state["highest_accepted"][key].is_number_integer())
                result.highestAccepted = state["highest_accepted"][key].get<int64_t>();
        }
        if (state.contains("active_compiler") && !state["active_compiler"].is_null())
            result.active = JsonSlotRef(state["active_compiler"]);
        if (state.contains("previous_compiler") && !state["previous_compiler"].is_null())
            result.previous = JsonSlotRef(state["previous_compiler"]);
        if (state.contains("last_error") && state["last_error"].is_string())
            result.lastError = state["last_error"].get<std::string>();
        if (state.contains("failed_releases") && state["failed_releases"].is_array())
            for (const auto& failed : state["failed_releases"])
                if (failed.is_object()) result.failedReleases.push_back(failed.value("version", "unknown"));
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool SaveState(const fs::path& root, const LauncherState& state, std::string& error)
{
    json output{
        { "schema", kStateSchema },
        { "install_root", root.generic_string() },
        { "channel", state.channel },
        { "active_compiler", state.active ? ToJson(*state.active) : json(nullptr) },
        { "previous_compiler", state.previous ? ToJson(*state.previous) : json(nullptr) },
        { "highest_accepted", {{ "compiler-release:cflat:" + state.channel, state.highestAccepted }} },
        { "failed_releases", json::array() },
        { "last_error", state.lastError.empty() ? json(nullptr) : json(state.lastError) }
    };
    for (const auto& version : state.failedReleases)
        output["failed_releases"].push_back({ { "version", version }, { "time_utc", launcher_platform::CurrentUtcTimestamp() } });
    return WriteAtomic(root / "update-state.json", output.dump(2) + "\n", error);
}

bool WriteCurrent(const fs::path& root, const Slot& slot, std::string& error)
{
    return WriteAtomic(root / "current", RelativePath(root, slot.path) + "\n", error);
}

std::optional<fs::path> ReadCurrent(const fs::path& root)
{
    std::string value;
    std::string error;
    if (!ReadBytes(root / "current", value, error)) return std::nullopt;
    value = Trim(value);
    if (value.empty() || (!IsSafeRelativePath(value) && value != ".")) return std::nullopt;
    fs::path path = ResolveRelative(root, value);
    if (!IsInside(root, path)) return std::nullopt;
    return path;
}

std::optional<fs::path> StatePath(const fs::path& root, const std::optional<SlotRef>& ref)
{
    if (!ref) return std::nullopt;
    fs::path path = ResolveRelative(root, ref->path);
    if (!IsInside(root, path)) return std::nullopt;
    return path;
}

bool CopyReleaseFiles(const fs::path& source, const fs::path& destination, const Manifest& manifest, std::string& error)
{
    std::error_code ec;
    for (const auto& [relative, entry] : manifest.files)
    {
        fs::path sourcePath = ResolveRelative(source, relative);
        fs::path destinationPath = ResolveRelative(destination, relative);
        if (!IsInside(source, sourcePath) || !IsInside(destination, destinationPath) ||
            fs::is_symlink(sourcePath, ec) || !fs::is_regular_file(sourcePath, ec))
        {
            error = "release contains an unsafe or missing file: " + relative;
            return false;
        }
        fs::create_directories(destinationPath.parent_path(), ec);
        if (ec || !fs::copy_file(sourcePath, destinationPath, fs::copy_options::overwrite_existing, ec) || ec)
        {
            error = "could not stage " + relative + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options, std::string& error)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string argument = argv[i];
        if (argument == "--")
        {
            for (++i; i < argc; ++i) options.compilerArgs.emplace_back(argv[i]);
            break;
        }
        auto requireValue = [&](const char* name, fs::path& destination) -> bool {
            if (i + 1 >= argc) { error = std::string(name) + " requires a value"; return false; }
            destination = argv[++i];
            return true;
        };
        if (argument == "--status") { options.action = Action::Status; options.actionSpecified = true; }
        else if (argument == "--run") { options.action = Action::Run; options.actionSpecified = true; }
        else if (argument == "--no-update") { options.action = Action::Run; options.noUpdate = true; options.actionSpecified = true; }
        else if (argument == "--rollback") { options.action = Action::Rollback; options.actionSpecified = true; }
        else if (argument == "--root")
        {
            if (!requireValue("--root", options.root)) return false;
        }
        else if (argument == "--channel")
        {
            if (i + 1 >= argc) { error = "--channel requires a value"; return false; }
            options.channel = argv[++i];
        }
        else if (argument == "--update-from")
        {
            if (!requireValue("--update-from", options.feed)) return false;
            options.action = Action::UpdateFrom;
            options.actionSpecified = true;
        }
        else if (argument == "--install-release")
        {
            if (i + 3 >= argc) { error = "--install-release requires manifest, signature, and artifact"; return false; }
            options.action = Action::InstallRelease;
            options.actionSpecified = true;
            options.manifest = argv[++i];
            options.signature = argv[++i];
            options.artifact = argv[++i];
        }
        else if (argument == "--help" || argument == "-h")
        {
            std::cout << "Usage: cflat-launcher [--root DIR] <command>\n"
                         "\nCommands:\n"
                         "  --status\n"
                         "  --run -- <cflat arguments>\n"
                         "  --no-update -- <cflat arguments>\n"
                         "  --update-from FEED\n"
                         "  --install-release MANIFEST SIGNATURE ARTIFACT_DIR\n"
                         "  --rollback\n";
            return false;
        }
        else
        {
            error = "unknown launcher argument: " + argument;
            return false;
        }
    }
    if (!options.actionSpecified) options.action = Action::Status;
    if ((options.action == Action::Run) && options.compilerArgs.empty())
    {
        error = "run commands require '--' followed by cflat arguments";
        return false;
    }
    return true;
}

class Launcher
{
public:
    Launcher(fs::path root, std::string channel)
        : root_(std::move(root)), channel_(std::move(channel)), statePath_(root_ / "update-state.json")
    {
    }

    int Status()
    {
        LauncherState state;
        bool stateLoaded = LoadState(statePath_, state);
        if (!stateLoaded) state.channel = channel_;
        std::cout << "install_root: " << root_.string() << "\n";
        std::cout << "channel: " << state.channel << "\n";
        std::string error;
        auto active = ResolveActive(state);
        if (active)
        {
            Slot slot;
            bool verified = VerifySlot(*active, slot, error, true);
            std::cout << "active_compiler: " << active->string() << "\n";
            std::cout << "active_version: " << (verified ? slot.manifest.version : "invalid") << "\n";
            std::cout << "active_verified: " << (verified ? "yes" : "no") << "\n";
            if (!verified) std::cout << "last_verify_error: " << error << "\n";
        }
        else
            std::cout << "active_compiler: (none)\n";
        std::cout << "previous_compiler: " << (state.previous ? state.previous->path : "(none)") << "\n";
        std::cout << "failed_releases: " << state.failedReleases.size() << "\n";
        std::cout << "last_error: " << (state.lastError.empty() ? "(none)" : state.lastError) << "\n";
        return 0;
    }

    int Run(const std::vector<std::string>& arguments)
    {
        LauncherState state;
        LoadOrSeedState(state);
        if (state.active)
        {
            std::string stateError;
            if (!SaveState(root_, state, stateError))
                std::cerr << "launcher: could not persist launcher state: " << stateError << "\n";
        }
        launcher_platform::FileLock lock;
        if (!lock.TryAcquire(root_ / "update-lock"))
            std::cerr << "launcher: update lock is busy; running the current slot without update work\n";

        std::string error;
        auto activePath = ResolveActive(state);
        Slot active;
        if (!activePath || !VerifySlot(*activePath, active, error, true))
        {
            if (state.previous)
            {
                auto previousPath = StatePath(root_, state.previous);
                Slot previous;
                std::string previousError;
                if (previousPath && VerifySlot(*previousPath, previous, previousError, false))
                {
                    std::cerr << "launcher: active compiler failed verification; falling back to "
                              << previous.manifest.version << "\n";
                    if (!Activate(previous, active, state, error))
                    {
                        std::cerr << "launcher: could not persist fallback: " << error << "\n";
                        return 1;
                    }
                    active = std::move(previous);
                }
                else
                {
                    std::cerr << "launcher: active compiler is invalid: " << error << "\n";
                    std::cerr << "launcher: previous compiler is also invalid: " << previousError << "\n";
                    return 1;
                }
            }
            else
            {
                std::cerr << "launcher: active compiler is invalid: " << error << "\n";
                return 1;
            }
        }

        // Reverify immediately before CreateProcess. This closes the normal launcher path
        // against edits between status/update verification and the actual compiler launch.
        Slot finalCheck;
        if (!VerifySlot(active.path, finalCheck, error, active.seed))
        {
            std::cerr << "launcher: compiler changed before launch: " << error << "\n";
            return 1;
        }
        fs::path executable = active.path / "cflat.exe";
        int exitCode = launcher_platform::RunProcess(executable, arguments, active.path, error);
        if (exitCode < 0)
        {
            std::cerr << "launcher: " << error << "\n";
            return 1;
        }
        return exitCode;
    }

    int InstallRelease(const fs::path& manifestPath, const fs::path& signaturePath, const fs::path& artifactPath)
    {
        LauncherState state;
        LoadOrSeedState(state);
        launcher_platform::FileLock lock;
        if (!lock.TryAcquire(root_ / "update-lock"))
        {
            std::cerr << "launcher: another update is already in progress\n";
            return 1;
        }

        Manifest manifest;
        std::string raw;
        std::string error;
        if (!ParseManifest(manifestPath, manifest, raw, error)) return Fail(state, error);
        std::string manifestHash;
        if (!VerifyDetachedSha256(manifestPath, signaturePath, raw, manifestHash, error)) return Fail(state, error);
        if (manifest.scope != "compiler-release" || manifest.product != "cflat" || !IsSafeVersion(manifest.version))
            return Fail(state, "release manifest has an invalid scope, product, or version");
        if (state.highestAccepted >= 0 && manifest.sequence <= state.highestAccepted)
            return Fail(state, "release sequence is not newer than the highest accepted sequence");
        if (!fs::is_directory(artifactPath))
            return Fail(state, "local prototype requires an unpacked artifact directory: " + artifactPath.string());

        fs::path releases = root_ / "releases" / "compiler";
        fs::create_directories(releases, errorCode_);
        if (errorCode_)
            return Fail(state, "could not create release directory: " + errorCode_.message());
        fs::path staging = releases / (".staging-" + manifest.version + "-" + TimestampForPath());
        fs::create_directories(staging, errorCode_);
        if (errorCode_)
            return Fail(state, "could not create staging directory: " + errorCode_.message());

        if (!CopyReleaseFiles(artifactPath, staging, manifest, error))
            return Quarantine(state, staging, manifest.version, error);
        if (!CopyMetadata(manifestPath, signaturePath, staging, error))
            return Quarantine(state, staging, manifest.version, error);

        Slot staged;
        if (!VerifySlot(staging, staged, error, false))
            return Quarantine(state, staging, manifest.version, error);
        std::string processError;
        int initResult = launcher_platform::RunProcess(staging / "cflat.exe", { "--init-local" }, staging, processError);
        if (initResult != 0)
            return Quarantine(state, staging, manifest.version,
                              "--init-local failed with exit code " + std::to_string(initResult) +
                              (processError.empty() ? "" : ": " + processError));
        int smokeResult = launcher_platform::RunProcess(staging / "cflat.exe", { "--version" }, staging, processError);
        if (smokeResult != 0)
            return Quarantine(state, staging, manifest.version,
                              "compiler smoke check failed with exit code " + std::to_string(smokeResult));

        fs::path finalPath = releases / manifest.version;
        if (fs::exists(finalPath))
            return Quarantine(state, staging, manifest.version, "release slot already exists: " + finalPath.string());
        fs::rename(staging, finalPath, errorCode_);
        if (errorCode_)
            return Quarantine(state, staging, manifest.version, "could not activate staged directory: " + errorCode_.message());

        Slot installed;
        if (!VerifySlot(finalPath, installed, error, false))
            return Fail(state, "installed release failed final verification: " + error);
        Slot oldActive;
        auto oldPath = ResolveActive(state);
        bool hadOldActive = oldPath && VerifySlot(*oldPath, oldActive, error, true);
        if (!Activate(installed, hadOldActive ? std::optional<Slot>(oldActive) : std::nullopt, state, error))
            return Fail(state, error);
        std::cout << "installed and activated compiler " << manifest.version << "\n";
        return 0;
    }

    int UpdateFrom(const fs::path& feed, const std::string& channel)
    {
        fs::path channelPath = feed / "compiler" / channel / "channel.cflat";
        fs::path signaturePath = feed / "compiler" / channel / "channel.cflat.sig";
        std::string raw;
        std::string error;
        if (!ReadBytes(channelPath, raw, error)) { std::cerr << "launcher: " << error << "\n"; return 1; }
        std::string hash;
        if (!VerifyDetachedSha256(channelPath, signaturePath, raw, hash, error))
        {
            std::cerr << "launcher: channel verification failed: " << error << "\n";
            return 1;
        }
        std::map<std::string, std::string> values;
        std::istringstream lines(raw);
        std::string line;
        while (std::getline(lines, line))
        {
            size_t equals = line.find('=');
            if (equals != std::string::npos) values[line.substr(0, equals)] = line.substr(equals + 1);
        }
        if (values["scope"] != "compiler-channel" || values["product"] != "cflat" || values["channel"] != channel ||
            !values.contains("manifest_path") || !values.contains("signature_path") || !values.contains("artifact_path") ||
            !IsSafeRelativePath(values["manifest_path"]) || !IsSafeRelativePath(values["signature_path"]) ||
            !IsSafeRelativePath(values["artifact_path"]))
        {
            std::cerr << "launcher: invalid channel metadata\n";
            return 1;
        }
        fs::path channelRoot = channelPath.parent_path();
        return InstallRelease(channelRoot / fs::path(values["manifest_path"]),
                              channelRoot / fs::path(values["signature_path"]),
                              channelRoot / fs::path(values["artifact_path"]));
    }

    int Rollback()
    {
        LauncherState state;
        LoadOrSeedState(state);
        launcher_platform::FileLock lock;
        if (!lock.TryAcquire(root_ / "update-lock"))
        {
            std::cerr << "launcher: another update is already in progress\n";
            return 1;
        }
        if (!state.previous)
        {
            std::cerr << "launcher: no previous compiler is available\n";
            return 1;
        }
        auto previousPath = StatePath(root_, state.previous);
        Slot previous;
        std::string error;
        if (!previousPath || !VerifySlot(*previousPath, previous, error, false))
        {
            std::cerr << "launcher: previous compiler failed verification: " << error << "\n";
            return 1;
        }
        std::optional<Slot> oldActive;
        if (auto activePath = ResolveActive(state))
        {
            Slot active;
            if (VerifySlot(*activePath, active, error, true)) oldActive = std::move(active);
        }
        if (!Activate(previous, oldActive, state, error))
        {
            std::cerr << "launcher: rollback failed: " << error << "\n";
            return 1;
        }
        std::cout << "rolled back to " << previous.manifest.version << "\n";
        return 0;
    }

private:
    void LoadOrSeedState(LauncherState& state)
    {
        if (!LoadState(statePath_, state)) state = LauncherState{};
        if (!state.active)
        {
            auto current = ReadCurrent(root_);
            if (current)
            {
                Slot slot;
                std::string error;
                if (VerifySlot(*current, slot, error, true)) state.active = MakeSlotRef(root_, slot);
            }
        }
        if (!state.active && fs::is_regular_file(root_ / "cflat.exe"))
        {
            Slot seed;
            std::string error;
            if (VerifySlot(root_, seed, error, true)) state.active = MakeSlotRef(root_, seed);
        }
        if (state.channel.empty()) state.channel = channel_;
    }

    std::optional<fs::path> ResolveActive(const LauncherState& state)
    {
        if (auto path = StatePath(root_, state.active)) return path;
        if (auto current = ReadCurrent(root_)) return current;
        if (fs::is_regular_file(root_ / "cflat.exe")) return root_;
        return std::nullopt;
    }

    bool CopyMetadata(const fs::path& manifest, const fs::path& signature, const fs::path& destination, std::string& error)
    {
        std::error_code ec;
        fs::copy_file(manifest, destination / "manifest.cflat", fs::copy_options::overwrite_existing, ec);
        if (ec) { error = "could not copy manifest: " + ec.message(); return false; }
        fs::copy_file(signature, destination / "manifest.cflat.sig", fs::copy_options::overwrite_existing, ec);
        if (ec) { error = "could not copy signature metadata: " + ec.message(); return false; }
        return true;
    }

    bool Activate(const Slot& next, const std::optional<Slot>& oldActive, LauncherState& state, std::string& error)
    {
        if (!WriteCurrent(root_, next, error)) return false;
        state.previous = oldActive ? MakeSlotRef(root_, *oldActive) : std::nullopt;
        state.active = MakeSlotRef(root_, next);
        state.highestAccepted = std::max(state.highestAccepted, next.manifest.sequence);
        state.channel = channel_;
        state.lastError.clear();
        return SaveState(root_, state, error);
    }

    int Fail(LauncherState& state, const std::string& error)
    {
        state.lastError = error;
        std::string saveError;
        SaveState(root_, state, saveError);
        std::cerr << "launcher: " << error << "\n";
        if (!saveError.empty()) std::cerr << "launcher: could not save state: " << saveError << "\n";
        return 1;
    }

    int Quarantine(LauncherState& state, const fs::path& staging, const std::string& version, const std::string& error)
    {
        fs::path quarantine = root_ / "releases" / "compiler" / "quarantine";
        std::error_code ec;
        fs::create_directories(quarantine, ec);
        fs::path destination = quarantine / (version + "-" + TimestampForPath());
        fs::rename(staging, destination, ec);
        if (ec) std::cerr << "launcher: could not quarantine staging directory: " << ec.message() << "\n";
        state.failedReleases.push_back(version);
        return Fail(state, error);
    }

    static std::string TimestampForPath()
    {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return std::to_string(now);
    }

    fs::path root_;
    std::string channel_;
    fs::path statePath_;
    std::error_code errorCode_;
};
}

int main(int argc, char** argv)
{
    Options options;
    std::string error;
    if (!ParseOptions(argc, argv, options, error))
    {
        if (!error.empty()) std::cerr << "launcher: " << error << "\n";
        return error.empty() ? 0 : 1;
    }

    std::string platformError;
    auto executablePath = launcher_platform::ExecutablePath(platformError);
    if (!executablePath)
    {
        std::cerr << "launcher: " << platformError << "\n";
        return 1;
    }
    std::error_code ec;
    fs::path root = options.root.empty() ? executablePath->parent_path() : options.root;
    root = fs::absolute(root, ec).lexically_normal();
    if (ec)
    {
        std::cerr << "launcher: invalid install root: " << ec.message() << "\n";
        return 1;
    }
    fs::create_directories(root, ec);
    if (ec)
    {
        std::cerr << "launcher: could not create install root: " << ec.message() << "\n";
        return 1;
    }

    Launcher launcher(root, options.channel);
    switch (options.action)
    {
        case Action::Status: return launcher.Status();
        case Action::Run: return launcher.Run(options.compilerArgs);
        case Action::InstallRelease: return launcher.InstallRelease(options.manifest, options.signature, options.artifact);
        case Action::UpdateFrom: return launcher.UpdateFrom(options.feed, options.channel);
        case Action::Rollback: return launcher.Rollback();
    }
    return 1;
}
