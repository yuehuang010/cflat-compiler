#include <string>
#include <any>
#include <functional>
#include <map>
#include <typeindex>
#include <typeinfo>
#include <cstdio>
int twice(int x) { return x * 2; }
int main()
{
    int failures = 0;
    std::any a = 5; std::any b = 7; std::any c = std::string("s"); std::any d = 2.5;
    if (a.type() != b.type() || a.type() == c.type() || a.type() == d.type() || !(a.type() == b.type())) { printf("FAIL any type\n"); failures |= 1; }
    std::function<int(int)> f = twice;
    std::function<int(int)> empty;
    if (f.target_type() == empty.target_type() || f.target_type() == a.type() || empty.target_type() == a.type()) { printf("FAIL target_type\n"); failures |= 2; }
    std::type_index ta(a.type()), tb(b.type()), tc(c.type()), td(d.type());
    if (!(ta == tb) || ta != tb || ta == tc || ta.hash_code() != tb.hash_code()) { printf("FAIL type_index eq\n"); failures |= 4; }
    if ((ta < tc) == (tc < ta) || (ta < tc) != a.type().before(c.type()) || ta < tb || tb < ta) { printf("FAIL type_index order\n"); failures |= 8; }
    std::map<std::type_index, int> counts;
    counts[ta] += 1; counts[tb] += 1; counts[tc] += 5; counts[td] += 7;
    if (counts.size() != 3 || counts[ta] != 2 || counts[tc] != 5 || counts[td] != 7) { printf("FAIL type_index map\n"); failures |= 16; }
    if (failures == 0) printf("PASS std_full_11_946_type_info_return\n");
    return failures;
}
