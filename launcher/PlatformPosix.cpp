#include "Platform.h"

#include <chrono>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <array>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

namespace launcher_platform
{
struct FileLock::State
{
    int fd = -1;
};

bool PathComponentEqual(std::string_view left, std::string_view right)
{
    return left == right;
}

std::optional<std::filesystem::path> ExecutablePath(std::string& error)
{
    std::array<char, 4096> buffer{};
    ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0)
    {
        error = "could not resolve the launcher's executable path";
        return std::nullopt;
    }
    buffer[static_cast<size_t>(length)] = '\0';
    return std::filesystem::path(buffer.data());
}

std::string CurrentUtcTimestamp()
{
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

bool MakeExecutable(const std::filesystem::path& path, std::string& error)
{
    std::error_code ec;
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_exec |
                                 std::filesystem::perms::group_exec |
                                 std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
    if (ec)
    {
        error = "could not make test compiler executable: " + ec.message();
        return false;
    }
    return true;
}

FileLock::FileLock() : state_(std::make_unique<State>())
{
}

FileLock::~FileLock()
{
    if (state_->fd >= 0) close(state_->fd);
}

bool FileLock::TryAcquire(const std::filesystem::path& path)
{
    state_->fd = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (state_->fd < 0) return false;
    if (flock(state_->fd, LOCK_EX | LOCK_NB) != 0)
    {
        close(state_->fd);
        state_->fd = -1;
        return false;
    }
    return true;
}

int RunProcess(const std::filesystem::path& executable,
               const std::vector<std::string>& arguments,
               const std::filesystem::path& workingDirectory,
               std::string& error)
{
    pid_t child = fork();
    if (child < 0)
    {
        error = "could not fork compiler process";
        return -1;
    }
    if (child == 0)
    {
        if (chdir(workingDirectory.c_str()) != 0) _exit(127);
        std::vector<std::string> storage;
        storage.push_back(executable.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (std::string& value : storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0)
    {
        error = "could not wait for compiler process";
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 1);
}
}
