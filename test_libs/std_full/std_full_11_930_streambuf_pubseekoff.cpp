#include <string>
#include <cstdio>
#include <sstream>
int main()
{
    std::stringbuf buf("abc", std::ios::in);
    auto pos = buf.pubseekoff(1, std::ios::beg, std::ios::in);
    if (pos != std::streampos(1) || buf.sgetc() != 'b') { std::puts("FAIL streambuf pubseekoff"); return 1; }
    std::puts("PASS std_full_11_930_streambuf_pubseekoff");
    return 0;
}
