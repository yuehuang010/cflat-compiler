#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

enum class IsolatedCapability
{
    Pure,
    Stdio,
    Clock,
    Random,
    Filesystem,
    Network,
    Ui,
    Process,
    Threads,
};

constexpr size_t IsolatedPolicyCapabilityCount = 9;

const char* IsolatedCapabilityName(IsolatedCapability capability);
std::optional<IsolatedCapability> ClassifyIsolatedSymbol(std::string_view symbol);
bool IsNeverAllowedIsolatedSymbol(std::string_view symbol);
std::optional<IsolatedCapability> ClassifyIsolatedCoreModule(
    const std::filesystem::path& path, const std::filesystem::path& coreDirectory);

struct IsolatedPolicy
{
    std::string path;
    std::array<bool, IsolatedPolicyCapabilityCount> allow{};
    std::optional<uint64_t> heapBytes;
    std::optional<uint64_t> maxThreads;
    std::string digestHex;

    bool IsDenied(IsolatedCapability capability) const
    {
        return capability != IsolatedCapability::Pure && !allow[static_cast<size_t>(capability)];
    }
};

bool ParseIsolatedPolicyFile(const std::string& path, IsolatedPolicy& policy, std::string& error);
