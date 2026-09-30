// C++20 equivalent of simdjson_01_dom.cb (compile-time parity baseline)
#include <cstdint>
#include <cstdio>
#include <string>
#include <simdjson.h>

int main()
{
    int failures = 0;
    simdjson::dom::parser parser;
    simdjson::simdjson_result<simdjson::dom::element> result = parser.parse("[1,2,42]", 8, true);
    int parseError = (int)result.error();
    if (parseError != 0) { std::printf("FAIL parse: got %d want 0\n", parseError); failures |= 1; }
    simdjson::simdjson_result<int64_t> third = result.at(2).get_int64();
    int64_t value = third.value();
    if (value != 42) { std::printf("FAIL value: got %lld want 42\n", (long long)value); failures |= 2; }
    if (failures == 0) std::printf("PASS simdjson_01_dom\n");
    return failures;
}
