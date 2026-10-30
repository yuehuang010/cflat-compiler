#ifndef CFLAT_TEST_CPP_HDRGLOB_COMMON_HPP
#define CFLAT_TEST_CPP_HDRGLOB_COMMON_HPP

namespace hdrglob {
inline int counter;
inline int sequence;
inline int first_value = (++counter, sequence = sequence * 10 + 1, 10);
inline const int dynamic_const = (++counter, sequence = sequence * 10 + 2, 27);
struct Registrar {
    Registrar() { ++counter; sequence = sequence * 10 + 3; }
};
inline Registrar registrar;
inline int referenced = (++counter, sequence = sequence * 10 + 4, 40);
inline int counter_now() { return counter; }
inline int sequence_now() { return sequence; }
inline int referenced_now() { return referenced; }
namespace nested {
inline int dynamic_value = (++hdrglob::counter,
                            hdrglob::sequence = hdrglob::sequence * 10 + 5, 70);
}
inline constexpr int constexpr_value = 41;
inline const int constant_value = 42;
template <class T> inline int variable_template = (++counter, 80);
template <class T> struct Holder {
    static inline int static_data = (++counter, 90);
};
}

#endif
