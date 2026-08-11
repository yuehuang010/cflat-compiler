#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher_platform
{
bool PathComponentEqual(std::string_view left, std::string_view right);
std::optional<std::filesystem::path> ExecutablePath(std::string& error);
std::string CurrentUtcTimestamp();
bool MakeExecutable(const std::filesystem::path& path, std::string& error);

class FileLock
{
public:
    FileLock();
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    bool TryAcquire(const std::filesystem::path& path);

private:
    struct State;
    std::unique_ptr<State> state_;
};

int RunProcess(const std::filesystem::path& executable,
               const std::vector<std::string>& arguments,
               const std::filesystem::path& workingDirectory,
               std::string& error);
}
