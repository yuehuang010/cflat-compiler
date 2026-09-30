// C++20 equivalent of json_03_build.cb (compile-time parity baseline)
#include <cstdio>
#include <string>
#include <nlohmann/json.hpp>

int main()
{
    int failures = 0;
    nlohmann::json j;
    j["x"] = 3;
    j["name"] = "bob";
    int size = (int)j.size();
    if (size != 2) { std::printf("FAIL size: got %d want 2\n", size); failures |= 1; }
    if (failures == 0) std::printf("PASS json_03_build\n");
    return failures;
}
