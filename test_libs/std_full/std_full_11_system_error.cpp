// C++20 equivalent of std_full_11_system_error.cb (compile-time parity baseline)
#include <string>
#include <system_error>
#include <cstring>
#include <cerrno>
#include <cstdio>

int main() {
    int failures = 0;
    std::error_code ec(EINVAL, std::generic_category());
    if (ec.value() != EINVAL || !static_cast<bool>(ec) || ec.category() != std::generic_category() || ec.message().empty()) { printf("FAIL error_code\n"); failures |= 1; }
    ec.clear();
    if (ec.value() != 0 || static_cast<bool>(ec)) { printf("FAIL error_code clear\n"); failures |= 2; }
    std::error_condition inval = std::error_condition(std::errc::invalid_argument);
    std::error_condition noent = std::error_condition(std::errc::no_such_file_or_directory);
    std::error_code mk = std::make_error_code(std::errc::invalid_argument);
    if (mk.value() != EINVAL || mk != inval || mk == noent || mk != std::error_code(EINVAL, std::generic_category()))
    { printf("FAIL make_error_code\n"); failures |= 4; }
    std::error_condition cond = std::make_error_condition(std::errc::invalid_argument);
    if (cond.value() != EINVAL || cond != inval || !static_cast<bool>(cond) || mk != cond || mk.default_error_condition() != cond)
    { printf("FAIL error_condition\n"); failures |= 8; }
    if (std::strcmp(std::generic_category().name(), "generic") != 0 || std::strcmp(std::system_category().name(), "system") != 0 || std::generic_category() == std::system_category())
    { printf("FAIL categories\n"); failures |= 16; }
    std::error_code sysec(ENOENT, std::system_category());
    if (sysec != noent || sysec.value() != ENOENT) { printf("FAIL system category\n"); failures |= 32; }
    std::system_error se(mk, "context");
    std::system_error se2(EINVAL, std::generic_category());
    std::string w(se.what());
    if (se.code() != mk || se2.code().value() != EINVAL || w.find("context") == std::string::npos || se.code().category().name() == nullptr)
    { printf("FAIL system_error\n"); failures |= 64; }
    std::error_code a(1, std::generic_category()), b(2, std::generic_category());
    if (!(a < b) || a == b || b < a) { printf("FAIL error_code order\n"); failures |= 128; }
    if (std::is_error_code_enum<std::errc>::value != false || !std::is_error_condition_enum<std::errc>::value) { printf("FAIL enum traits\n"); failures |= 256; }
    if (failures == 0) printf("PASS std_full_11_system_error\n");
    return failures;
}
