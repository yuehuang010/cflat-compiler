#pragma once
#line 2 "t26v_system/vbase.hpp"
// Lazily bound shapes for a virtual-base conversion: the base sits at a per-dynamic-type
// offset, so only a complete object of the static type may use the static layout offset.
namespace t26v {
struct base { int value = 731; };
// operator int makes pick(int) viable: C++ still ranks the base conversion first.
struct left : virtual base {
    long pad[2] = {111, 222};
    virtual ~left() {}
    operator int() const { return 42; }
};
struct sink {
    int saved = 0;
    sink() = default;
    sink(const base& v) : saved(v.value) {}
    int read(const base& v) const { return v.value; }
    int pick(const base& v) const { return 1000 + v.value; }
    int pick(int) const { return -1; }
};
inline int read(const base& v) { return v.value; }
inline int pick(const base& v) { return 1000 + v.value; }
inline int pick(int) { return -1; }
inline int bump(base& v) { return ++v.value; }
// The held object is a `both`: its `base` is not where a complete `left` keeps it.
struct right : virtual base { long pad[3] = {333, 444, 555}; virtual ~right() {} };
struct both : left, right { long extra[4] = {666, 667, 668, 669}; };
inline left& held() { static both object; return object; }

struct final_zero_base { virtual ~final_zero_base() {} virtual int get() const { return 731; } };
struct final_zero final : virtual final_zero_base { long pad[2] = {111, 222}; };
struct final_zero_sink {
    int saved = 0;
    final_zero_sink(const final_zero_base& v) : saved(v.get()) {}
    int read(const final_zero_base& v) const { return v.get(); }
};
inline final_zero& held_final_zero() { static final_zero object; return object; }

struct final_shift_base { int value = 742; };
struct final_shift final : virtual final_shift_base {
    long pad[2] = {111, 222};
    virtual ~final_shift() {}
};
struct final_shift_sink {
    int saved = 0;
    final_shift_sink(const final_shift_base& v) : saved(v.value) {}
    int read(const final_shift_base& v) const { return v.value; }
};
inline final_shift& held_final_shift() { static final_shift object; return object; }
}
