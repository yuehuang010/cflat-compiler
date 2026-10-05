#include <string>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <ctime>

int main()
{
    int failures = 0;
    std::ostringstream manip;
    manip << "x" << std::hex << 26 << std::boolalpha << true << std::endl;
    if (manip.str() != "x1atrue\n") failures |= 256;
    std::ostringstream upper; upper << std::showbase << std::hex << std::uppercase << 255;
    if (upper.str() != "0XFF") failures |= 512;
    std::ostringstream formatted;
    formatted << std::showbase << std::hex << 16 << ' ' << std::fixed << std::setprecision(2) << 1.5;
    if (formatted.str() != "0x10 1.50") failures |= 1;
    std::ostringstream padded;
    padded << std::setfill('0') << std::setw(4) << 12;
    if (padded.str() != "0012" || padded.tellp() != std::streampos(4)) failures |= 2;

    std::ostringstream putwrite;
    putwrite.write("xy", 2); putwrite.put('z');
    if (putwrite.str() != "xyz" || putwrite.tellp() != std::streampos(3)) failures |= 16;
    std::ostringstream sourcefmt;
    sourcefmt << std::showpos;
    sourcefmt.precision(3); sourcefmt.width(5); sourcefmt.fill('_');
    bool fmt_state = sourcefmt.precision() == 3 && sourcefmt.width() == 5 && sourcefmt.fill() == '_';
    sourcefmt << 12;
    if (!fmt_state || sourcefmt.str() != "__+12") failures |= 32;
    std::tm date{};
    date.tm_year = 124; date.tm_mon = 1; date.tm_mday = 3;
    std::ostringstream dateout; dateout << std::put_time(&date, "%Y-%m-%d");
    std::tm parsed{};
    std::istringstream datein("2024-02-03"); datein >> std::get_time(&parsed, "%Y-%m-%d");
    if (dateout.str() != "2024-02-03" || parsed.tm_year != 124 || parsed.tm_mon != 1 || parsed.tm_mday != 3) failures |= 64;

    std::istringstream input("12 alpha\nbeta");
    int n = 0;
    std::string word, line;
    input >> n >> word;
    input.get();
    std::getline(input, line);
    if (n != 12 || word != "alpha" || line != "beta") failures |= 4;
    input.clear();
    input.str("xy");
    char ch = 0;
    input.get(ch);
    int peeked = input.peek();
    input.ignore();
    std::streamsize ignored = input.gcount();
    input.get(ch);
    if (ch != 'x' || peeked != 'y' || ignored != 1 || !input.eof()) failures |= 8;
    input.clear(); input.seekg(1); input.get(ch); input.unget(); input.get(ch);
    if (ch != 'y') failures |= 128;
    std::istringstream bad_input("word"); int invalid = 0; bad_input >> invalid;
    bool fail_state = bad_input.fail() && !bad_input.bad() && (bad_input.rdstate() & std::ios::failbit) != 0 && !bad_input;
    bad_input.clear(); bad_input.setstate(std::ios::badbit);
    bool bad_state = bad_input.bad() && (bad_input.rdstate() & std::ios::badbit) != 0 && !bad_input;
    bad_input.clear();
    if (!fail_state || !bad_state || bad_input.rdstate() != 0 || !bad_input) failures |= 256;
    if (failures & 1) std::puts("FAIL ostream formatting");
    if (failures & 2) std::puts("FAIL width fill tellp");
    if (failures & 4) std::puts("FAIL formatted input getline");
    if (failures & 8) std::puts("FAIL get peek ignore gcount state");
    if (failures & 16) std::puts("FAIL ostream put/write/tellp");
    if (failures & 32) std::puts("FAIL ios_base flags/precision/width/fill");
    if (failures & 64) std::puts("FAIL put_time/get_time C locale");
    if (failures & 128) std::puts("FAIL unget/clear");
    if (failures & 256) std::puts("FAIL endl/hex/boolalpha");
    if (failures & 16) std::puts("FAIL ostream put/write/tellp");
    if (failures & 32) std::puts("FAIL ios_base flags/precision/width/fill");
    if (failures & 64) std::puts("FAIL put_time/get_time C locale");
    if (failures & 128) std::puts("FAIL unget/clear");
    if (failures & 256) std::puts("FAIL endl/hex/boolalpha");
    if (failures == 0) std::puts("PASS std_full_11_iostreams");
    return failures;
}
