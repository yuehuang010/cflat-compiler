#include <string>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <sstream>
int main()
{
    int failures = 0;
    std::ostringstream explicit_out;
    explicit_out << "output";
    if (explicit_out.str() != "output") { printf("FAIL ostringstream\n"); failures |= 16; }
    std::stringstream ss;
    ss << "first";
    if (ss.str() != "first") { printf("FAIL stringstream output\n"); failures |= 1; }
    std::stringbuf buf("abc", std::ios::in | std::ios::out);
    buf.sputc('d');
    if (buf.str() != "dbc" || buf.in_avail() != 3) { printf("FAIL stringbuf operations\n"); failures |= 2; }
    buf.pubseekpos(0, std::ios::in);
    if (buf.sgetc() != 'd') { printf("FAIL stringbuf seek/read\n"); failures |= 2; }
    if (failures == 0) printf("PASS std_full_11_943_stringbuf_after_stream\n");
    return failures;
}
