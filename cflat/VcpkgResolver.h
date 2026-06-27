// VcpkgResolver.h - integrates `import package-vcpkg "header" from "port";` with a
// user-maintained vcpkg.json. Discovers the manifest by walking up from the root .cb,
// validates that each imported port is declared, runs `vcpkg install` once per compile,
// and exposes the resolved include dir, link libs, and runtime DLLs from
// vcpkg_installed/<triplet>/.
//
// Header-only to match the rest of the front-end.

#pragma once

#include "platform/PlatformCompat.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 4244 4267 4624 4996)
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#pragma warning(pop)

#include "platform/JsonCompat.h"

class VcpkgResolver
{
public:
    struct Resolution
    {
        std::string includeDir;            // vcpkg_installed/<triplet>/include
        std::vector<std::string> libs;     // every .lib in vcpkg_installed/<triplet>/lib
        std::vector<std::string> dlls;     // every .dll in vcpkg_installed/<triplet>/bin
    };

    void SetExeOverride(const std::string& path)      { exeOverride_ = path; }
    void SetManifestOverride(const std::string& path) { manifestOverride_ = path; }
    void SetTripletOverride(const std::string& t)     { tripletOverride_ = t; }
    void SetPlatform(const std::string& p)            { platform_ = p; }   // "win64" / "win32"
    void SetVerbose(bool v)                           { verbose_ = v; }
    void SetLspMode(bool v)                           { lspMode_ = v; }
    void SetNoInstall(bool v)                         { noInstall_ = v; } // skip `vcpkg install`

    // True when `vcpkg install` is suppressed (LSP mode or --vcpkg-no-install). Lets the
    // caller phrase a more accurate "package not installed" diagnostic.
    bool InstallSuppressed() const { return lspMode_ || noInstall_; }

    // Validate that 'portSpec' (e.g. "curl" or "curl[ssl]") is declared in the manifest.
    // On success, populates 'out' with the include dir / libs / dlls for the resolved
    // <triplet>. The first call per compile may spawn vcpkg.exe; subsequent calls reuse
    // the cached resolution (per process).
    // 'errorMsg' carries a user-facing message on failure; pass it to LogError or print.
    bool Resolve(const std::string& importingFilePath,
                 const std::string& portSpec,
                 Resolution& out,
                 std::string& errorMsg)
    {
        std::string portName = ExtractPortName(portSpec);
        if (portName.empty())
        {
            errorMsg = std::format("import package-vcpkg: empty port name in 'from \"{}\"'.", portSpec);
            return false;
        }

        // Locate (and cache) the manifest.
        if (!manifestResolved_)
        {
            if (!LocateManifest(importingFilePath, errorMsg)) return false;
            manifestResolved_ = true;
        }

        // Verify the port is declared in vcpkg.json.
        if (declaredPorts_.find(portName) == declaredPorts_.end())
        {
            errorMsg = std::format(
                "import package-vcpkg references port '{}', which is not in '{}'.\n"
                "  Run this from '{}':\n"
                "      vcpkg add port {}\n"
                "  Then re-run the cflat command.",
                portName, manifestPath_.string(), manifestDir_.string(), portSpec);
            return false;
        }

        // Resolve the triplet and ensure vcpkg has installed everything (once per compile).
        std::string triplet = ResolveTriplet();
        if (!installRan_)
        {
            if (!RunVcpkgInstall(triplet, errorMsg)) return false;
            installRan_ = true;
        }

        // Build the Resolution from vcpkg_installed/<triplet>/. We deliberately include
        // every .lib / .dll under the triplet: vcpkg installed the transitive closure of
        // the manifest's dependencies, lld-link dedupes and discards unreferenced libs,
        // and copying a few extra DLLs next to the exe is harmless. A precise per-port
        // closure (via spdx) is future work.
        if (!cachedResolution_.has_value())
        {
            Resolution r;
            std::filesystem::path tripletDir = manifestDir_ / "vcpkg_installed" / triplet;
            r.includeDir = (tripletDir / "include").string();

            std::filesystem::path libDir = tripletDir / "lib";
            std::filesystem::path binDir = tripletDir / "bin";
            CollectByExtension(libDir, ".lib", r.libs);
            CollectByExtension(binDir, ".dll", r.dlls);

            cachedResolution_ = std::move(r);
        }

        out = *cachedResolution_;
        return true;
    }

private:
    // Extract the bare port name from "curl" or "curl[ssl,http2]". Empty on error.
    static std::string ExtractPortName(const std::string& spec)
    {
        auto lb = spec.find('[');
        std::string name = (lb == std::string::npos) ? spec : spec.substr(0, lb);
        // Trim trailing whitespace.
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        return name;
    }

