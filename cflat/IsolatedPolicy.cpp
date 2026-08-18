#include "IsolatedPolicy.h"

#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/SHA256.h>
#include <nlohmann/json.hpp>

namespace
{
using json = nlohmann::json;

const std::array<std::string_view, 8> kPolicyCapabilities = {
    "stdio", "clock", "random", "filesystem", "network", "ui", "process", "threads"};

class DuplicateKeyValidator : public json::json_sax_t
{
public:
    bool null() override { return !failed_; }
    bool boolean(bool) override { return !failed_; }
    bool number_integer(number_integer_t) override { return !failed_; }
    bool number_unsigned(number_unsigned_t) override { return !failed_; }
    bool number_float(number_float_t, const string_t&) override { return !failed_; }
    bool string(string_t&) override { return !failed_; }
    bool binary(binary_t&) override { return !failed_; }

    bool start_object(std::size_t) override
    {
        objectKeys_.emplace_back();
        return !failed_;
    }

    bool key(string_t& value) override
    {
        auto& keys = objectKeys_.back();
        if (!keys.insert(value).second)
        {
            error_ = "duplicate key '" + value + "'";
            failed_ = true;
            return false;
        }
        return true;
    }

    bool end_object() override
    {
        objectKeys_.pop_back();
        return !failed_;
    }

    bool start_array(std::size_t) override { return !failed_; }
    bool end_array() override { return !failed_; }

    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override
    {
        return false;
    }

