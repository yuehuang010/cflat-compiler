#pragma once
#include <memory>

namespace cpp_t46 {
inline int itemGetCalls = 0;
inline int itemResetCalls = 0;
inline int holderGetCalls = 0;
inline int holderResetCalls = 0;

struct Item {
    int get() const { ++itemGetCalls; return 461; }
    int get(int) const { ++itemGetCalls; return 462; }
    int reset() { ++itemResetCalls; return 463; }
};

struct HolderBase {
    int reset() { ++holderResetCalls; return 471; }
    int inherited() const { return 472; }
};

template<class T> struct Ptr : HolderBase {
    T* value = nullptr;
    Ptr(T* p = nullptr) : value(p) {}
    T* get() const { ++holderGetCalls; return value; }
    T* operator->() const { return value; }
};

inline Item item;
inline Ptr<Item> make() { return Ptr<Item>(&item); }
inline std::unique_ptr<Item> make_unique() { return std::make_unique<Item>(); }
inline int item_get_calls() { return itemGetCalls; }
inline int item_reset_calls() { return itemResetCalls; }
inline int holder_get_calls() { return holderGetCalls; }
inline int holder_reset_calls() { return holderResetCalls; }
}
