#include <string>
#include <cstdio>
#include <iostream>
#include <sstream>
int main()
{
    std::ostringstream source;
    source << std::showpos;
    source.precision(3); source.width(5); source.fill('_');
    std::ostringstream target;
    target.copyfmt(source);
    bool state = target.precision() == 3 && target.width() == 5 && target.fill() == '_';
    target << 12;
    if (!state || target.str() != "__+12") { std::puts("FAIL ios_base copyfmt state"); return 1; }
    std::puts("PASS std_full_11_917_ios_copyfmt");
    return 0;
}
