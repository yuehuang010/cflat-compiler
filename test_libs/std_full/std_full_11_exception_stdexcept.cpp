// C++20 equivalent of std_full_11_exception_stdexcept.cb (compile-time parity baseline)
#include <string>
#include <exception>
#include <stdexcept>
#include <cstring>
#include <cstdio>

int terminate_calls = 0;
void my_terminate() { terminate_calls++; }

static bool same(const char* w, const char* want) { return w != nullptr && std::strcmp(w, want) == 0; }

int main() {
    int failures = 0;
    std::exception e;
    if (e.what() == nullptr) { printf("FAIL exception what\n"); failures |= 1; }
    if (std::uncaught_exceptions() != 0) { printf("FAIL uncaught\n"); failures |= 2; }
    std::terminate_handler old = std::set_terminate(my_terminate);
    std::terminate_handler cur = std::get_terminate();
    std::set_terminate(old);
    std::terminate_handler back = std::get_terminate();
    cur();
    if (terminate_calls != 1 || ((back == nullptr) != (old == nullptr))) { printf("FAIL terminate handler\n"); failures |= 4; }
    std::exception_ptr np;
    std::exception_ptr ep = std::make_exception_ptr(std::runtime_error("boom"));
    if (static_cast<bool>(np) || !static_cast<bool>(ep) || np != nullptr || ep == nullptr) { printf("FAIL exception_ptr\n"); failures |= 8; }
    std::string s1 = "from string";
    std::logic_error le1("logic c"); std::logic_error le2(s1);
    std::invalid_argument ia1("inv c"); std::invalid_argument ia2(s1);
    std::domain_error de1("dom c"); std::domain_error de2(s1);
    std::length_error ln1("len c"); std::length_error ln2(s1);
    std::out_of_range oo1("oor c"); std::out_of_range oo2(s1);
    if (!same(le1.what(), "logic c") || !same(le2.what(), "from string") || !same(ia1.what(), "inv c") || !same(ia2.what(), "from string")
        || !same(de1.what(), "dom c") || !same(de2.what(), "from string") || !same(ln1.what(), "len c") || !same(ln2.what(), "from string")
        || !same(oo1.what(), "oor c") || !same(oo2.what(), "from string"))
    { printf("FAIL logic family\n"); failures |= 16; }
    std::runtime_error re1("run c"); std::runtime_error re2(s1);
    std::range_error ra1("range c"); std::range_error ra2(s1);
    std::overflow_error ov1("over c"); std::overflow_error ov2(s1);
    std::underflow_error un1("under c"); std::underflow_error un2(s1);
    if (!same(re1.what(), "run c") || !same(re2.what(), "from string") || !same(ra1.what(), "range c") || !same(ra2.what(), "from string")
        || !same(ov1.what(), "over c") || !same(ov2.what(), "from string") || !same(un1.what(), "under c") || !same(un2.what(), "from string"))
    { printf("FAIL runtime family\n"); failures |= 32; }
    std::exception& base = oo1;
    std::exception& base2 = ov1;
    std::runtime_error copy = re1;
    if (!same(base.what(), "oor c") || !same(base2.what(), "over c") || !same(copy.what(), "run c")) { printf("FAIL slicing\n"); failures |= 64; }
    std::logic_error& lref = ia1;
    if (!same(lref.what(), "inv c")) { printf("FAIL logic base\n"); failures |= 128; }
    if (failures == 0) printf("PASS std_full_11_exception_stdexcept\n");
    return failures;
}
