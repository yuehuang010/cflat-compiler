#pragma once
#include <memory>
#include <vector>
#include <map>
#include <utility>

// A CRTP base whose member templates read and write the derived class's fields by name.
namespace cppi_crtpf {
template<class D> struct Base : std::enable_shared_from_this<D> {
    std::vector<std::shared_ptr<D>> makev() { return {this->shared_from_this()}; }
    std::map<int, D> makem() { std::map<int, D> m; m.try_emplace(2); m.at(2).value = 73; return m; }
    std::pair<D, D> makep() { std::pair<D, D> p; p.first.value = 83; p.second.value = 89; return p; }
    // nslookup r4: clang's own size and offset of the derived class's `c` member.
    int self_size() { return (int)sizeof(D); }
    int c_offset() { D* self = static_cast<D*>(this); return (int)((char*)&self->c - (char*)self); }
};
// Free helpers for a [cpp] struct that has no CRTP base: clang's sizeof and offset of `c`.
template<class T> int cxx_size_of() { return (int)sizeof(T); }
template<class T> int cxx_c_offset() {
    alignas(64) unsigned char buf[sizeof(T)];
    T* self = reinterpret_cast<T*>(buf);
    return (int)((char*)&self->c - (char*)self);
}
}
