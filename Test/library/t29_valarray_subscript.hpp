#pragma once
#include <cstddef>

namespace t29sub {
template<class T> struct LibcScalarFirst {
    int operator[](std::size_t index) { return 10 + static_cast<int>(index); }
    int operator[](const LibcScalarFirst<bool>&) { return 20; }
    int operator[](LibcScalarFirst<bool>&&) { return 30; }
};
template<class T> struct LibcMaskFirst {
    int operator[](std::size_t index) { return 10 + static_cast<int>(index); }
    int operator[](const LibcMaskFirst<bool>&) { return 20; }
    int operator[](LibcMaskFirst<bool>&&) { return 30; }
};
template<class T> struct MsvcScalarFirst {
    int operator[](std::size_t index) { return 10 + static_cast<int>(index); }
    int operator[](MsvcScalarFirst<bool>&&) { return 30; }
    int operator[](const MsvcScalarFirst<bool>&) { return 20; }
};
template<class T> struct MsvcMaskFirst {
    int operator[](std::size_t index) { return 10 + static_cast<int>(index); }
    int operator[](MsvcMaskFirst<bool>&&) { return 30; }
    int operator[](const MsvcMaskFirst<bool>&) { return 20; }
};
}
