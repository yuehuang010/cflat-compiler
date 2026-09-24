#pragma once

namespace cppsetcopy {
struct CopyBase {
    int tag = 0;
    CopyBase() = default;
    bool operator<(const CopyBase& other) const { return tag < other.tag; }
};
}
