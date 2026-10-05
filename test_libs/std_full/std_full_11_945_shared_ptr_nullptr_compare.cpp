#include <string>
#include <utility>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>
#include <functional>
#include <cstdio>
int main()
{
    int failures = 0;
    std::shared_ptr<std::runtime_error> re = std::make_shared<std::runtime_error>("rt");
    std::shared_ptr<std::exception> ex = re;
    std::shared_ptr<std::runtime_error> down = std::dynamic_pointer_cast<std::runtime_error>(ex);
    std::shared_ptr<std::logic_error> wrong = std::dynamic_pointer_cast<std::logic_error>(ex);
    if (down.get() != re.get() || wrong != nullptr || down == nullptr || std::string(down->what()) != "rt") { printf("FAIL dynamic_pointer_cast\n"); failures |= 128; }
    std::shared_ptr<int> si = std::make_shared<int>(42);
    std::shared_ptr<void> sv = std::static_pointer_cast<void>(si);
    std::shared_ptr<int> back = std::static_pointer_cast<int>(sv);
    std::shared_ptr<long long> rc = std::reinterpret_pointer_cast<long long>(si);
    std::weak_ptr<int> wk = si;
    bool live = !wk.expired() && wk.lock() != nullptr && *wk.lock() == 42;
    si.reset(); back.reset(); rc.reset(); sv.reset();
    if (!live || !wk.expired() || wk.lock() != nullptr || wk.use_count() != 0) { printf("FAIL weak expired\n"); failures |= 256; }
    if (failures == 0) printf("PASS std_full_11_945_shared_ptr_nullptr_compare\n");
    return failures;
}
