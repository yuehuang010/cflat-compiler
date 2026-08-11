#include "Platform.h"
#include "Sha256.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
struct TestFailure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

void Require(bool condition, const std::string& message)
{
    if (!condition) throw TestFailure(message);
}

template <typename Function>
void RunCase(const char* name, Function&& function)
{
    try
    {
        function();
        std::cout << "PASSED: " << name << "\n";
    }
    catch (const std::exception& exception)
    {
        throw TestFailure(std::string(name) + ": " + exception.what());
    }
}

void WriteBytes(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(output), "could not write " + path.string());
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    Require(static_cast<bool>(output), "could not finish writing " + path.string());
}

std::string ReadBytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "could not read " + path.string());
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void CopyExecutable(const fs::path& source, const fs::path& destination)
{
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    Require(!ec, "could not create artifact directory: " + ec.message());
    Require(fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec) && !ec,
            "could not copy test compiler: " + ec.message());
    std::string permissionError;
    Require(launcher_platform::MakeExecutable(destination, permissionError), permissionError);
}

std::string HashFile(const fs::path& path)
{
    std::array<uint8_t, 32> digest{};
    uint64_t size = 0;
    std::string error;
    Require(Sha256::File(path, digest, size, error), error);
    return Sha256::Hex(digest);
}

struct ReleaseFixture
{
    fs::path manifest;
    fs::path signature;
    fs::path artifact;
};

ReleaseFixture MakeRelease(const fs::path& base, const fs::path& testCompiler,
                           const std::string& version, int sequence)
{
    fs::path release = base / ("release-" + version);
    fs::path artifact = release / "artifact";
    fs::path compiler = artifact / "cflat.exe";
    CopyExecutable(testCompiler, compiler);
    WriteBytes(artifact / "version.txt", version + "\n");

    std::string manifest;
    manifest += "cflat-manifest-v1\n";
    manifest += "scope=compiler-release\n";
    manifest += "product=cflat\n";
    manifest += "channel=stable\n";
    manifest += "sequence=" + std::to_string(sequence) + "\n";
    manifest += "version=" + version + "\n";
    manifest += "platform=test\n";
    manifest += "file=cflat.exe " + std::to_string(fs::file_size(compiler)) + " " + HashFile(compiler) + "\n";
    manifest += "file=version.txt " + std::to_string(fs::file_size(artifact / "version.txt")) + " " + HashFile(artifact / "version.txt") + "\n";
    manifest += "end\n";

    fs::path manifestPath = release / "manifest.cflat";
    fs::path signaturePath = release / "manifest.cflat.sig";
    WriteBytes(manifestPath, manifest);
    std::string manifestHash = HashFile(manifestPath);
    WriteBytes(signaturePath,
               "cflat-signature-v1\n"
               "algorithm=sha256-standin\n"
               "key_id=launcher-test\n"
               "signed_sha256=" + manifestHash + "\n"
               "signature=" + manifestHash + "\n"
               "end\n");
    return { manifestPath, signaturePath, artifact };
}

int RunLauncher(const fs::path& launcher, const fs::path& root,
                std::initializer_list<std::string> arguments)
{
    std::vector<std::string> command{ "--root", root.string() };
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::string error;
    int result = launcher_platform::RunProcess(launcher, command, launcher.parent_path(), error);
    Require(result >= 0, "launcher process failed to start: " + error);
    return result;
}

json ReadState(const fs::path& root)
{
    std::ifstream input(root / "update-state.json");
    Require(static_cast<bool>(input), "launcher state was not written");
    json state;
    input >> state;
    return state;
}

std::string ActiveVersion(const fs::path& root)
{
    json state = ReadState(root);
    return state.at("active_compiler").at("version").get<std::string>();
}

void Install(const fs::path& launcher, const fs::path& root, const ReleaseFixture& release)
{
    int result = RunLauncher(launcher, root, {
        "--install-release", release.manifest.string(), release.signature.string(), release.artifact.string()
    });
    Require(result == 0, "valid release install failed with exit code " + std::to_string(result));
}