    const std::string& error() const { return error_; }

private:
    std::vector<std::unordered_set<std::string>> objectKeys_;
    std::string error_;
    bool failed_ = false;
};

bool HasOnlyKeys(const json& object, std::initializer_list<std::string_view> keys,
                std::string& error)
{
    for (const auto& key : object.items())
    {
        bool known = false;
        for (auto allowed : keys)
            if (key.key() == allowed) known = true;
        if (!known)
        {
            error = "unknown key '" + key.key() + "'";
            return false;
        }
    }
    return true;
}

bool PositiveInteger(const json& value, uint64_t& result)
{
    if (value.is_number_unsigned())
    {
        result = value.get<uint64_t>();
        return result > 0;
    }
    if (value.is_number_integer())
    {
        auto signedValue = value.get<int64_t>();
        if (signedValue <= 0) return false;
        result = static_cast<uint64_t>(signedValue);
        return true;
    }
    return false;
}

std::string DigestHex(const std::string& serialized)
{
    llvm::SHA256 sha;
    sha.update(llvm::StringRef(serialized));
    const auto digest = sha.final();
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (uint8_t byte : digest)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

json CanonicalPolicy(const json& document)
{
    json canonical = json::object();
    canonical["version"] = document["version"];
    canonical["language"] = document["language"];

    json capabilities = json::object();
    for (auto name : kPolicyCapabilities)
        capabilities[std::string(name)] = document["capabilities"][std::string(name)];
    canonical["capabilities"] = std::move(capabilities);

    if (document.contains("limits"))
    {
        json limits = json::object();
        const auto& source = document["limits"];
        for (const char* name : {"heap_bytes", "heap_mb", "max_threads"})
            if (source.contains(name)) limits[name] = source[name];
        canonical["limits"] = std::move(limits);
    }
    return canonical;
}

void AddSymbols(std::unordered_map<std::string, IsolatedCapability>& table,
                IsolatedCapability capability, std::initializer_list<std::string_view> symbols)
{
    for (auto symbol : symbols)
        table.emplace(std::string(symbol), capability);
}

const std::unordered_map<std::string, IsolatedCapability>& SymbolTable()
{
    static const auto table = [] {
        std::unordered_map<std::string, IsolatedCapability> result;
        // Inventory rules: fd read/write/close/fcntl are console I/O (Stdio), while
        // open/fopen/CreateFile and file mappings are Filesystem. Stream read/write on an
        // already-open FILE* is Stdio; obtaining the handle is the Filesystem-gated step.
        // Memory, string, math, allocator, and atomic primitives are Pure. Sleep is Clock.
        // Ambiguous OS calls use the more restrictive capability; dynamic loading is never approved.
        AddSymbols(result, IsolatedCapability::Pure, {
            "_aligned_free", "_aligned_malloc", "_atoi64", "_controlfp_s", "__atomic_acquire_load_flag",
            "__atomic_acquire_load_i32", "__atomic_acquire_load_i64", "__atomic_counter_add",
            "__atomic_counter_decrement", "__atomic_counter_increment", "__atomic_counter_read",
            "__atomic_flag_clear", "__atomic_flag_test_and_set", "__atomic_i32_cas", "__atomic_i32_load",
            "__atomic_i32_store", "__atomic_i64_cas", "__atomic_i64_load", "__atomic_i64_store",
            "__atomic_release_store_flag", "__atomic_release_store_i32", "__atomic_release_store_i64",
            "abort", "abs", "acos", "asin", "atan", "atan2", "atexit", "atof", "atoi", "atol",
            "atoll", "calloc", "cbrt", "cflat_heap_audit_alloc", "cflat_heap_audit_free",
            "cflat_heap_audit_report_leaks", "copysign", "copysignf", "cosh", "expm1", "fabs",
            "fegetenv", "fesetenv", "fmax", "fmin", "fmod", "free", "imaxabs", "labs", "llabs",
            "GetLargePageMinimum", "GetProcessHeap", "HeapValidate", "hypot", "isalnum", "isalpha", "isdigit", "islower", "isspace",
            "isupper", "log1p", "malloc", "memchr", "memcmp", "memcpy", "memmove", "memset",
            "posix_memalign", "VirtualAlloc", "VirtualAllocExNuma", "VirtualFree", "mprotect", "madvise",
            "pow", "qsort", "realloc", "sin", "sinh", "sqrt", "strcat", "strchr", "strcmp",
            "strcpy", "strlen", "strncmp", "strncpy", "strncat", "strrchr", "strstr", "strtod",
            "strtok", "strtol", "strtoll", "strtoul", "strtoull", "tan", "tanh", "tolower",
            "toupper", "MultiByteToWideChar", "WideCharToMultiByte", "exit", "_exit"});
        AddSymbols(result, IsolatedCapability::Stdio, {
            "__acrt_iob_func", "__isoc99_vfscanf", "__isoc99_vsscanf", "__stdio_common_vfprintf",
            "__stdio_common_vfscanf", "__stdio_common_vsscanf", "__stdio_common_vsprintf",
            "__stderrp", "__stdinp", "__stdoutp", "__vfprintf_chk", "__vsnprintf_chk", "_getch",
            "close", "fflush", "fgetc", "fread", "fprintf", "fputc", "fputs", "fwrite", "getc", "getchar",
            "GetConsoleMode", "GetConsoleOutputCP", "GetConsoleScreenBufferInfo", "GetNumberOfConsoleInputEvents",
            "GetStdHandle", "fcntl", "ioctl", "PeekConsoleInputA", "poll", "printf", "putc", "putchar", "putn", "puts",
            "ReadConsoleInputA", "ReadConsoleOutputA", "read", "SetConsoleCursorInfo", "SetConsoleMode",
            "SetConsoleOutputCP", "stderr", "stdin", "stdout", "tcgetattr", "tcsetattr", "vfprintf",
            "vfscanf", "vprintf", "vscanf", "vsnprintf", "vsprintf", "vsscanf", "write", "_write", "sprintf", "snprintf",
            "scanf", "sscanf", "WriteFile", "ReadFile",
            "WriteConsoleInputA", "_getch", "FlushConsoleInputBuffer"});
        AddSymbols(result, IsolatedCapability::Clock, {
            "GetSystemTimeAsFileTime", "QueryPerformanceCounter", "QueryPerformanceFrequency",
            "Sleep", "clock_gettime", "nanosleep", "time"});
        AddSymbols(result, IsolatedCapability::Random, {
            "BCryptGenRandom", "CryptGenRandom", "RtlGenRandom", "arc4random", "arc4random_buf",
            "getentropy", "rand", "srand"});
        AddSymbols(result, IsolatedCapability::Filesystem, {
            "_access", "_chdir", "_fseeki64", "_ftelli64", "_getcwd", "_mkdir", "_rmdir", "_unlink",
            "access", "chdir", "closedir", "CopyFileA", "CreateFileA", "CreateFileMappingA",
            "FindClose", "FindFirstFileA", "FindNextFileA", "FlushViewOfFile", "fclose", "feof", "fgets", "fopen",
            "fseek", "fseeko", "ftell", "ftello", "ftruncate", "GetFileAttributesA",
            "GetFileSizeEx", "GetFullPathNameA", "GetTempFileNameA", "GetTempPathA", "getcwd", "lseek",
            "MapViewOfFile", "mmap", "mkdir", "mkstemp", "msync", "munmap", "open", "opendir",
            "realpath", "readdir", "rename", "rmdir", "SetEndOfFile", "SetFilePointerEx", "stat", "unlink",
            "MoveFileExA", "fscanf",
            "UnmapViewOfFile"});
        AddSymbols(result, IsolatedCapability::Network, {
            "WSACleanup", "WSAGetLastError", "WSAStartup", "accept", "bind", "closesocket", "connect",
            "freeaddrinfo", "getaddrinfo", "htons", "inet_addr", "ioctlsocket", "listen", "ntohs", "recv",
            "send", "setsockopt", "socket"});
        AddSymbols(result, IsolatedCapability::Ui, {
            "AdjustTokenPrivileges", "CreateDIBSection",
            "DwmSetWindowAttribute", "EnterCriticalSection", "GetDIBits", "InitCommonControls",
            "InitCommonControlsEx", "LeaveCriticalSection", "LoadImageA", "LoadLibraryA",
            "GetProcAddress", "MddBootstrapInitialize2", "MddBootstrapShutdown", "MultiByteToWideChar",
            "PrintWindow",
            "RoActivateInstance", "RoGetActivationFactory", "RoInitialize", "RoUninitialize", "SetWindowTheme",
            "WideCharToMultiByte", "WindowsCreateString", "WindowsDeleteString", "WindowsGetStringRawBuffer"});
        // CloseHandle only releases an already-audited OS handle.
        AddSymbols(result, IsolatedCapability::Pure, {"CloseHandle"});
        AddSymbols(result, IsolatedCapability::Process, {
            "CreatePipe", "CreateProcessA", "dup2", "execvp", "fork", "GetCurrentProcess", "GetExitCodeProcess",
            "GetProcessAffinityMask", "K32GetProcessMemoryInfo", "kill", "OpenProcessToken",
            "pipe", "SetHandleInformation", "SetProcessAffinityMask", "signal",
            "TerminateProcess", "waitpid", "GetLastError", "getenv",
            "LookupPrivilegeValueA"});
        AddSymbols(result, IsolatedCapability::Threads, {
            "AcquireSRWLockExclusive", "AcquireSRWLockShared", "CreateEventA", "CreateSemaphoreA", "CreateThread",
            "DeleteCriticalSection", "GetActiveProcessorCount", "GetCurrentThreadId", "GetLogicalProcessorInformation",
            "GetLogicalProcessorInformationEx", "GetSystemCpuSetInformation", "GetSystemInfo",
            "GetExitCodeThread", "GetNumaAvailableMemoryNodeEx", "GetNumaHighestNodeNumber", "GetThreadAffinityMask",
            "InitializeConditionVariable", "InitializeCriticalSection", "pthread_cancel",
            "pthread_cond_broadcast", "pthread_cond_destroy", "pthread_cond_init", "pthread_cond_signal",
            "pthread_cond_wait", "pthread_create", "pthread_detach", "pthread_join", "pthread_mutex_destroy",
            "pthread_mutex_init", "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_rwlock_destroy",
            "pthread_rwlock_init", "pthread_rwlock_rdlock", "pthread_rwlock_unlock", "pthread_rwlock_wrlock",
            "pthread_set_qos_class_self_np", "pthread_setaffinity_np", "pthread_threadid_np",
            "pthread_timedjoin_np", "sched_getaffinity", "sched_setaffinity", "sched_yield", "sem_destroy",
            "sem_init", "sem_post", "sem_wait", "SleepConditionVariableCS", "SleepConditionVariableSRW",
            "ResumeThread", "SetEvent", "SetThreadAffinityMask", "SuspendThread", "SwitchToThread", "TerminateThread",
            "WaitForSingleObject", "WakeAllConditionVariable", "WakeConditionVariable", "ReleaseSemaphore",
            "ReleaseSRWLockExclusive", "ReleaseSRWLockShared", "SetProcessDefaultCpuSets", "sysctlbyname", "dispatch_release",
            "dispatch_semaphore_create", "dispatch_semaphore_signal",
            "dispatch_semaphore_wait", "gettid", "sysconf"});
        return result;
    }();
    return table;
}

const std::unordered_set<std::string>& NeverAllowedSymbols()
{
    static const std::unordered_set<std::string> symbols = {
        "dlsym", "dlopen", "dlclose", "LoadLibraryA", "GetProcAddress", "syscall"};
    return symbols;
}
}

const char* IsolatedCapabilityName(IsolatedCapability capability)
{
    switch (capability)
    {
    case IsolatedCapability::Pure: return "pure";
    case IsolatedCapability::Stdio: return "stdio";
    case IsolatedCapability::Clock: return "clock";
    case IsolatedCapability::Random: return "random";
    case IsolatedCapability::Filesystem: return "filesystem";
    case IsolatedCapability::Network: return "network";
    case IsolatedCapability::Ui: return "ui";
    case IsolatedCapability::Process: return "process";
    case IsolatedCapability::Threads: return "threads";
    }
    return "unknown";
}

std::optional<IsolatedCapability> ClassifyIsolatedSymbol(std::string_view symbol)
{
    auto it = SymbolTable().find(std::string(symbol));
    if (it == SymbolTable().end()) return std::nullopt;
    return it->second;
}

bool IsNeverAllowedIsolatedSymbol(std::string_view symbol)
{
    return NeverAllowedSymbols().count(std::string(symbol)) != 0;
}

std::optional<IsolatedCapability> ClassifyIsolatedCoreModule(
    const std::filesystem::path& path, const std::filesystem::path& coreDirectory)
{
    std::error_code ec;
    auto relative = std::filesystem::relative(path, coreDirectory, ec).generic_string();
    if (ec) return std::nullopt;
    if (relative == "filesystem.cb") return IsolatedCapability::Filesystem;
    if (relative == "time.cb") return IsolatedCapability::Clock;
    if (relative == "random.cb") return IsolatedCapability::Random;
    if (relative == "terminal.cb") return IsolatedCapability::Stdio;
    if (relative == "process.cb") return IsolatedCapability::Process;
    if (relative == "thread.cb" || relative == "threadpool.cb"
        || relative == "hpc/parallel.cb" || relative == "numa.cb")
        return IsolatedCapability::Threads;
    if (relative == "cocoa.cb" || relative == "com.cb" || relative == "winrt.cb"
        || relative == "ui_test.cb" || relative.starts_with("graphic/") || relative.starts_with("ui_native/")
        || relative.starts_with("ui_canvas/"))
        return IsolatedCapability::Ui;
    if (relative == "network/socket.cb" || relative == "network/socket.posix.cb"
        || relative == "network/socket.windows.cb")
        return IsolatedCapability::Network;
    return std::nullopt;
}

bool ParseIsolatedPolicyFile(const std::string& path, IsolatedPolicy& policy, std::string& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "cannot read policy file";
        return false;
    }

