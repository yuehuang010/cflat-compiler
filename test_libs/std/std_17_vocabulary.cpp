// C++20 equivalent of std_17_vocabulary.cb (compile-time parity baseline)
#include <string>
#include <any>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>
#include <cstdio>

static int plus_one(int x) { return x + 1; }

int main() {
    int failures = 0;
    std::optional<int> o; o.emplace(23); int ov = o.value_or(0); o.reset(); bool empty = !o.has_value();
    std::variant<int> v(std::in_place_index<0>, 37);
    std::size_t ix = v.index(); bool holds = std::holds_alternative<int>(v);
    int* ptr = std::get_if<int>(&v); std::function<int(int)> visitor(plus_one); int visited = std::visit(visitor, v);
    std::any av = 41; int anyv = std::any_cast<int>(av); bool anyhas = av.has_value();
    std::string_view sv("abcdef"); auto sub = sv.substr(2, 3); auto pos = sv.find("cd"); sv.remove_prefix(1);
    bool cmp = (sv == std::string("bcdef"));
#define CHECK(name, expr) do { if (!(expr)) { std::printf("FAIL %s\n", name); failures |= 1; } } while (0)
    CHECK("optional", ov == 23 && empty); CHECK("variant_state", ix == 0 && holds && ptr && *ptr == 37 && visited == 38);
CHECK("any", anyv == 41 && anyhas);
    CHECK("string_view", sub == "cde" && pos == 2 && cmp);
    if (!failures) std::printf("PASS std_17_vocabulary\n");
    return failures;
}
