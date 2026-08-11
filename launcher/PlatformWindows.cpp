#include "Platform.h"

#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <iomanip>
#include <sstream>

namespace launcher_platform
{
struct FileLock::State
{
    HANDLE handle = INVALID_HANDLE_VALUE;
};

bool PathComponentEqual(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i)
    {
        char lhs = left[i];
        char rhs = right[i];
        if (lhs >= 'A' && lhs <= 'Z') lhs = static_cast<char>(lhs - 'A' + 'a');
        if (rhs >= 'A' && rhs <= 'Z') rhs = static_cast<char>(rhs - 'A' + 'a');
        if (lhs != rhs) return false;
    }
    return true;
}

std::optional<std::filesystem::path> ExecutablePath(std::string& error)
{
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size())
    {
        error = "could not resolve the launcher's executable path";
        return std::nullopt;
    }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::string CurrentUtcTimestamp()
{
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_s(&tm, &now);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

bool MakeExecutable(const std::filesystem::path&, std::string&)
{
    return true;
}

FileLock::FileLock() : state_(std::make_unique<State>())
{
}

FileLock::~FileLock()
{
    if (state_->handle != INVALID_HANDLE_VALUE)
        CloseHandle(state_->handle);
}

bool FileLock::TryAcquire(const std::filesystem::path& path)
{
    state_->handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return state_->handle != INVALID_HANDLE_VALUE;
}

namespace
{
std::wstring ToWide(const std::string& value)
{
    if (value.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring QuoteArgument(const std::wstring& value)
{
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : value)
    {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'\"') result.append(backslashes * 2 + 1, L'\\');
        else result.append(backslashes, L'\\');
        result.push_back(c);
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}
}

int RunProcess(const std::filesystem::path& executable,
               const std::vector<std::string>& arguments,
               const std::filesystem::path& workingDirectory,
               std::string& error)
{
    std::wstring command = QuoteArgument(executable.wstring());
    for (const std::string& argument : arguments)
    {
        std::wstring wide = ToWide(argument);
        if (wide.empty() && !argument.empty())
        {
            error = "compiler argument is not valid UTF-8";
            return -1;
        }
        command += L" ";
        command += QuoteArgument(wide);
    }
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring cwd = workingDirectory.wstring();
    if (!CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                        0, nullptr, cwd.c_str(), &startup, &process))
    {
        error = "could not start " + executable.string() + " (Windows error " + std::to_string(GetLastError()) + ")";
        return -1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
}
