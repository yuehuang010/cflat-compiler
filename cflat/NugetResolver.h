// NugetResolver.h - integrates `import package-nuget "file" from "id[/version]";`.
// Locates the package in the NuGet global packages folder (NUGET_PACKAGES env ->
// %USERPROFILE%/.nuget/packages), acquires it on a cache miss (synthesized-project
// `dotnet restore` when dotnet is on PATH; direct flatcontainer download via
// curl.exe + tar.exe as fallback), then extracts include dirs / winmd dirs / libs /
// runtime DLLs by evaluating the package's build/native/<id>.targets (+ .props)
// through MsbuildLite, with a directory-convention probe as fallback.
//
// Design and layout survey: internal/plan/package-nuget-import.md.
// Header-only, mirrors VcpkgResolver.h in shape and conventions.

#pragma once

#include "platform/PlatformCompat.h"

#include "MsbuildLite.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 4244 4267 4624 4996)
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#pragma warning(pop)

class NugetResolver
{
public:
    struct Resolution
    {
        std::vector<std::string> includeDirs;  // -I roots for the C header binding path
        std::vector<std::string> winmdDirs;    // dirs to search for the imported .winmd
        std::vector<std::string> libs;         // absolute .lib paths to link
        std::vector<std::string> dlls;         // runtime DLLs to deploy next to the exe
    };

    void SetPackagesDirOverride(const std::string& d) { packagesDirOverride_ = d; }  // --nuget-packages-dir
    void SetPlatform(const std::string& p)            { platform_ = p; }  // "win64" / "win32"
    void SetVerbose(bool v)                           { verbose_ = v; }
    void SetLspMode(bool v)                           { lspMode_ = v; }
    void SetNoInstall(bool v)                         { noInstall_ = v; }  // --nuget-no-install

    // True when package acquisition is suppressed (LSP mode or --nuget-no-install).
    // Lets the caller phrase a more accurate "package not resolved" diagnostic.
    bool InstallSuppressed() const { return lspMode_ || noInstall_; }

    // Resolve 'packageSpec' ("Id" or "Id/1.2.3"): locate in the packages folder,
    // acquire on miss (unless suppressed), and populate 'out' from the package's
    // .props/.targets (MsbuildLite) or the directory-convention fallback.
    // Unpinned specs resolve to the highest version already in the cache and error
    // with a pin suggestion when absent - never silently float to nuget.org latest.
    // Cached per id/version for the life of the process.
    // 'errorMsg' carries a user-facing message on failure; pass it to LogError.
    bool Resolve(const std::string& packageSpec,
                 Resolution& out,
                 std::string& errorMsg)
    {
        // Parse "Id" or "Id/Version" (single '/'). Empty id is an error.
        std::string id, version;
        if (!SplitSpec(packageSpec, id, version, errorMsg))
            return false;
        std::string idLower = ToLower(id);

        // Locate the packages root (override -> NUGET_PACKAGES -> %USERPROFILE%\.nuget\packages).
        std::filesystem::path root;
        if (!PackagesRoot(root, errorMsg))
            return false;
        if (verbose_)
            std::cout << std::format("[verbose] nuget: packages folder '{}'\n", root.string());

        std::filesystem::path idDir = root / idLower;

        // Resolve the concrete version + package folder.
        std::string resolvedVersion;
        std::filesystem::path pkgFolder;
        if (!version.empty())
        {
            // Pinned: try the folder directly (literal, then leading-zero-stripped).
            if (!LocatePinnedFolder(idDir, version, pkgFolder, resolvedVersion))
            {
                if (InstallSuppressed())
                {
                    errorMsg = std::format(
                        "import package-nuget: package '{}/{}' is not in the NuGet packages folder "
                        "'{}', and acquisition is disabled ({}).\n"
                        "  Restore it once (e.g. `dotnet restore` a project that references it, or "
                        "re-run without --nuget-no-install), then re-run the cflat command.",
                        id, version, root.string(), lspMode_ ? "LSP mode" : "--nuget-no-install");
                    return false;
                }
                // Acquire the pinned version, then re-locate.
                if (!Acquire(root, id, idLower, version, errorMsg))
                    return false;
                if (!LocatePinnedFolder(idDir, version, pkgFolder, resolvedVersion))
                {
                    errorMsg = std::format(
                        "import package-nuget: acquired '{}/{}' but its folder did not appear under "
                        "'{}'. The download/extract may have failed silently.",
                        id, version, idDir.string());
                    return false;
                }
            }
        }
        else
        {
            // Unpinned: pick the highest version already cached. Never download 'latest'.
            std::vector<std::string> cached = EnumerateCachedVersions(idDir);
            if (cached.empty())
            {
                errorMsg = std::format(
                    "import package-nuget: package '{}' has no version in the NuGet packages folder "
                    "'{}'.\n"
                    "  Pin a version so the build stays reproducible, e.g.:\n"
                    "      import package-nuget \"<file>\" from \"{}/<version>\";\n"
                    "  Find available versions at https://www.nuget.org/packages/{}\n"
                    "  (cflat never downloads an unpinned 'latest').",
                    id, root.string(), id, id);
                return false;
            }
            resolvedVersion = PickHighestVersion(cached);
            pkgFolder = idDir / resolvedVersion;
            if (verbose_)
                std::cout << std::format("[verbose] nuget: unpinned '{}' resolved to cached version {}\n",
                    id, resolvedVersion);
        }

        std::error_code ec;
        if (!std::filesystem::exists(pkgFolder, ec))
        {
            errorMsg = std::format("import package-nuget: package folder does not exist: {}", pkgFolder.string());
            return false;
        }

        // Per-process cache keyed on "<id-lower>/<version>".
        std::string cacheKey = idLower + "/" + resolvedVersion;
        auto cachedIt = cache_.find(cacheKey);
        if (cachedIt != cache_.end())
        {
            out = cachedIt->second;
            return true;
        }

        Resolution r;
        BuildResolution(pkgFolder, id, root, r);
        cache_[cacheKey] = r;
        out = std::move(r);
        return true;
    }

private:
    std::string packagesDirOverride_;
    std::string platform_ = "win64";
    bool verbose_   = false;
    bool lspMode_   = false;
    bool noInstall_ = false;

