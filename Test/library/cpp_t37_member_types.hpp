// T37: member typedefs / using-aliases / nested types of C++ records named from CFlat
// as `Spec.name` in type position. Shaped like std::vector / std::iterator_traits.
#pragma once
#include <cstddef>

namespace t37 {

struct rand_tag {};
struct fwd_tag {};

template <typename T>
class vec {
public:
    typedef T value_type;
    using size_type = unsigned long;
    using pointer = T*;
    class iterator {
    public:
        using value_type = T;
        using iterator_category = rand_tag;
        T* p;
    };
    enum class mode { mode_a = 1, mode_b = 2 };
    struct node { using type = T; T v; };
    T items[4];
    int count;
    vec() : count(0) {}
    void push(T v) { items[count++] = v; }
    T at(int i) const { return items[i]; }
    static constexpr int kind = 7;
    int size_fn() const { return count; }
};

template <typename It>
struct traits {
    using value_type = typename It::value_type;
    using iterator_category = typename It::iterator_category;
};

template <typename T>
struct traits<T*> {
    using value_type = T;
    using iterator_category = rand_tag;
};

template <typename A, typename B> struct same { static constexpr bool value = false; };
template <typename A> struct same<A, A> { static constexpr bool value = true; };

struct plain {
    enum class color { color_r = 1, color_g = 2 };
    typedef long id_type;
    using label = int;
    struct inner { using type = short; int z; };
    int id_type_field;
    int label_count;
};

template <typename K, typename V>
struct dict {
    using key_type = K;
    using mapped_type = V;
};

inline int sum_vec(const vec<int>& v) { int s = 0; for (int i = 0; i < v.count; ++i) s += v.items[i]; return s; }

} // namespace t37

namespace t37 {
template <int N> struct fixed { int a[N]; static constexpr int n = N; };
template <bool B> struct flag { static constexpr bool value = B; };
}

namespace t37 {
// Copy / move constructors count separately: a by-value param's final use must move.
inline int track_copies = 0;
inline int track_moves = 0;
struct track {
    int v;
    track(int n) : v(n) {}
    track(const track& o) : v(o.v) { ++track_copies; }
    track(track&& o) : v(o.v) { ++track_moves; o.v = -1; }
    ~track() {}
};
inline int track_copy_count() { return track_copies; }
inline int track_move_count() { return track_moves; }
inline void track_reset() { track_copies = 0; track_moves = 0; }
}