void TestSha256()
{
    Sha256 hash;
    hash.Update("abc");
    Require(Sha256::Hex(hash.Final()) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 known vector failed");
}

void TestActivationRunReplayAndRollback(const fs::path& base, const fs::path& launcher, const fs::path& testCompiler)
{
    fs::path root = base / "activation";
    ReleaseFixture one = MakeRelease(base, testCompiler, "one", 1);
    ReleaseFixture two = MakeRelease(base, testCompiler, "two", 2);
    Install(launcher, root, one);
    Require(ActiveVersion(root) == "one", "first release was not active");
    Require(fs::exists(root / "releases" / "compiler" / "one" / ".cflat" / "initialized"),
            "init-local smoke setup was not created");
    Require(RunLauncher(launcher, root, { "--no-update", "--", "--version" }) == 0,
            "--no-update did not run the active compiler");
    Require(RunLauncher(launcher, root, { "--run", "--", "--version" }) == 0,
            "active compiler did not run");
    Require(ReadBytes(root / "releases" / "compiler" / "one" / "run.marker") == "one\none\none\n",
            "compiler arguments were not forwarded to the active slot");
    Require(RunLauncher(launcher, root, { "--run", "--", "--exit-code", "37" }) == 37,
            "compiler exit code was not forwarded");

    Install(launcher, root, two);
    Require(ActiveVersion(root) == "two", "second release was not activated");
    Require(ReadState(root).at("previous_compiler").at("version").get<std::string>() == "one",
            "previous compiler was not recorded");
    Require(RunLauncher(launcher, root, {
        "--install-release", two.manifest.string(), two.signature.string(), two.artifact.string()
    }) != 0, "replayed release was accepted");
    Require(ActiveVersion(root) == "two", "replay changed the active release");

    Require(RunLauncher(launcher, root, { "--rollback" }) == 0, "rollback failed");
    Require(ActiveVersion(root) == "one", "rollback selected the wrong release");

    WriteBytes(root / "update-state.json", "not json\n");
    Require(RunLauncher(launcher, root, { "--run", "--", "--version" }) == 0,
            "corrupt state did not recover from the current pointer");
    Require(ActiveVersion(root) == "one", "state recovery selected the wrong release");
}

void TestTamperFallback(const fs::path& base, const fs::path& launcher, const fs::path& testCompiler)
{
    fs::path root = base / "tamper";
    ReleaseFixture one = MakeRelease(base, testCompiler, "one", 1);
    ReleaseFixture two = MakeRelease(base, testCompiler, "two", 2);
    Install(launcher, root, one);
    Install(launcher, root, two);
    WriteBytes(root / "releases" / "compiler" / "two" / "version.txt", "tampered\n");
    Require(RunLauncher(launcher, root, { "--run", "--", "--version" }) == 0,
            "tampered active release did not fall back");
    Require(ActiveVersion(root) == "one", "tamper fallback selected the wrong release");
}

void TestInvalidMetadata(const fs::path& base, const fs::path& launcher, const fs::path& testCompiler)
{
    fs::path root = base / "invalid";
    ReleaseFixture release = MakeRelease(base, testCompiler, "invalid", 1);
    WriteBytes(release.signature, "cflat-signature-v1\nalgorithm=sha256-standin\nend\n");
    Require(RunLauncher(launcher, root, {
        "--install-release", release.manifest.string(), release.signature.string(), release.artifact.string()
    }) != 0, "invalid detached metadata was accepted");
    Require(!fs::exists(root / "current"), "invalid metadata changed the active pointer");

    fs::path protectedRoot = base / "protected";
    ReleaseFixture valid = MakeRelease(base, testCompiler, "valid", 1);
    Install(launcher, protectedRoot, valid);
    ReleaseFixture badUpdate = MakeRelease(base, testCompiler, "bad-update", 2);
    WriteBytes(badUpdate.signature, "cflat-signature-v1\nalgorithm=sha256-standin\nend\n");
    Require(RunLauncher(launcher, protectedRoot, {
        "--install-release", badUpdate.manifest.string(), badUpdate.signature.string(), badUpdate.artifact.string()
    }) != 0, "invalid update was accepted over an active release");
    Require(ActiveVersion(protectedRoot) == "valid", "invalid update changed the active release");

    fs::path traversalRoot = base / "traversal";
    std::string traversal = "cflat-manifest-v1\n"
                            "scope=compiler-release\nproduct=cflat\nsequence=1\nversion=traversal\n"
                            "file=cflat.exe 0 " + std::string(64, '0') + "\n"
                            "file=../outside 0 " + std::string(64, '0') + "\nend\n";
    fs::path traversalManifest = base / "traversal.manifest.cflat";
    fs::path traversalSignature = base / "traversal.manifest.cflat.sig";
    WriteBytes(traversalManifest, traversal);
    std::string hash = HashFile(traversalManifest);
    WriteBytes(traversalSignature,
               "cflat-signature-v1\nalgorithm=sha256-standin\nkey_id=launcher-test\n"
               "signed_sha256=" + hash + "\nsignature=" + hash + "\nend\n");
    Require(RunLauncher(launcher, traversalRoot, {
        "--install-release", traversalManifest.string(), traversalSignature.string(), release.artifact.string()
    }) != 0, "path traversal metadata was accepted");
    Require(!fs::exists(traversalRoot / "current"), "path traversal changed the active pointer");
}

void TestChannelUpdate(const fs::path& base, const fs::path& launcher, const fs::path& testCompiler)
{
    fs::path feed = base / "feed" / "compiler" / "stable";
    ReleaseFixture release = MakeRelease(base / "feed-source", testCompiler, "channel", 7);
    fs::path releaseRoot = feed / "releases" / "channel";
    fs::create_directories(releaseRoot);
    std::error_code ec;
    fs::copy_file(release.manifest, releaseRoot / "manifest.cflat", fs::copy_options::overwrite_existing, ec);
    Require(!ec, "could not copy channel manifest");
    fs::copy_file(release.signature, releaseRoot / "manifest.cflat.sig", fs::copy_options::overwrite_existing, ec);
    Require(!ec, "could not copy channel signature");
    fs::copy(release.artifact, releaseRoot / "artifact", fs::copy_options::recursive, ec);
    Require(!ec, "could not copy channel artifact");

    std::string channel = "cflat-channel-v1\n"
                          "scope=compiler-channel\nproduct=cflat\nchannel=stable\n"
                          "latest_sequence=7\nlatest_version=channel\n"
                          "manifest_path=releases/channel/manifest.cflat\n"
                          "signature_path=releases/channel/manifest.cflat.sig\n"
                          "artifact_path=releases/channel/artifact\nend\n";
    fs::path channelPath = feed / "channel.cflat";
    fs::path channelSignature = feed / "channel.cflat.sig";
    WriteBytes(channelPath, channel);
    std::string hash = HashFile(channelPath);
    WriteBytes(channelSignature,
               "cflat-signature-v1\nalgorithm=sha256-standin\nkey_id=launcher-test\n"
               "signed_sha256=" + hash + "\nsignature=" + hash + "\nend\n");

    fs::path root = base / "channel-install";
    Require(RunLauncher(launcher, root, { "--update-from", (base / "feed").string() }) == 0,
            "channel update failed");
    Require(ActiveVersion(root) == "channel", "channel update selected the wrong release");
}
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "Usage: cflat-launcher-test <launcher> <test-compiler>\n";
        return 2;
    }

    fs::path launcher = fs::absolute(argv[1]);
    fs::path testCompiler = fs::absolute(argv[2]);
    fs::path base = fs::current_path() / "scratch" /
                    ("launcher-test-suite-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool passed = false;
    try
    {
        fs::create_directories(base);
        RunCase("sha256-known-vector", TestSha256);
        RunCase("activation-run-replay-rollback", [&] {
            TestActivationRunReplayAndRollback(base, launcher, testCompiler);
        });
        RunCase("tamper-fallback", [&] {
            TestTamperFallback(base, launcher, testCompiler);
        });
        RunCase("invalid-metadata-and-protected-update", [&] {
            TestInvalidMetadata(base, launcher, testCompiler);
        });
        RunCase("channel-update", [&] {
            TestChannelUpdate(base, launcher, testCompiler);
        });
        passed = true;
        std::cout << "PASS: cflat launcher integration suite\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: " << exception.what() << "\n";
        std::cerr << "Test artifacts retained at: " << base.string() << "\n";
    }

    if (passed)
    {
        std::error_code ec;
        fs::remove_all(base, ec);
        if (ec)
        {
            std::cerr << "FAIL: could not clean test artifacts: " << ec.message() << "\n";
            return 1;
        }
    }
    return passed ? 0 : 1;
}
