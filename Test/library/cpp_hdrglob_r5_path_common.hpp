#ifndef CFLAT_TEST_CPP_HDRGLOB_R5_PATH_COMMON_HPP
#define CFLAT_TEST_CPP_HDRGLOB_R5_PATH_COMMON_HPP

namespace hdrglob_r5_path {
inline int initializer_count;
inline int note(int value)
{
    ++initializer_count;
    return value;
}
static int S = note(3);
inline int count() { return initializer_count; }
}

#endif
