// C++20 equivalent of std_full_20_bit_numbers_compare_span.cb
#include <string>
#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    unsigned int u = 5;
    if (std::bit_ceil(u) != 8u || std::bit_floor(u) != 4u || std::bit_ceil(8u) != 8u || std::rotr(1u, 1) != 0x80000000u || std::rotl(0x80000000u, 1) != 1u || std::countl_zero(1u) != 31 || std::countl_one(0xF0000000u) != 4 || std::countr_zero(8u) != 3 || std::countr_one(7u) != 3 || std::popcount(255u) != 8 || std::bit_width(8u) != 4 || std::has_single_bit(6u)) { std::printf("FAIL bit_ops\n"); failures |= 1; }
    if (std::countl_zero((unsigned char)1) != 7 || std::countr_zero((unsigned long long)0) != 64 || std::bit_floor(0u) != 0u || std::rotr((unsigned char)1, 1) != 128) { std::printf("FAIL bit_widths\n"); failures |= 2; }
    if (!(std::numbers::e > 2.7182 && std::numbers::e < 2.7183) || !(std::numbers::pi > 3.14159 && std::numbers::pi < 3.1416) || !(std::numbers::sqrt2 > 1.4142 && std::numbers::sqrt2 < 1.4143) || !(std::numbers::ln2 > 0.6931 && std::numbers::ln2 < 0.6932) || !(std::numbers::phi > 1.6180 && std::numbers::phi < 1.6181)) { std::printf("FAIL numbers_double\n"); failures |= 4; }
    float pf = std::numbers::pi_v<float>;
    float ef = std::numbers::e_v<float>;
    if (!(pf > 3.14159f && pf < 3.1416f) || !(ef > 2.7182f && ef < 2.7183f) || !(std::numbers::inv_pi > 0.3183 && std::numbers::inv_pi < 0.3184) || !(std::numbers::sqrt3 > 1.7320 && std::numbers::sqrt3 < 1.7321) || !(std::numbers::ln10 > 2.3025 && std::numbers::ln10 < 2.3026) || !(std::numbers::log2e > 1.4426 && std::numbers::log2e < 1.4427) || !(std::numbers::log10e > 0.4342 && std::numbers::log10e < 0.4343) || !(std::numbers::egamma > 0.5772 && std::numbers::egamma < 0.5773) || !(std::numbers::inv_sqrt3 > 0.5773 && std::numbers::inv_sqrt3 < 0.5774) || !(std::numbers::inv_sqrtpi > 0.5641 && std::numbers::inv_sqrtpi < 0.5642)) { std::printf("FAIL numbers_float_more\n"); failures |= 8; }
    auto c1 = std::compare_three_way()(1, 2);
    auto c2 = std::strong_order(2.0, 1.0);
    auto c3 = std::weak_order(1.0, 1.0);
    auto c4 = std::partial_order(1.0, 2.0);
    if (!std::is_lt(c1) || !std::is_gt(c2) || !std::is_eq(c3) || !std::is_lt(c4)) { std::printf("FAIL compare_three_way\n"); failures |= 32; }
    std::strong_ordering slt = std::strong_ordering::less;
    std::strong_ordering seq = std::strong_ordering::equal;
    std::strong_ordering sgt = std::strong_ordering::greater;
    std::weak_ordering wlt = std::weak_ordering::less;
    std::weak_ordering weq = std::weak_ordering::equivalent;
    std::partial_ordering pun = std::partial_ordering::unordered;
    std::partial_ordering peq = std::partial_ordering::equivalent;
    auto c5 = std::compare_three_way()(3, 3);
    if (!std::is_lt(slt) || !std::is_eq(seq) || !std::is_gt(sgt) || !std::is_lt(wlt) || !std::is_eq(weq) || std::is_lt(pun) || std::is_gteq(pun) || std::is_eq(pun) || !std::is_eq(peq) || c1 != slt || c5 != seq || c2 != sgt) { std::printf("FAIL ordering_values\n"); failures |= 512; }
    std::vector<int> vec{1, 2, 3, 4};
    std::span<int, 4> fixed(vec.data(), 4);
    std::span<int> dyn(vec);
    std::span<int> cs(vec);
    if (decltype(fixed)::extent != 4 || fixed.size() != 4 || fixed.extent != 4 || dyn.size() != 4 || std::dynamic_extent == 0 || fixed.size_bytes() != 4 * sizeof(int) || dyn.size_bytes() != 16 || cs[2] != 3 || fixed.front() != 1 || dyn.back() != 4 || dyn.empty()) { std::printf("FAIL span_extent\n"); failures |= 64; }
    auto fs = fixed.first<2>();
    auto ls = dyn.last(2);
    auto ss = dyn.subspan(1, 2);
    if (fs.size() != 2 || fs[1] != 2 || ls[0] != 3 || ss[0] != 2 || ss.size() != 2 || dyn.data() != vec.data() || *dyn.begin() != 1 || *(dyn.end() - 1) != 4) { std::printf("FAIL span_subviews\n"); failures |= 128; }
    auto rb = std::as_bytes(dyn);
    auto wb = std::as_writable_bytes(dyn);
    wb[0] = std::byte{9};
    if (rb.size() != 16 || vec[0] != 9 || rb.size_bytes() != 16) { std::printf("FAIL as_bytes\n"); failures |= 256; }
    if (failures == 0) std::printf("PASS std_full_20_bit_numbers_compare_span\n");
    return failures;
}
