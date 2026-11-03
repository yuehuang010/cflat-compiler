#pragma once

namespace t513s2
{
    inline char trace[256] = {};
    inline int trace_size = 0;
    inline int sequence = 0;
    inline int live = 0;

    inline void mark(char value)
    {
        if (trace_size < 255)
        {
            trace[trace_size++] = value;
            trace[trace_size] = 0;
        }
        const int digit = value == 'a' ? 1 : value == 'c' ? 2 : value == 'm' ? 3 : value == 'x' ? 4
            : value == 't' ? 5 : value == 'v' ? 6 : value == 'y' ? 7 : value == 'b' ? 8
            : value == 'p' ? 9 : 0;
        sequence = sequence * 10 + digit;
    }

    inline void reset()
    {
        trace_size = 0;
        trace[0] = 0;
        sequence = 0;
        live = 0;
    }

    inline int trace_code() { return sequence; }
    inline int live_count() { return live; }

    struct A
    {
        int value;
        A(int input) : value(input) { mark('a'); ++live; }
        A(const A& other) : value(other.value) { mark('c'); ++live; }
        A(A&& other) : value(other.value) { mark('m'); ++live; }
        ~A() { mark('x'); --live; }
    };

    struct BUserDtor
    {
        int value;
        BUserDtor(int input = 20) : value(input) { mark('b'); ++live; }
        BUserDtor(A input) : value(input.value + 100) { mark('v'); ++live; }
        BUserDtor(const BUserDtor& other) : value(other.value) { mark('p'); ++live; }
        BUserDtor(BUserDtor&& other) : value(other.value) { mark('q'); ++live; }
        ~BUserDtor() { mark('y'); --live; }
    };

    struct Token
    {
        Token() { mark('t'); ++live; }
        Token(const Token&) { mark('c'); ++live; }
        Token(Token&&) { mark('m'); ++live; }
        ~Token() { mark('y'); --live; }
    };

    // This record has an implicit destructor that destroys Token, with no user destructor.
    struct BNoUserDtor
    {
        int value;
        Token token;
        BNoUserDtor(int input = 20) : value(input), token() { mark('b'); }
        BNoUserDtor(A input) : value(input.value + 100), token() { mark('v'); }
    };

    inline A make_a(int value) { return A(value); }
    inline BUserDtor make_user() { return BUserDtor(20); }
    inline BNoUserDtor make_no_user() { return BNoUserDtor(20); }
    inline int const_ref_take(const BUserDtor& value, int extra = 3)
    {
        return value.value + extra;
    }
}