    // Per-process resolution cache, keyed on "<id-lower>/<version>".
    std::unordered_map<std::string, Resolution> cache_;

    // ---------------------------------------------------------------
    // Spec / environment / path helpers
    // ---------------------------------------------------------------
    static std::string ToLower(std::string s)
    {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // "x64" for win64, "x86" for win32 - the native NuGet arch folder name.
    std::string Arch() const { return (platform_ == "win32") ? "x86" : "x64"; }

    // Split "Id" or "Id/Version" on the single '/'. Empty id -> error.
    static bool SplitSpec(const std::string& spec, std::string& id, std::string& version, std::string& errorMsg)
    {
        std::string s = spec;
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();

        auto slash = s.find('/');
        if (slash == std::string::npos)
        {
            id = s;
            version.clear();
        }
        else
        {
            id = s.substr(0, slash);
            version = s.substr(slash + 1);
        }
        if (id.empty())
        {
            errorMsg = std::format("import package-nuget: empty package id in 'from \"{}\"'.", spec);
            return false;
        }
        return true;
    }

    static std::optional<std::string> GetEnv(const char* name)
    {
        char buf[4096] = {};
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof(buf), name) == 0 && len > 0)
            return std::string(buf);
        return std::nullopt;
    }

    // Packages root: --nuget-packages-dir -> NUGET_PACKAGES -> %USERPROFILE%\.nuget\packages.
    bool PackagesRoot(std::filesystem::path& out, std::string& errorMsg) const
    {
        if (!packagesDirOverride_.empty())
        {
            out = std::filesystem::path(packagesDirOverride_);
            return true;
        }
        if (auto np = GetEnv("NUGET_PACKAGES"))
        {
            out = std::filesystem::path(*np);
            return true;
        }
        // USERPROFILE on Windows, HOME on POSIX - NuGet uses ~/.nuget/packages on both.
        for (const char* home : { "USERPROFILE", "HOME" })
        {
            if (auto up = GetEnv(home))
            {
                out = std::filesystem::path(*up) / ".nuget" / "packages";
                return true;
            }
        }
        errorMsg =
            "import package-nuget: cannot locate the NuGet packages folder (USERPROFILE/HOME unset). "
            "Set NUGET_PACKAGES or pass --nuget-packages-dir.";
        return false;
    }

    // NuGet folder names are lowercase and version-normalized (no leading zeros per
    // segment). Try the literal lowercase version first, then a leading-zero-stripped
    // variant. First existing folder wins.
    static bool LocatePinnedFolder(const std::filesystem::path& idDir,
                                   const std::string& version,
                                   std::filesystem::path& outFolder,
                                   std::string& outVersion)
    {
        std::error_code ec;
        std::string vLower = ToLower(version);
        std::string candidates[2] = { vLower, StripLeadingZeros(vLower) };
        for (const auto& cand : candidates)
        {
            if (cand.empty()) continue;
            std::filesystem::path p = idDir / cand;
            if (std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec))
            {
                outFolder = p;
                outVersion = cand;
                return true;
            }
        }
        return false;
    }

    // Strip leading zeros from each dot-separated numeric segment of the core version
    // (the part before any '-prerelease' suffix). "1.02.0003" -> "1.2.3".
    static std::string StripLeadingZeros(const std::string& version)
    {
        auto dash = version.find('-');
        std::string core = (dash == std::string::npos) ? version : version.substr(0, dash);
        std::string suffix = (dash == std::string::npos) ? "" : version.substr(dash);

        std::string out;
        std::stringstream ss(core);
        std::string seg;
        bool first = true;
        while (std::getline(ss, seg, '.'))
        {
            if (!first) out += '.';
            first = false;
            bool numeric = !seg.empty() && std::all_of(seg.begin(), seg.end(),
                [](unsigned char c) { return std::isdigit(c) != 0; });
            if (numeric)
            {
                size_t nz = seg.find_first_not_of('0');
                out += (nz == std::string::npos) ? "0" : seg.substr(nz);
            }
            else
                out += seg;
        }
        return out + suffix;
    }

    // All immediate subdirectory names of idDir (each is a cached version string).
    static std::vector<std::string> EnumerateCachedVersions(const std::filesystem::path& idDir)
    {
        std::vector<std::string> out;
        std::error_code ec;
        if (!std::filesystem::exists(idDir, ec)) return out;
        for (auto it = std::filesystem::directory_iterator(idDir, ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
        {
            if (it->is_directory(ec))
                out.push_back(it->path().filename().string());
        }
        return out;
    }

    static std::string PickHighestVersion(const std::vector<std::string>& versions)
    {
        std::string best = versions.front();
        for (size_t i = 1; i < versions.size(); i++)
            if (CompareVersions(versions[i], best) > 0) best = versions[i];
        return best;
    }

    // Segment-wise numeric compare of the core version; a prerelease sorts below the
    // matching release. Returns <0, 0, >0.
    static int CompareVersions(const std::string& a, const std::string& b)
    {
        std::string aCore, aPre, bCore, bPre;
        SplitPrerelease(a, aCore, aPre);
        SplitPrerelease(b, bCore, bPre);

        std::vector<long long> as = NumericSegments(aCore), bs = NumericSegments(bCore);
        size_t n = std::max(as.size(), bs.size());
        for (size_t i = 0; i < n; i++)
        {
            long long av = i < as.size() ? as[i] : 0;
            long long bv = i < bs.size() ? bs[i] : 0;
            if (av != bv) return av < bv ? -1 : 1;
        }
        // Cores equal: no-prerelease outranks prerelease; else lexicographic on suffix.
        bool aRel = aPre.empty(), bRel = bPre.empty();
        if (aRel != bRel) return aRel ? 1 : -1;
        if (aPre != bPre) return aPre < bPre ? -1 : 1;
        return 0;
    }

    static void SplitPrerelease(const std::string& v, std::string& core, std::string& pre)
    {
        auto dash = v.find('-');
        if (dash == std::string::npos) { core = v; pre.clear(); }
        else { core = v.substr(0, dash); pre = v.substr(dash + 1); }
    }

    static std::vector<long long> NumericSegments(const std::string& core)
    {
        std::vector<long long> out;
        std::stringstream ss(core);
        std::string seg;
        while (std::getline(ss, seg, '.'))
        {
            long long v = 0;
            try { v = seg.empty() ? 0 : std::stoll(seg); } catch (...) { v = 0; }
            out.push_back(v);
        }
        return out;
    }

    // ---------------------------------------------------------------
    // Acquisition (pinned miss, not suppressed)
    // ---------------------------------------------------------------
    // Prefer `dotnet restore` of a synthesized project; fall back to a direct
    // flatcontainer download via curl.exe + tar.exe. Both land the extracted tree in
    // the resolved packages root so real NuGet tooling recognizes it.
    bool Acquire(const std::filesystem::path& root,
                 const std::string& id,
                 const std::string& idLower,
                 const std::string& version,
                 std::string& errorMsg)
    {
        if (verbose_)
            std::cout << std::format("[verbose] nuget: '{}/{}' not cached; acquiring\n", id, version);

        std::string dotnetNote;
        if (auto dotnet = llvm::sys::findProgramByName("dotnet"))
        {
            if (AcquireViaDotnet(*dotnet, root, id, version, dotnetNote))
                return true;
            if (verbose_)
                std::cout << std::format("[verbose] nuget: dotnet restore did not produce the package ({}); "
                                         "falling back to direct download\n",
                    dotnetNote.empty() ? "see output above" : dotnetNote);
        }
        else if (verbose_)
            std::cout << "[verbose] nuget: 'dotnet' not on PATH; using direct download\n";

        return AcquireViaCurl(root, idLower, version, errorMsg);
    }

    // Synthesize a minimal net8.0 project referencing the package and `dotnet restore`
    // it into the resolved packages root. Native packages have no net8 asset (restore
    // warns, not fails); the download still lands in the global folder.
    bool AcquireViaDotnet(const std::string& dotnet,
                          const std::filesystem::path& root,
                          const std::string& id,
                          const std::string& version,
                          std::string& note)
    {
        llvm::SmallString<256> tmpDir;
        if (llvm::sys::fs::createUniqueDirectory("cflat-nuget", tmpDir))
        {
            note = "could not create a temp directory";
            return false;
        }
        std::filesystem::path projDir(tmpDir.str().str());
        std::filesystem::path projPath = projDir / "cflat_nuget_restore.csproj";

        // Plain Microsoft.NET.Sdk + net8.0; RestorePackagesPath is left unset so the
        // package installs into the (resolved) global packages folder via --packages.
        {
            std::ofstream f(projPath, std::ios::binary);
            f << "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
              << "  <PropertyGroup>\n"
              << "    <TargetFramework>net8.0</TargetFramework>\n"
              << "    <DisableImplicitNuGetFallbackFolder>true</DisableImplicitNuGetFallbackFolder>\n"
              << "  </PropertyGroup>\n"
              << "  <ItemGroup>\n"
              << "    <PackageReference Include=\"" << id << "\" Version=\"" << version << "\" />\n"
              << "  </ItemGroup>\n"
              << "</Project>\n";
        }

        std::string projStr = projPath.string();
        std::string rootStr = root.string();
        std::vector<llvm::StringRef> args = {
            dotnet, "restore", projStr, "--packages", rootStr, "--verbosity", "quiet"
        };
        if (verbose_)
            std::cout << std::format("[verbose] nuget: dotnet restore {} --packages {}\n", projStr, rootStr);

        std::string execErr;
        int rc = llvm::sys::ExecuteAndWait(dotnet, args, std::nullopt, {}, 0, 0, &execErr);

        std::error_code rmEc;
        std::filesystem::remove_all(projDir, rmEc);

        if (rc != 0)
        {
            note = execErr.empty() ? std::format("exit {}", rc) : execErr;
            return false;
        }
        // Verify the package folder now exists (restore of a native pkg can 'succeed'
        // with only warnings; the download is what we care about).
        std::filesystem::path idDir = root / ToLower(id);
        std::filesystem::path folder;
        std::string resolved;
        if (!LocatePinnedFolder(idDir, version, folder, resolved))
        {
            note = "restore succeeded but the package folder was not created";
            return false;
        }
        return true;
    }

    // Direct NuGet v3 flatcontainer download: GET the .nupkg (a plain zip) with curl,
    // extract with tar (bsdtar reads zip), and mimic the NuGet folder layout enough for
    // tooling to tolerate it.
    bool AcquireViaCurl(const std::filesystem::path& root,
                        const std::string& idLower,
                        const std::string& version,
                        std::string& errorMsg)
    {
        std::string verLower = ToLower(version);
        std::string curl = FindSystemTool("curl.exe");
        std::string tar  = FindSystemTool("tar.exe");
        if (curl.empty() || tar.empty())
        {
            errorMsg = std::format(
                "import package-nuget: cannot acquire '{}/{}' - {} not found. Install .NET "
                "(so 'dotnet' is on PATH) or ensure curl.exe and tar.exe are available.",
                idLower, version, curl.empty() ? "curl.exe" : "tar.exe");
            return false;
        }

        std::string url = std::format(
            "https://api.nuget.org/v3-flatcontainer/{}/{}/{}.{}.nupkg",
            idLower, verLower, idLower, verLower);

        llvm::SmallString<256> tmpDir;
        if (llvm::sys::fs::createUniqueDirectory("cflat-nuget-dl", tmpDir))
        {
            errorMsg = "import package-nuget: could not create a temp directory for download.";
            return false;
        }
        std::filesystem::path tmp(tmpDir.str().str());
        std::filesystem::path nupkgTmp = tmp / (idLower + "." + verLower + ".nupkg");

        if (verbose_)
            std::cout << std::format("[verbose] nuget: GET {}\n", url);

        std::string nupkgTmpStr = nupkgTmp.string();
        std::vector<llvm::StringRef> dlArgs = { curl, "-sSL", "-f", "-o", nupkgTmpStr, url };
        std::string execErr;
        int rc = llvm::sys::ExecuteAndWait(curl, dlArgs, std::nullopt, {}, 0, 0, &execErr);
        std::error_code ec;
        if (rc != 0 || !std::filesystem::exists(nupkgTmp, ec))
        {
            std::filesystem::remove_all(tmp, ec);
            errorMsg = std::format(
                "import package-nuget: download failed for '{}/{}' (curl exit {}). Check the "
                "id/version at https://www.nuget.org/packages/{} and that the network is reachable.",
                idLower, version, rc, idLower);
            return false;
        }

        // Extract into the final package folder.
        std::filesystem::path pkgFolder = root / idLower / verLower;
        std::filesystem::create_directories(pkgFolder, ec);
        std::string pkgFolderStr = pkgFolder.string();
        std::vector<llvm::StringRef> exArgs = { tar, "-xf", nupkgTmpStr, "-C", pkgFolderStr };
        rc = llvm::sys::ExecuteAndWait(tar, exArgs, std::nullopt, {}, 0, 0, &execErr);
        if (rc != 0)
        {
            std::filesystem::remove_all(tmp, ec);
            errorMsg = std::format(
                "import package-nuget: failed to extract '{}.{}.nupkg' (tar exit {}).",
                idLower, verLower, rc);
            return false;
        }

        // Drop the .nupkg itself + a minimal .nupkg.metadata alongside the extracted
        // tree so NuGet tooling tolerates the folder. An empty contentHash is acceptable
        // for the v1 metadata schema (real restores overwrite it later).
        std::error_code copyEc;
        std::filesystem::copy_file(nupkgTmp, pkgFolder / (idLower + "." + verLower + ".nupkg"),
            std::filesystem::copy_options::overwrite_existing, copyEc);
        {
            std::ofstream meta(pkgFolder / ".nupkg.metadata", std::ios::binary);
            meta << "{\n  \"version\": 2,\n  \"contentHash\": \"\",\n"
                    "  \"source\": \"https://api.nuget.org/v3/index.json\"\n}\n";
        }

        std::filesystem::remove_all(tmp, ec);
        if (verbose_)
            std::cout << std::format("[verbose] nuget: extracted '{}/{}' into {}\n", idLower, verLower, pkgFolderStr);
        return true;
    }

    // Prefer the System32 copy (guaranteed on Windows 10+), else PATH.
    static std::string FindSystemTool(const char* name)
    {
        std::filesystem::path sys32 = std::filesystem::path("C:\\Windows\\System32") / name;
        std::error_code ec;
        if (std::filesystem::exists(sys32, ec)) return sys32.string();
        if (auto p = llvm::sys::findProgramByName(name)) return *p;
        return "";
    }

    // ---------------------------------------------------------------
    // Extraction: MsbuildLite evaluation UNIONed with a directory-convention probe.
    // ---------------------------------------------------------------
    void BuildResolution(const std::filesystem::path& pkgFolder,
                         const std::string& id,
                         const std::filesystem::path& root,
                         Resolution& out)
    {
        std::set<std::string> seenInc, seenWinmd, seenLib, seenDll;

        // --- 1. MsbuildLite eval of build\native\<Id>.{props,targets} + build\<Id>.{props,targets} ---
        std::map<std::string, std::string> seed = BuildSeedProps(root);
        MsbuildLite::EvalResult eval;
        std::string evalErr;

        std::filesystem::path buildNative = pkgFolder / "build" / "native";
        std::filesystem::path build = pkgFolder / "build";
        // Order: both .props first, then both .targets - props define the properties
        // the targets consume.
        const std::pair<std::filesystem::path, std::string> evalFiles[] = {
            { buildNative, id + ".props" },
            { build,       id + ".props" },
            { buildNative, id + ".targets" },
            { build,       id + ".targets" },
        };
        for (const auto& [dir, fname] : evalFiles)
        {
            std::string match = MatchFileCaseInsensitive(dir, fname);
            if (match.empty()) continue;
            if (verbose_)
                std::cout << std::format("[verbose] nuget: evaluating {}\n", match);
            if (!MsbuildLite::EvaluateFile(match, seed, eval, evalErr, verbose_, pkgFolder.string()))
                if (verbose_)
                    std::cout << std::format("[verbose] nuget: msbuild-lite eval failed for {}: {}\n", match, evalErr);
        }
        for (const auto& d : eval.includeDirs) AddUnique(out.includeDirs, seenInc, d);
        for (const auto& l : eval.libs)        AddUnique(out.libs, seenLib, l);
        for (const auto& d : eval.dlls)        AddUnique(out.dlls, seenDll, d);

        size_t evalInc = out.includeDirs.size(), evalLib = out.libs.size(), evalDll = out.dlls.size();

        // --- 2. Directory-convention probe (complementary to eval; UNION, dedupe) ---
        std::string arch = Arch();

        AddDirIfExists(out.includeDirs, seenInc, pkgFolder / "include");
        AddDirIfExists(out.includeDirs, seenInc, pkgFolder / "build" / "native" / "include");

        // Libs: lib\native\<arch>\ and build\native\<arch>\, in deterministic sorted order.
        AddLibsInDir(out.libs, seenLib, pkgFolder / "lib" / "native" / arch);
        AddLibsInDir(out.libs, seenLib, pkgFolder / "build" / "native" / arch);

        // DLLs: runtimes\win-<arch>\native\ only (never runtimes-framework\).
        AddDllsInDir(out.dlls, seenDll, pkgFolder / "runtimes" / ("win-" + arch) / "native");

        // WinMD dirs: metadata\ and lib\uap10.0\ (dirs only; .winmds are enumerated by the caller).
        // Some packages version the metadata folder (WinAppSDK.InteractiveExperiences ships
        // metadata\10.0.17763.0\*.winmd), so add immediate subdirs too, newest name first.
        AddDirIfExists(out.winmdDirs, seenWinmd, pkgFolder / "metadata");
        AddSubdirsNewestFirst(out.winmdDirs, seenWinmd, pkgFolder / "metadata");
        AddDirIfExists(out.winmdDirs, seenWinmd, pkgFolder / "lib" / "uap10.0");

        if (verbose_)
            std::cout << std::format(
                "[verbose] nuget: resolved '{}' - include {} (eval {} / probe {}), "
                "libs {} (eval {} / probe {}), dlls {} (eval {} / probe {}), winmd dirs {}\n",
                id,
                out.includeDirs.size(), evalInc, out.includeDirs.size() - evalInc,
                out.libs.size(), evalLib, out.libs.size() - evalLib,
                out.dlls.size(), evalDll, out.dlls.size() - evalDll,
                out.winmdDirs.size());
    }

    std::map<std::string, std::string> BuildSeedProps(const std::filesystem::path& root) const
    {
        // MSBuild native convention: 32-bit uses Platform=Win32, PlatformTarget=x86.
        std::string plat = (platform_ == "win32") ? "Win32" : "x64";
        std::string platTarget = (platform_ == "win32") ? "x86" : "x64";
        std::string rootWithSep = root.string();
        if (!rootWithSep.empty() && rootWithSep.back() != '\\' && rootWithSep.back() != '/')
            rootWithSep += "\\";
        return {
            { "Platform", plat },
            { "PlatformTarget", platTarget },
            { "Configuration", "Release" },
            { "NuGetPackageRoot", rootWithSep },
            { "TargetFramework", "native" },
            { "TargetPlatformIdentifier", "Windows" },
        };
    }

    // Case-insensitive filename match inside 'dir' (folder is lowercase on disk but
    // shipped files keep their original casing). Returns the absolute path or "".
    static std::string MatchFileCaseInsensitive(const std::filesystem::path& dir, const std::string& fname)
    {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) return "";
        std::string want = ToLower(fname);
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            if (ToLower(it->path().filename().string()) == want)
                return it->path().string();
        }
        return "";
    }

    static std::string DedupeKey(const std::string& p)
    {
        std::string k = p;
        for (auto& c : k) { if (c == '/') c = '\\'; c = (char)std::tolower((unsigned char)c); }
        while (!k.empty() && k.back() == '\\') k.pop_back();
        return k;
    }

    static void AddUnique(std::vector<std::string>& list, std::set<std::string>& seen, const std::string& path)
    {
        if (path.empty()) return;
        if (seen.insert(DedupeKey(path)).second) list.push_back(path);
    }

    static void AddDirIfExists(std::vector<std::string>& list, std::set<std::string>& seen, const std::filesystem::path& dir)
    {
        std::error_code ec;
        if (std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec))
            AddUnique(list, seen, dir.string());
    }

    // Add every immediate subdirectory of 'dir', sorted by name descending so a
    // versioned layout (metadata\10.0.18362.0 vs 10.0.17763.0) probes newest first.
    static void AddSubdirsNewestFirst(std::vector<std::string>& list, std::set<std::string>& seen, const std::filesystem::path& dir)
    {
        std::error_code ec;
        std::vector<std::string> subs;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
        {
            if (it->is_directory(ec)) subs.push_back(it->path().string());
        }
        std::sort(subs.rbegin(), subs.rend());
        for (const auto& s : subs) AddUnique(list, seen, s);
    }

    // Collect every .lib in 'dir', sorted, so the accumulation order is deterministic.
    // CAVEAT (WebView2): build\native\x64 ships BOTH WebView2Loader.dll.lib (import lib)
    // and WebView2LoaderStatic.lib. Both define the loader symbols; lld-link resolves an
    // archive member lazily from the first lib that satisfies an undefined symbol, so the
    // import lib (sorted first: '.' < 'S') wins and the static member is never pulled -
    // no duplicate-symbol error. Empirically this links + runs; no drop needed.
    static void AddLibsInDir(std::vector<std::string>& list, std::set<std::string>& seen, const std::filesystem::path& dir)
    {
        AddFilesByExt(list, seen, dir, ".lib");
    }

    static void AddDllsInDir(std::vector<std::string>& list, std::set<std::string>& seen, const std::filesystem::path& dir)
    {
        AddFilesByExt(list, seen, dir, ".dll");
    }

    static void AddFilesByExt(std::vector<std::string>& list, std::set<std::string>& seen,
                              const std::filesystem::path& dir, const std::string& ext)
    {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) return;
        std::vector<std::string> found;
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            std::string e = ToLower(it->path().extension().string());
            if (e == ext) found.push_back(it->path().string());
        }
        std::sort(found.begin(), found.end());
        for (const auto& f : found) AddUnique(list, seen, f);
    }
};
