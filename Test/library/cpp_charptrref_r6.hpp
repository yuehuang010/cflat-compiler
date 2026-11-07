#pragma once

namespace cpp_charptrref_r6 {
inline int a6(const char* const& value) { return value == nullptr ? 1 : 3; }
inline int a6(const char* value) { return value == nullptr ? 2 : 4; }
inline int b6(const char* const& value) { return value == nullptr ? 1 : 3; }
inline int b6(const void* value) { return value == nullptr ? 2 : 4; }
}
