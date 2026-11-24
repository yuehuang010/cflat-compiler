#pragma once
// CACHE-KEY: a hidden-friend operator whose return type comes from the template argument, so
// only the infix probe (__cflat_free_) reaches it. The argument's header is NOT included here:
// test_cpp_interop_template.cb and test_cpp_interop_bridge.cb pair this header with two different
// ckeyl::Leaf headers, so a wrapper keyed on this header alone collides across the two files.
namespace ckeyb {
template<class T> struct Box {
    T item;
    Box(T t) : item(t) {}
    friend auto operator*(const Box& b, int k) { return b.item.val() * k; }
};
}
