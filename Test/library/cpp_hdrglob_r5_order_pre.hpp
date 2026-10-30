#ifndef CFLAT_TEST_CPP_HDRGLOB_R5_ORDER_PRE_HPP
#define CFLAT_TEST_CPP_HDRGLOB_R5_ORDER_PRE_HPP

namespace hdrglob_r5_order {
inline int sequence;
inline int note(int digit)
{
    sequence = sequence * 10 + digit;
    return digit;
}
inline int sequence_now() { return sequence; }
}

#endif
