// C++20 equivalent of std_11_96_enable_shared_from_this.cb (compile-time parity baseline)
#include <memory>
#include <cstdio>
#include <string>

struct SelfShared : std::enable_shared_from_this<SelfShared> { int value = 0; };

int main() {
    int failures = 0;
    auto owner = std::make_shared<SelfShared>(); owner->value = 17;
    std::weak_ptr<SelfShared> weak = owner->weak_from_this();
    if (weak.use_count() != 1 || weak.expired()) { std::printf("FAIL enable_shared_from_this weak\n"); failures |= 1; }
    std::shared_ptr<SelfShared> again = owner->shared_from_this();
    if (again->value != 17 || owner.use_count() != 2) { std::printf("FAIL enable_shared_from_this\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_11_96_enable_shared_from_this\n");
    return failures;
}
