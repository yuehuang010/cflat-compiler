#pragma once
// Cross-platform compatibility shims for Windows CRT / SAL functions and
// calling-convention keywords used throughout the compiler host code. On
// Windows everything here resolves to the native CRT; on POSIX (Linux/macOS)
// the underscore-prefixed CRT functions are mapped to their standard
// equivalents and the MSVC calling-convention keywords expand to nothing.
//
// Include this early in any TU that uses getenv_s / _strnicmp / _aligned_malloc
// / __stdcall / MAX_PATH / PlatformExePath, etc.

#include <string>
#include <cstdio>     // EOF, std::FILE

#if defined(_WIN32)

#  include <cstdlib>   // _get_pgmptr, getenv_s
#  include <process.h> // _getpid
#  include <malloc.h>  // _aligned_malloc / _aligned_free
#  include <cstring>   // _strnicmp / _stricmp live in <string.h>

#else // ---------------------------- POSIX ----------------------------

#  include <cstdlib>
#  include <cstring>
#  include <strings.h>   // strcasecmp / strncasecmp
#  include <unistd.h>    // getpid, readlink
#  include <climits>     // PATH_MAX
#  if defined(__APPLE__)
#    include <mach-o/dyld.h> // _NSGetExecutablePath
#    include <cstdint>
#  endif

// MSVC calling-convention keywords are not recognized by GCC/Clang on non-x86
// or in C++ mode; expand them away. Guarded so we never clobber a real define.
#  ifndef __stdcall
#    define __stdcall
#  endif
#  ifndef __cdecl
#    define __cdecl
#  endif

#  ifndef MAX_PATH
#    if defined(PATH_MAX)
#      define MAX_PATH PATH_MAX
#    else
#      define MAX_PATH 4096
#    endif
#  endif

// errno_t-style getenv_s: writes the value (NUL-terminated) into buffer and the
// required size (including NUL) into *requiredSize. Querying with buffer==nullptr
// is supported (returns the size only). Returns 0 on success, ERANGE if too small.
inline int getenv_s(size_t* requiredSize, char* buffer, size_t bufferSize, const char* name)
{
    const char* v = std::getenv(name);
    size_t len = v ? std::strlen(v) + 1 : 0;
    if (requiredSize) *requiredSize = len;
    if (!v)
    {
        if (buffer && bufferSize) buffer[0] = '\0';
        return 0;
    }
    if (buffer)
    {
        if (bufferSize < len) return 34 /*ERANGE*/;
        std::memcpy(buffer, v, len);
    }
    return 0;
}

inline int _putenv_s(const char* name, const char* value) { return ::setenv(name, value, 1); }

inline int _strnicmp(const char* a, const char* b, size_t n) { return ::strncasecmp(a, b, n); }
inline int _stricmp(const char* a, const char* b)           { return ::strcasecmp(a, b); }

inline int _getpid() { return ::getpid(); }

inline void* _aligned_malloc(size_t size, size_t alignment)
{
    void* p = nullptr;
    // posix_memalign requires alignment to be a power of two multiple of sizeof(void*).
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    if (::posix_memalign(&p, alignment, size) != 0) return nullptr;
    return p;
}
inline void _aligned_free(void* p) { ::free(p); }

// Low-level CRT fd functions (used by the LSP transport and a few file paths).
// POSIX has no text/binary stream distinction, so _setmode is a no-op and
// _O_BINARY is 0.
#  ifndef _O_BINARY
#    define _O_BINARY 0
#  endif
inline int _setmode(int /*fd*/, int /*mode*/)              { return 0; }
inline int _read(int fd, void* buf, unsigned int n)        { return static_cast<int>(::read(fd, buf, n)); }
inline int _write(int fd, const void* buf, unsigned int n) { return static_cast<int>(::write(fd, buf, n)); }
inline int _close(int fd)                                  { return ::close(fd); }
inline int _dup(int fd)                                    { return ::dup(fd); }
inline int _fileno(std::FILE* f)                           { return ::fileno(f); }

#endif // _WIN32

// Absolute path of the running executable, cross-platform. Empty on failure.
inline std::string PlatformExePath()
{
#if defined(_WIN32)
    char* pgmptr = nullptr;
    _get_pgmptr(&pgmptr);
    return pgmptr ? std::string(pgmptr) : std::string();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return std::string();
    return std::string(buf);
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    return std::string(buf);
#endif
}
