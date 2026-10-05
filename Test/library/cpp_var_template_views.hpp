#pragma once
// Variable templates of class type used as objects, shaped like std::views::empty<T> (an empty
// view whose begin/end/size are STATIC members over a CRTP interface base) and a non-empty one.
namespace vtv {
template <class D> struct view_base {
    constexpr bool is_empty() const { return static_cast<const D*>(this)->size() == 0; }
};
template <class T> struct empty_view : view_base<empty_view<T>> {
    static constexpr T* begin() noexcept { return nullptr; }
    static constexpr T* end() noexcept { return nullptr; }
    static constexpr int size() noexcept { return 0; }
};
template <class T> inline constexpr empty_view<T> empty{};
template <class T> struct Box { int tag = (int)sizeof(T) * 10; int get() const { return tag; } };
template <class T> inline constexpr Box<T> box{};
inline const void* addr_empty_int() { return &empty<int>; }
inline const void* addr_box_int() { return &box<int>; }
inline int box_ref(const Box<int>& b) { return b.get(); }
}
