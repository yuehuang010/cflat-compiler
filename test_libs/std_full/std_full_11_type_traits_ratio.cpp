// C++20 equivalent of std_full_11_type_traits_ratio.cb (compile-time parity baseline)
#include <string>
#include <type_traits>
#include <ratio>
#include <memory>
#include <stdexcept>
#include <vector>
#include <array>
#include <functional>
#include <cstddef>
#include <cstdio>

int main() {
    int failures = 0;
    if (!std::is_integral<int>::value || std::is_integral<double>::value || !std::is_floating_point<double>::value || !std::is_pointer<int*>::value
        || std::is_pointer<int>::value || !std::is_enum<std::byte>::value || !std::is_class<std::string>::value || std::is_class<int>::value
        || !std::is_void<void>::value
        || !std::is_arithmetic<float>::value || !std::is_fundamental<int>::value || !std::is_scalar<std::byte>::value || !std::is_compound<std::string>::value
        || !std::is_object<int>::value || !std::is_null_pointer<std::nullptr_t>::value || std::is_function<int>::value)
    { printf("FAIL categories\n"); failures |= 1; }
    if (!std::is_trivially_copyable<std::array<int, 3>>::value || !std::is_standard_layout<std::array<int, 3>>::value || std::is_trivially_copyable<std::string>::value
        || !std::is_signed<int>::value || std::is_signed<unsigned>::value || !std::is_unsigned<unsigned char>::value
        || !std::is_empty<std::true_type>::value || std::is_empty<std::string>::value || std::is_final<std::string>::value
        || !std::is_aggregate<std::array<int, 3>>::value || std::is_abstract<std::string>::value || std::is_polymorphic<std::string>::value || !std::is_polymorphic<std::exception>::value
        || !std::has_virtual_destructor<std::exception>::value)
    { printf("FAIL properties\n"); failures |= 2; }
    if (!std::is_constructible<std::string, const char*>::value || std::is_constructible<std::string, std::vector<int>>::value || !std::is_nothrow_move_constructible<std::string>::value
        || std::is_copy_constructible<std::unique_ptr<int>>::value || !std::is_move_constructible<std::unique_ptr<int>>::value || std::is_copy_assignable<std::unique_ptr<int>>::value
        || !std::is_move_assignable<std::unique_ptr<int>>::value || !std::is_default_constructible<std::string>::value || !std::is_destructible<std::string>::value
        || !std::is_trivially_destructible<std::array<int, 3>>::value || std::is_trivially_destructible<std::string>::value || !std::is_swappable<int>::value
        || !std::is_nothrow_destructible<std::string>::value)
    { printf("FAIL operations\n"); failures |= 4; }
    if (!std::is_same<int, int>::value || std::is_same<int, long>::value || !std::is_base_of<std::exception, std::runtime_error>::value || std::is_base_of<std::runtime_error, std::exception>::value
        || !std::is_convertible<int, double>::value || std::is_convertible<std::string, int>::value
        || !std::is_convertible<std::runtime_error*, std::exception*>::value || std::is_convertible<std::exception*, std::runtime_error*>::value
        || !std::is_invocable<std::function<int(int)>, int>::value || std::is_invocable<std::function<int(int)>, std::string>::value
        || !std::is_same<std::invoke_result_t<std::function<double(int)>, int>, double>::value)
    { printf("FAIL relationships\n"); failures |= 8; }
    if (!std::is_same<std::make_unsigned_t<int>, unsigned>::value
        || !std::is_same<std::make_signed_t<unsigned char>, signed char>::value || !std::is_same<std::common_type_t<int, double>, double>::value
        || !std::is_same<std::underlying_type_t<std::byte>, unsigned char>::value || !std::is_same<std::remove_pointer_t<int*>, int>::value || !std::is_same<std::add_pointer_t<int>, int*>::value
       
        || !std::is_same<std::conditional_t<true, int, long>, int>::value
        || !std::is_same<std::conditional_t<false, int, long>, long>::value || !std::is_same<std::enable_if_t<true, short>, short>::value)
    { printf("FAIL transformations\n"); failures |= 16; }
    using three = std::integral_constant<int, 3>;
    using yes = std::bool_constant<true>;
    if (three::value != 3 || (int)three() != 3 || !yes::value || !std::true_type::value || std::false_type::value || std::is_constant_evaluated()
        || std::alignment_of<double>::value < 4 || !std::negation<std::false_type>::value || !std::conjunction<yes, std::true_type>::value || std::disjunction<std::false_type, std::false_type>::value)
    { printf("FAIL constants\n"); failures |= 32; }
    using third = std::ratio<1, 3>; using sixth = std::ratio<1, 6>;
    using sum = std::ratio_add<third, sixth>;
    using prod = std::ratio_multiply<std::ratio<2, 3>, std::ratio<3, 4>>;
    using diff = std::ratio_subtract<third, sixth>;
    using quo = std::ratio_divide<third, sixth>;
    if (third::num != 1 || third::den != 3 || sum::num != 1 || sum::den != 2 || prod::num != 1 || prod::den != 2 || diff::num != 1 || diff::den != 6 || quo::num != 2 || quo::den != 1
        || std::ratio<4, 6>::num != 2 || std::ratio<4, 6>::den != 3 || std::ratio<-1, 2>::num != -1)
    { printf("FAIL ratio arithmetic\n"); failures |= 64; }
    if (!std::ratio_less<sixth, third>::value || std::ratio_less<third, sixth>::value || !std::ratio_equal<std::ratio<2, 4>, std::ratio<1, 2>>::value || !std::ratio_greater<third, sixth>::value
        || !std::ratio_less_equal<third, third>::value || !std::ratio_not_equal<third, sixth>::value || !std::ratio_greater_equal<third, third>::value)
    { printf("FAIL ratio comparison\n"); failures |= 128; }
    if (std::milli::den != 1000 || std::milli::num != 1 || std::kilo::num != 1000 || std::kilo::den != 1 || std::micro::den != 1000000 || std::mega::num != 1000000 || std::centi::den != 100 || std::deca::num != 10)
    { printf("FAIL SI typedefs\n"); failures |= 256; }
    if (failures == 0) printf("PASS std_full_11_type_traits_ratio\n");
    return failures;
}
