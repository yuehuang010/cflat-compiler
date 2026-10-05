#pragma once
// Namespace-scope class-typed constants, shaped like std::chrono::February, std::nullopt and the
// std tag objects (defer_lock, in_place_type<T>). Variable templates reach CFlat lazily (a
// decltype request); the plain ones are harvested at import.
namespace nscc {
struct Month { unsigned m; constexpr explicit Month(unsigned v) : m(v) {} constexpr unsigned value() const { return m; } };
inline constexpr Month February{2};
constexpr Month March{3};
inline const Month April{4};
struct none_t { struct secret { explicit secret() = default; }; constexpr explicit none_t(secret) {} };
inline constexpr none_t none{none_t::secret{}};
struct defer_t { explicit defer_t() = default; };
inline constexpr defer_t defer{};
// Trivially copyable with converting constructor templates, like std::optional<int>.
struct Opt {
    int v; bool has;
    Opt() : v(0), has(false) {}
    Opt(int x) : v(x), has(true) {}
    Opt(none_t) : v(0), has(false) {}
    template <class U> Opt(U* p) : v(p ? 1 : 0), has(p != nullptr) {}
    Opt& operator=(none_t) { has = false; v = 0; return *this; }
    bool has_value() const { return has; }
};
inline bool operator==(const Opt& o, none_t) { return !o.has; }
struct Lock { int mode; Lock(int) : mode(1) {} Lock(int, defer_t) : mode(2) {} };
inline const Month* addr_feb() { return &February; }
inline unsigned month_value(Month m) { return m.value(); }
inline unsigned month_ref(const Month& m) { return m.value(); }
template <class T> inline constexpr Month month_v{static_cast<unsigned>(sizeof(T))};
template <class T> constexpr Month cmonth_v{static_cast<unsigned>(sizeof(T) + 1)};
template <class T> struct type_tag { explicit type_tag() = default; };
template <class T> inline constexpr type_tag<T> type_tag_v{};
struct Picker { int k; Picker(type_tag<int>) : k(4) {} Picker(type_tag<double>) : k(8) {} };
inline const Month* addr_month_int() { return &month_v<int>; }
// Accept-set: scalar constants and enumerators keep folding.
inline constexpr int kPrim = 7;
constexpr double kHalf = 0.5;
template <class T> inline constexpr int size_v = (int)sizeof(T);
enum Color { Red = 1, Green = 2 };
}
