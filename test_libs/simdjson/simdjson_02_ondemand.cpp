// C++20 equivalent of simdjson_02_ondemand.cb (compile-time parity baseline)
#include <cstdint>
#include <cstdio>
#include <simdjson.h>

// The cflat case names the concrete x64 implementation (fallback) types; mirror that here.
using SjImplParser = simdjson::fallback::ondemand::parser;
using SjImplDocument = simdjson::fallback::ondemand::document;

int main()
{
    int failures = 0;

    simdjson::ondemand::parser p1;
    simdjson::padded_string j1("[1,2,42]", 8);
    simdjson::simdjson_result<simdjson::ondemand::document> d1 = p1.iterate(j1);
    simdjson::simdjson_result<int64_t> r1 = d1.at(2).get_int64();
    if (r1.value() != 42) { std::printf("FAIL o1: got %lld want 42\n", (long long)r1.value()); failures |= 1; }

    SjImplParser p2;
    simdjson::padded_string j2("[1,2,42]", 8);
    simdjson::simdjson_result<SjImplDocument> d2 = p2.iterate(j2);
    simdjson::simdjson_result<int64_t> r2 = d2.at(2).get_int64();
    if (r2.value() != 42) { std::printf("FAIL o2: got %lld want 42\n", (long long)r2.value()); failures |= 2; }

    SjImplParser p3;
    simdjson::padded_string j3("[1,2,42]", 8);
    simdjson::simdjson_result<SjImplDocument> d3 = p3.iterate((simdjson::padded_string_view)j3);
    simdjson::simdjson_result<int64_t> r3 = d3.at(2).get_int64();
    if (r3.value() != 42) { std::printf("FAIL o3: got %lld want 42\n", (long long)r3.value()); failures |= 4; }

    SjImplParser p4;
    simdjson::padded_string j4("[1,2,42]", 8);
    simdjson::simdjson_result<SjImplDocument> d4 = p4.iterate(j4.data(), j4.length(), j4.length() + 64);
    simdjson::simdjson_result<int64_t> r4 = d4.at(2).get_int64();
    if (r4.value() != 42) { std::printf("FAIL o4: got %lld want 42\n", (long long)r4.value()); failures |= 8; }

    simdjson::ondemand::parser p5;
    simdjson::padded_string j5("[1,2,42]", 8);
    simdjson::simdjson_result<SjImplDocument> d5 = p5.iterate(j5.data(), j5.length(), j5.length() + 64);
    simdjson::simdjson_result<int64_t> r5 = d5.at(2).get_int64();
    if (r5.value() != 42) { std::printf("FAIL o5: got %lld want 42\n", (long long)r5.value()); failures |= 16; }

    simdjson::ondemand::parser p6;
    simdjson::padded_string j6("[1,2,42]", 8);
    simdjson::padded_string_view v6 = (simdjson::padded_string_view)j6;
    simdjson::simdjson_result<SjImplDocument> d6 = p6.iterate(v6);
    simdjson::simdjson_result<int64_t> r6 = d6.at(2).get_int64();
    if (r6.value() != 42) { std::printf("FAIL o6: got %lld want 42\n", (long long)r6.value()); failures |= 32; }

    simdjson::ondemand::parser p7;
    simdjson::padded_string j7("[1,2,42]", 8);
    simdjson::simdjson_result<SjImplDocument> d7 = p7.iterate((simdjson::padded_string_view)j7);
    simdjson::simdjson_result<int64_t> r7 = d7.at(2).get_int64();
    if (r7.value() != 42) { std::printf("FAIL o7: got %lld want 42\n", (long long)r7.value()); failures |= 64; }

    simdjson::ondemand::parser p8;
    simdjson::padded_string j8("{\"k\":[1,2,42]}", 14);
    simdjson::padded_string_view v8 = (simdjson::padded_string_view)j8;
    simdjson::simdjson_result<simdjson::ondemand::document> d8 = p8.iterate(v8);
    simdjson::simdjson_result<int64_t> r8 = d8.find_field("k").at(2).get_int64();
    if (r8.value() != 42) { std::printf("FAIL o8: got %lld want 42\n", (long long)r8.value()); failures |= 128; }
    if (failures == 0) std::printf("PASS simdjson_02_ondemand\n");
    return failures;
}
