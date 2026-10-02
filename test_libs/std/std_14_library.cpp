// C++20 equivalent of std_14_library.cb (compile-time parity baseline)
#include <string>
#include <algorithm>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <utility>
#include <cstdio>

int main() {
    int failures = 0;
    auto p = std::make_unique<int>(17);
    int old = std::exchange(*p, 19);
    std::map<std::string, int, std::less<>> m; m.emplace("key", 31);
    int hetero = m.find("key")->second;
    auto t = std::make_tuple(7, 2.5);
    int by_type = std::get<int>(t);
    int raw[] = {4, 5};
    int begin = *std::cbegin(raw);
    int reverse = *std::crbegin(raw);
    int free_reverse = *std::rbegin(raw);
    int eq = std::equal(raw, raw + 2, std::begin(raw), std::end(raw));
    auto mm = std::mismatch(raw, raw + 2, std::begin(raw), std::end(raw));
    std::shared_timed_mutex mutex; int locked = 0;
    { std::shared_lock<std::shared_timed_mutex> lock(mutex); locked = 1; }
    std::stringstream ss; ss << std::quoted("hello world");
    std::string quoted; ss >> std::quoted(quoted);
    bool traits = std::is_final<std::string>::value == false;
#define CHECK(name, expr) do { if (!(expr)) { std::printf("FAIL %s\n", name); failures |= 1; } } while (0)
    CHECK("make_unique", *p == 19);
    CHECK("exchange", old == 17); CHECK("heterogeneous_find", hetero == 31);
    CHECK("get_type", by_type == 7); CHECK("iterators", begin == 4 && reverse == 5 && free_reverse == 5);
    CHECK("four_iterator_equal", eq); CHECK("four_iterator_mismatch", mm.first == raw + 2 && mm.second == raw + 2);
    CHECK("shared_lock", locked == 1); CHECK("quoted", quoted == "hello world"); CHECK("traits", traits);
    if (!failures) std::printf("PASS std_14_library\n");
    return failures;
}