    DuplicateKeyValidator duplicateValidator;
    input.clear();
    input.seekg(0);
    if (!json::sax_parse(input, &duplicateValidator) && !duplicateValidator.error().empty())
    {
        error = duplicateValidator.error();
        return false;
    }
    input.clear();
    input.seekg(0);

    json document;
    try
    {
        input >> document;
    }
    catch (const json::exception& ex)
    {
        error = std::string("invalid JSON: ") + ex.what();
        return false;
    }
    if (!document.is_object())
    {
        error = "policy must be a JSON object";
        return false;
    }
    if (!HasOnlyKeys(document, {"version", "language", "capabilities", "limits"}, error))
        return false;
    for (const char* key : {"version", "language", "capabilities"})
    {
        if (!document.contains(key))
        {
            error = "missing required key '" + std::string(key) + "'";
            return false;
        }
    }
    if (!document["version"].is_number_integer() || document["version"].get<int64_t>() != 1)
    {
        error = "'version' must be integer 1";
        return false;
    }
    if (!document["language"].is_string() || document["language"].get<std::string>() != "restricted-v1")
    {
        error = "'language' must be 'restricted-v1'";
        return false;
    }

    const auto& capabilities = document["capabilities"];
    if (!capabilities.is_object())
    {
        error = "'capabilities' must be an object";
        return false;
    }
    if (!HasOnlyKeys(capabilities, {"stdio", "clock", "random", "filesystem", "network", "ui", "process", "threads"}, error))
        return false;
    for (size_t i = 0; i < kPolicyCapabilities.size(); ++i)
    {
        std::string key(kPolicyCapabilities[i]);
        if (!capabilities.contains(key))
        {
            error = "missing required capability key '" + key + "'";
            return false;
        }
        if (!capabilities[key].is_string())
        {
            error = "capability '" + key + "' must be 'allow' or 'deny'";
            return false;
        }
        const auto value = capabilities[key].get<std::string>();
        if (value != "allow" && value != "deny")
        {
            error = "capability '" + key + "' must be 'allow' or 'deny'";
            return false;
        }
    }

