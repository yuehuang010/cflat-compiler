#include <memory_resource>
#include <cstdio>
int main()
{
    char backing[1024] = {};
    std::pmr::monotonic_buffer_resource fixed(backing, 1024, std::pmr::null_memory_resource());
    std::puts("PASS std_full_17_903_char_ptr_to_void_ptr_ctor");
    return 0;
}