    // Walk up from the importing .cb's directory looking for vcpkg.json. First hit wins.
    // 'manifestOverride_' bypasses the walk if non-empty.
    bool LocateManifest(const std::string& importingFilePath, std::string& errorMsg)
    {
        namespace fs = std::filesystem;
        if (!manifestOverride_.empty())
        {
            fs::path p(manifestOverride_);
            std::error_code ec;
            auto canon = fs::canonical(p, ec);
            if (ec || !fs::exists(canon))
            {
                errorMsg = std::format("--vcpkg-manifest path does not exist: {}", manifestOverride_);
                return false;
            }
            manifestPath_ = canon;
            manifestDir_  = canon.parent_path();
        }
        else
        {
            std::error_code ec;
            // Absolutize first so that a bare filename ("get.cb", no directory component)
            // resolves relative to CWD rather than giving an empty parent_path that
            // silently falls back to CWD one level too high.
            fs::path start = fs::absolute(fs::path(importingFilePath), ec).parent_path();
            if (start.empty()) start = fs::current_path(ec);

            // Walk up from 'start' looking for vcpkg.json. Stop at a .git boundary
            // (directory or file - submodules use a .git file) so we don't accidentally
            // pick up some unrelated repo's manifest above the user's project root.
            fs::path here = start;
            fs::path found;
            bool hitGitBoundary = false;
            fs::path gitBoundaryDir;
            for (;;)
            {
                fs::path candidate = here / "vcpkg.json";
                if (fs::exists(candidate, ec))
                {
                    found = candidate;
                    break;
                }
                // .git marks the project root; do not look past it.
                if (fs::exists(here / ".git", ec))
                {
                    hitGitBoundary = true;
                    gitBoundaryDir = here;
                    break;
                }
                fs::path parent = here.parent_path();
                if (parent == here) break;   // reached filesystem root
                here = parent;
            }
            if (found.empty())
            {
                if (hitGitBoundary)
                {
                    errorMsg = std::format(
                        "import package-vcpkg requires a vcpkg.json. None found between '{}' "
                        "and the project root '{}' (stopped at .git boundary).\n"
                        "  Create one with:\n"
                        "      cd \"{}\" && vcpkg new --application",
                        start.string(), gitBoundaryDir.string(), gitBoundaryDir.string());
                }
                else
                {
                    errorMsg = std::format(
                        "import package-vcpkg requires a vcpkg.json. None found walking up from '{}'.\n"
                        "  Create one with:\n"
                        "      vcpkg new --application",
                        start.string());
                }
                return false;
            }
            manifestPath_ = fs::canonical(found, ec);
            manifestDir_  = manifestPath_.parent_path();
        }

        // Parse the manifest and gather declared port names.
        std::ifstream in(manifestPath_);
        if (!in.is_open())
        {
            errorMsg = std::format("failed to open vcpkg.json at '{}'.", manifestPath_.string());
            return false;
        }
        nlohmann::json j;
        try
        {
            in >> j;
        }
        catch (const std::exception& e)
        {
            errorMsg = std::format("failed to parse vcpkg.json: {}", e.what());
            return false;
        }

        auto deps = j.find("dependencies");
        if (deps != j.end() && deps->is_array())
        {
            for (const auto& d : *deps)
            {
                if (d.is_string())
                    declaredPorts_.insert(d.get<std::string>());
                else if (d.is_object())
                {
                    auto name = d.find("name");
                    if (name != d.end() && name->is_string())
                        declaredPorts_.insert(name->get<std::string>());
                }
            }
        }
        return true;
    }

    std::string ResolveTriplet() const
    {
        if (!tripletOverride_.empty()) return tripletOverride_;
        return (platform_ == "win32") ? "x86-windows" : "x64-windows";
    }

