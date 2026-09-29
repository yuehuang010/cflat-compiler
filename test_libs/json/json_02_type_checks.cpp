// C++20 equivalent of json_02_type_checks.cb (compile-time parity baseline)
#include <cstdio>
#include <nlohmann/json.hpp>

int main()
{
    int failures = 0;
    nlohmann::json j = nlohmann::json::parse("{\"n\":1.5,\"s\":\"x\",\"z\":null}");
    int number = j["n"].is_number() ? 1 : 0;
    int stringValue = j["s"].is_string() ? 1 : 0;
    int nullValue = j["z"].is_null() ? 1 : 0;
    int object = j.is_object() ? 1 : 0;
    double numberValue = j["n"].get<double>();
    if (number != 1) { std::printf("FAIL is_number: got %d want 1\n", number); failures |= 1; }
    if (stringValue != 1) { std::printf("FAIL is_string: got %d want 1\n", stringValue); failures |= 2; }
    if (nullValue != 1) { std::printf("FAIL is_null: got %d want 1\n", nullValue); failures |= 4; }
    if (object != 1) { std::printf("FAIL is_object: got %d want 1\n", object); failures |= 8; }
    if (numberValue != 1.5) { std::printf("FAIL number value: got %f want 1.5\n", numberValue); failures |= 16; }
    if (failures == 0) std::printf("PASS json_02_type_checks\n");
    return failures;
}
