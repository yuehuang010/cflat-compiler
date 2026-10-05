// C++20 equivalent of std_full_17_vocabulary_charconv.cb
#include <string>
#include <any>
#include <charconv>
#include <optional>
#include <string_view>
#include <variant>
#include <cstdio>
#include <functional>

static int plus2(int x, int y) { return x + y; }

int main() {
    int failures = 0;
    std::optional<int> a = std::make_optional(5);
    std::optional<int> b;
    std::optional<int> c = std::optional<int>();
    if (!a || b || *a != 5 || a.value() != 5 || !(b < a) || !(a > c) || a != std::make_optional(5) || !(c == b) || a.value_or(9) != 5 || b.value_or(9) != 9) { std::printf("FAIL optional_compare\n"); failures |= 1; }
    b = 8;
    a.swap(b);
    std::swap(a, c);
    if (*b != 5 || *c != 8 || a.has_value()) { std::printf("FAIL optional_swap\n"); failures |= 2; }
    auto em = b.emplace(77);
    if (b.value() != 77 || em != 77) { std::printf("FAIL optional_emplace\n"); failures |= 4; }
    std::variant<std::monostate, int, double> v;
    if (v.index() != 0 || !std::holds_alternative<std::monostate>(v) || std::variant_size_v<decltype(v)> != 3) { std::printf("FAIL variant_monostate\n"); failures |= 8; }
    v.emplace<int>(4);
    bool ok = v.index() == 1 && std::get<int>(v) == 4 && std::get_if<double>(&v) == nullptr;
    v = 2.5;
    ok = ok && v.index() == 2 && std::get<double>(v) == 2.5;
    v.emplace<1>(9);
    ok = ok && std::get<1>(v) == 9;
    if (!ok) { std::printf("FAIL variant_emplace\n"); failures |= 16; }
    std::variant<int> v1 = 3;
    std::variant<int> v2 = 4;
    std::function<int(int, int)> sum = plus2;
    int s1 = std::visit(sum, v1, v2);
    int s2 = std::visit(sum, v1, v1);
    if (s1 != 7 || s2 != 6) { std::printf("FAIL variant_visit2\n"); failures |= 32; }
    std::any an = std::make_any<int>(41);
    bool any_ok = an.has_value() && std::any_cast<int>(an) == 41;
    an.reset();
    any_ok = any_ok && !an.has_value();
    an = std::string("hi");
    any_ok = any_ok && std::any_cast<std::string>(an) == "hi" && std::any_cast<int>(&an) == nullptr;
    if (!any_ok) { std::printf("FAIL any\n"); failures |= 64; }
    char buf[64];
    auto r1 = std::to_chars(buf, buf + 64, 1.5);
    std::string s_short(buf, r1.ptr);
    auto r2 = std::to_chars(buf, buf + 64, 2.25, std::chars_format::fixed, 3);
    std::string s_fixed(buf, r2.ptr);
    auto r3 = std::to_chars(buf, buf + 64, 0.5f);
    std::string s_float(buf, r3.ptr);
    if (r1.ec != std::errc() || s_short != "1.5" || s_fixed != "2.250" || s_float != "0.5") { std::printf("FAIL to_chars_float\n"); failures |= 128; }
    double dv = 0.0;
    std::string num = "3.75xyz";
    auto fr = std::from_chars(num.data(), num.data() + num.size(), dv);
    float fv = 0.0f;
    std::string num2 = "-0.5";
    auto fr2 = std::from_chars(num2.data(), num2.data() + num2.size(), fv);
    int iv = 0;
    std::string bad = "xyz";
    auto fr3 = std::from_chars(bad.data(), bad.data() + bad.size(), iv);
    int hv = 0;
    std::string hex = "ff";
    auto fr4 = std::from_chars(hex.data(), hex.data() + hex.size(), hv, 16);
    if (fr.ec != std::errc() || dv != 3.75 || *fr.ptr != 'x' || fr2.ec != std::errc() || fv != -0.5f || fr3.ec != std::errc::invalid_argument || fr4.ec != std::errc() || hv != 255) { std::printf("FAIL from_chars\n"); failures |= 256; }
    if (failures == 0) std::printf("PASS std_full_17_vocabulary_charconv\n");
    return failures;
}