    static std::optional<std::string> GetEnv(const char* name)
    {
        char buf[1024] = {};
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof(buf), name) == 0 && len > 0)
            return std::string(buf);
        return std::nullopt;
    }

    // Locate vcpkg.exe. Order: --vcpkg-exe, VCPKG_ROOT, vswhere (VS-bundled), PATH.
    std::string LocateVcpkgExe()
    {
        if (!exeOverride_.empty() && std::filesystem::exists(exeOverride_))
            return exeOverride_;
        if (auto root = GetEnv("VCPKG_ROOT"))
        {
            std::filesystem::path p = std::filesystem::path(*root) / "vcpkg.exe";
            if (std::filesystem::exists(p)) return p.string();
        }
        // VS-bundled: vswhere -> <VSInstall>\VC\vcpkg\vcpkg.exe
        {
            const char* vswhere = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
            if (llvm::sys::fs::exists(vswhere))
            {
                llvm::SmallString<256> outFile;
                llvm::sys::path::system_temp_directory(true, outFile);
                int fd = 0;
                if (!llvm::sys::fs::createTemporaryFile("cflat_vswhere", "txt", fd, outFile))
                {
                    _close(fd);
                    std::string outFileStr = outFile.str().str();
                    std::vector<llvm::StringRef> args = { vswhere, "-latest", "-property", "installationPath" };
                    std::optional<llvm::StringRef> redirects[3] = { std::nullopt, llvm::StringRef(outFileStr), std::nullopt };
                    llvm::sys::ExecuteAndWait(vswhere, args, std::nullopt, redirects);
                    std::string vsPath;
                    if (auto buf = llvm::MemoryBuffer::getFile(outFileStr))
                        vsPath = buf.get()->getBuffer().trim().str();
                    llvm::sys::fs::remove(outFile);
                    if (!vsPath.empty())
                    {
                        std::filesystem::path candidate = std::filesystem::path(vsPath) / "VC" / "vcpkg" / "vcpkg.exe";
                        if (std::filesystem::exists(candidate))
                            return candidate.string();
                    }
                }
            }
        }
        if (auto p = llvm::sys::findProgramByName("vcpkg"))
            return *p;
        return "";
    }

    bool RunVcpkgInstall(const std::string& triplet, std::string& errorMsg)
    {
        // LSP must never block on a network/build subprocess. The first non-LSP compile
        // populates vcpkg_installed/; LSP queries piggy-back on that state. `--vcpkg-no-install`
        // opts a CLI build into the same skip: it consumes an already-installed tree and lets
        // the downstream header-resolution check error out if the package is missing.
        if (InstallSuppressed())
        {
            if (verbose_)
                std::cout << "[verbose] vcpkg: skipping `vcpkg install` ("
                          << (lspMode_ ? "LSP mode" : "--vcpkg-no-install") << ")\n";
            return true;
        }

        // Skip if already installed and the manifest hasn't changed since.
        // Use vcpkg_installed/vcpkg/status as the watermark - vcpkg rewrites it on every
        // successful install. If its mtime is newer than vcpkg.json's, we're up to date.
        std::error_code ec;
        std::filesystem::path statusFile = manifestDir_ / "vcpkg_installed" / "vcpkg" / "status";
        if (std::filesystem::exists(statusFile, ec))
        {
            auto statusMt   = std::filesystem::last_write_time(statusFile, ec);
            auto manifestMt = std::filesystem::last_write_time(manifestPath_, ec);
            if (!ec && statusMt >= manifestMt)
            {
                if (verbose_)
                    std::cout << "[verbose] vcpkg: install state up to date (skipping vcpkg.exe)\n";
                return true;
            }
        }

        std::string vcpkgExe = LocateVcpkgExe();
        if (vcpkgExe.empty())
        {
            errorMsg =
                "vcpkg.exe not found. Install the 'vcpkg package manager' component in "
                "the Visual Studio Installer, or set VCPKG_ROOT, or pass --vcpkg-exe.";
            return false;
        }

        // Machine-wide binary cache so prebuilt packages are shared across cflat projects.
        std::filesystem::path binaryCache;
        if (auto appdata = GetEnv("LOCALAPPDATA"))
            binaryCache = std::filesystem::path(*appdata) / "cflat" / "vcpkg-cache";
        else
            binaryCache = std::filesystem::temp_directory_path(ec) / "cflat-vcpkg-cache";
        std::filesystem::create_directories(binaryCache, ec);
        _putenv_s("VCPKG_DEFAULT_BINARY_CACHE", binaryCache.string().c_str());
        _putenv_s("VCPKG_FEATURE_FLAGS", "manifests,binarycaching");

        // Manifest mode discovers vcpkg.json relative to the current directory. Pin cwd
        // to the manifest dir for the duration of the spawn, then restore.
        std::error_code cwdEc;
        std::filesystem::path prevCwd = std::filesystem::current_path(cwdEc);
        std::filesystem::current_path(manifestDir_, cwdEc);

        std::vector<llvm::StringRef> args = { vcpkgExe, "install", "--triplet", triplet };
        if (verbose_)
        {
            std::cout << std::format("[verbose] vcpkg install --triplet {}\n", triplet);
            std::cout << std::format("[verbose]   manifest dir: {}\n", manifestDir_.string());
        }
        std::string execErr;
        int rc = llvm::sys::ExecuteAndWait(vcpkgExe, args, std::nullopt, {}, 0, 0, &execErr);

        if (!prevCwd.empty()) std::filesystem::current_path(prevCwd, cwdEc);

        if (rc != 0)
        {
            errorMsg = std::format(
                "vcpkg install failed (exit {}) for manifest '{}'{}{}",
                rc, manifestPath_.string(),
                execErr.empty() ? "" : ": ", execErr);
            return false;
        }
        return true;
    }

    static void CollectByExtension(const std::filesystem::path& dir,
                                   const std::string& ext,
                                   std::vector<std::string>& out)
    {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) return;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
        {
            auto& p = it->path();
            if (!it->is_regular_file(ec)) continue;
            auto e = p.extension().string();
            std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (e == ext)
                out.push_back(p.string());
        }
    }

private:
    std::string exeOverride_;
    std::string manifestOverride_;
    std::string tripletOverride_;
    std::string platform_ = "win64";
    bool verbose_ = false;
    bool lspMode_ = false;
    bool noInstall_ = false;

    bool manifestResolved_ = false;
    std::filesystem::path manifestPath_;
    std::filesystem::path manifestDir_;
    std::unordered_set<std::string> declaredPorts_;

    bool installRan_ = false;
    std::optional<Resolution> cachedResolution_;
};
