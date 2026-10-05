#include <string>
#include <cmath>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv)
{
    int failures = 0;
    volatile double v25 = 2.5, v24 = 2.4, vn25 = -2.5, v1 = 1.0, v3 = 3.0;
    if (std::ceil(v24) != 3.0 || std::floor(v25) != 2.0 || std::trunc(vn25) != -2.0 || std::round(v25) != 3.0 || std::round(vn25) != -3.0
        || std::lround(v25) != 3L || std::llround(vn25) != -3LL || std::nearbyint(v25) != 2.0 || std::rint(v25) != 2.0 || std::lrint(v25) != 2L)
    { printf("FAIL rounding\n"); failures |= 1; }
    int ex = 0;
    double fr = std::frexp(8.0, &ex);
    double ip = 0.0;
    double fp = std::modf(3.75, &ip);
    double ip2 = 0.0;
    double fp2 = std::modf(-3.75, &ip2);
    if (fr != 0.5 || ex != 4 || ip != 3.0 || fp != 0.75 || ip2 != -3.0 || fp2 != -0.75 || std::ldexp(0.5, 4) != 8.0 || std::ldexp(3.0, -1) != 1.5)
    { printf("FAIL frexp/modf/ldexp\n"); failures |= 2; }
    if (std::copysign(3.0, -0.0) != -3.0 || std::copysign(-3.0, 1.0) != 3.0 || !(std::nextafter(1.0, 2.0) > 1.0) || !(std::nextafter(1.0, 0.0) < 1.0) || std::nextafter(1.0, 1.0) != 1.0
        || std::fabs(-4.5) != 4.5 || std::fmod(7.0, 4.0) != 3.0 || std::remainder(7.0, 4.0) != -1.0 || std::exp2(3.0) != 8.0 || std::sqrt(16.0) != 4.0 || std::cbrt(27.0) != 3.0
        || std::pow(2.0, 10.0) != 1024.0 || std::fmax(1.0, 2.0) != 2.0 || std::fmin(1.0, 2.0) != 1.0 || std::fdim(5.0, 3.0) != 2.0)
    { printf("FAIL copysign/nextafter/basic\n"); failures |= 4; }
    double zero = 0.0 * (double)argc;
    double inf = 1.0 / zero;
    double nan_ = zero / zero;
    if (std::fpclassify(1.0) != FP_NORMAL || std::fpclassify(0.0) != FP_ZERO || std::fpclassify(inf) != FP_INFINITE || std::fpclassify(nan_) != FP_NAN)
    { printf("FAIL fpclassify macros\n"); failures |= 512; }
    if (!std::signbit(-1.0) || std::signbit(1.0) || !std::isinf(inf) || std::isinf(1.0) || !std::isnan(nan_) || !std::isfinite(1.0) || std::isfinite(inf) || !std::isnormal(1.0))
    { printf("FAIL classification\n"); failures |= 8; }
    if (std::sin(zero) != 0.0 || std::cos(zero) != 1.0 || std::tan(zero) != 0.0 || std::atan2(zero, 1.0) != 0.0 || std::cosh(zero) != 1.0 || std::sinh(zero) != 0.0 || std::exp(zero) != 1.0 || std::log(1.0) != 0.0 || std::asin(zero) != 0.0 || std::acos(1.0) != 0.0)
    { printf("FAIL trig at zero\n"); failures |= 16; }
    if (std::hypot(2.0, 3.0, 6.0) != 7.0 || std::lerp(0.0, 10.0, 0.5) != 5.0) { printf("FAIL hypot3/lerp\n"); failures |= 32; }
    int mode0 = std::fegetround();
    bool defok = mode0 == FE_TONEAREST;
    // LLVM optimizes rint as rounding-mode independent at -O2.
    std::fesetround(FE_UPWARD);
    bool upok = std::fegetround() == FE_UPWARD;
    std::fesetround(FE_DOWNWARD);
    bool dnok = std::fegetround() == FE_DOWNWARD;
    std::fesetround(FE_TOWARDZERO);
    bool tzok = std::fegetround() == FE_TOWARDZERO;
    std::fesetround(mode0);
    if (!defok || !upok || !dnok || !tzok || std::fegetround() != FE_TONEAREST)
    { printf("FAIL rounding mode\n"); failures |= 64; }
    std::fenv_t env;
    std::fegetenv(&env);
    std::fesetround(FE_UPWARD);
    bool changed = std::fegetround() == FE_UPWARD;
    std::fesetenv(&env);
    if (!changed || std::fegetround() != FE_TONEAREST) { printf("FAIL fegetenv/fesetenv\n"); failures |= 128; }
    if (failures == 0) printf("PASS std_full_11_cmath_cfenv\n");
    return failures;
}