    IsolatedPolicy parsed;
    parsed.path = path;
    for (size_t i = 0; i < kPolicyCapabilities.size(); ++i)
        parsed.allow[i + 1] = capabilities[std::string(kPolicyCapabilities[i])] == "allow";

    if (document.contains("limits"))
    {
        const auto& limits = document["limits"];
        if (!limits.is_object())
        {
            error = "'limits' must be an object";
            return false;
        }
        if (!HasOnlyKeys(limits, {"heap_mb", "heap_bytes", "max_threads"}, error))
            return false;
        if (limits.contains("heap_mb") && limits.contains("heap_bytes"))
        {
            error = "'limits' may contain only one of 'heap_mb' and 'heap_bytes'";
            return false;
        }
        uint64_t value = 0;
        if (limits.contains("heap_mb"))
        {
            if (!PositiveInteger(limits["heap_mb"], value))
            {
                error = "'limits.heap_mb' must be a positive base-10 integer";
                return false;
            }
            if (value > std::numeric_limits<uint64_t>::max() / 1048576)
            {
                error = "'limits.heap_mb' is too large";
                return false;
            }
            parsed.heapBytes = value * 1048576;
        }
        if (limits.contains("heap_bytes"))
        {
            if (!PositiveInteger(limits["heap_bytes"], value))
            {
                error = "'limits.heap_bytes' must be a positive base-10 integer";
                return false;
            }
            parsed.heapBytes = value;
        }
        if (limits.contains("max_threads"))
        {
            if (!PositiveInteger(limits["max_threads"], value))
            {
                error = "'limits.max_threads' must be a positive base-10 integer";
                return false;
            }
            parsed.maxThreads = value;
        }
        if (parsed.maxThreads && *parsed.maxThreads > 1
            && capabilities["threads"] == "deny")
        {
            error = "'limits.max_threads' must be 1 when capability 'threads' is 'deny'";
            return false;
        }
    }

    parsed.digestHex = DigestHex(CanonicalPolicy(document).dump());
    policy = std::move(parsed);
    return true;
}
