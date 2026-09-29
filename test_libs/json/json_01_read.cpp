// C++20 equivalent of json_01_read.cb (compile-time parity baseline)
#include <cstdio>
#include <string>
#include <nlohmann/json.hpp>

int main()
{
    int failures = 0;
    nlohmann::json object = nlohmann::json::parse("{\"a\":5,\"s\":\"hi\"}");
    int a = object["a"].get<int>();
    bool has = object.contains("a");
    int at = object.at("a").get<int>();
    std::string s = object["s"].get<std::string>();
    if (a != 5) { std::printf("FAIL j01: got %d want 5\n", a); failures |= 1; }
    if (!has || at != 5) { std::printf("FAIL j02: got %d want 5\n", at); failures |= 2; }
    if (s.size() != 2) { std::printf("FAIL j06: got %d want 2\n", (int)s.size()); failures |= 4; }

    nlohmann::json array = nlohmann::json::array();
    array.push_back(1);
    array.push_back(2);
    if (array.size() != 2 || !array.is_array()) { std::printf("FAIL j04: got %d want 2\n", (int)array.size()); failures |= 8; }
    nlohmann::json parsed = nlohmann::json::parse("[10,20,30]");
    int indexed = parsed[1].get<int>();
    if (indexed != 20) { std::printf("FAIL j05: got %d want 20\n", indexed); failures |= 16; }
    nlohmann::json dumpValue = nlohmann::json::parse("[1]");
    std::string dump = dumpValue.dump(2);
    if (dump.size() <= 3) { std::printf("FAIL j07: got %d want >3\n", (int)dump.size()); failures |= 32; }
    nlohmann::json iterable = nlohmann::json::parse("[1,2,3]");
    int sum = 0;
    for (auto it = iterable.begin(); it != iterable.end(); ++it) sum += (*it).get<int>();
    if (sum != 6) { std::printf("FAIL j08: got %d want 6\n", sum); failures |= 64; }
    bool accepted = nlohmann::json::accept("[1,");
    if (accepted) { std::printf("FAIL j10: got 1 want 0\n"); failures |= 128; }
    if (failures == 0) std::printf("PASS json_01_read\n");
    return failures;
}
