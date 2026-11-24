#pragma once

namespace cj
{
    inline int* getm_() { static int value = 4; return &value; }
    inline int* getm_alt() { static int value = 6; return &value; }
    inline const int* getc_() { static int value = 8; return &value; }
    inline const int** getcpp_() { static const int* value = nullptr; return &value; }
    inline int is_cpp(void* value) { return value == (void*)getcpp_(); }
    inline int is_c(const void* value) { return value == getc_(); }
    inline int pick_void(void*) { return 1; }
    inline int pick_void(const void*) { return 2; }
    inline int take_void(void* value)
    {
        if (value == getm_()) return 31;
        if (value == getm_alt()) return 32;
        return 39;
    }
    struct CS
    {
        int value;
        CS(void* pointer)
            : value(pointer == getm_() ? 41 : pointer == getm_alt() ? 42 : 49) { }
    };
    struct JoinMethodHost
    {
        int* m() { return getm_(); }
        const int* c() { return getc_(); }
        int take_void(void* value) { return value != nullptr ? 1 : 0; }
        int take_cvoid(const void* value) { return value != nullptr ? 1 : 0; }
    };
}
