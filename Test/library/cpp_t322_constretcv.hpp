#pragma once

namespace cppi322
{
    struct R
    {
        int f() { return 1; }
        int f() const { return 2; }
        int onlyConst() const { return 3; }
    };

    inline const R make() { return R{}; }
    inline R make2() { return R{}; }
    inline const R& ref() { static R value; return value; }
    inline const R* ptr() { static R value; return &value; }

    struct Holder
    {
        const R get() const { return R{}; }
    };
    inline Holder holder;

    struct Only
    {
        int f() { return 7; }
    };
    inline const Only makeOnly() { return Only{}; }
}

namespace cppi322
{
    inline int const_arg_pick(R&&) { return 11; }
    inline int const_arg_pick(const R&&) { return 12; }
    inline int const_arg_only_rv(R&&) { return 13; }
    inline int const_arg_constref(const R&) { return 14; }
    inline int const_arg_byvalue(R) { return 15; }
    inline int const_arg_lref_pick(R&) { return 16; }
    inline int const_arg_lref_pick(const R&) { return 17; }
    inline int mutable_arg_pick(R&&) { return 18; }
    inline int mutable_arg_pick(const R&&) { return 19; }
    template<class T> inline const T make_template() { return T{}; }
    struct Base {};
    struct Derived : Base {};
    inline const Derived make_derived() { return Derived{}; }
    inline int const_base_arg_pick(Base&&) { return 21; }
    inline int const_base_arg_pick(const Base&&) { return 22; }
    inline int const_base_arg_only_rv(Base&&) { return 23; }
}

namespace cppi322
{
    using CR = const R;
    inline int forwarding_const_value(const R&) { return 18; }
    inline int forwarding_const_value(R&) { return 19; }
    template<class T> inline int forwarding_const(T&& value)
    {
        return forwarding_const_value(value);
    }

    struct EmplaceR { int value; };
    inline const EmplaceR make_emplace() { return EmplaceR{43}; }
}

namespace cppi322
{
    // T337 r3: member, free-operator and member-operator sets over a const prvalue / xvalue.
    struct Call
    {
        int choose(R&&) { return 41; }
        int choose(const R&&) { return 42; }
        int bind(R&) { return 43; }
        int bind(const R&) { return 44; }
        int only(R&&) { return 45; }
    };
    inline Call caller;
    struct OpTag {};
    inline OpTag opTag;
    inline int operator+(const OpTag&, R&&) { return 51; }
    inline int operator+(const OpTag&, const R&&) { return 52; }
    inline int operator-(const OpTag&, R&&) { return 53; }
    struct OpLeft {};
    inline OpLeft opLeft;
    inline int operator+(R&&, const OpLeft&) { return 54; }
    inline int operator+(const R&&, const OpLeft&) { return 55; }
    struct OpMember
    {
        int operator+(R&&) const { return 56; }
        int operator+(const R&&) const { return 57; }
    };
    inline OpMember opMember;
    inline const R&& const_xvalue() { static R value; return static_cast<const R&&>(value); }
    // T337 r4: assignment and compound assignment keep the const prvalue operand's cv.
    struct AssignC
    {
        int n = 0;
        AssignC& operator+=(R&&) { n = 81; return *this; }
        AssignC& operator+=(const R&&) { n = 82; return *this; }
        AssignC& operator=(R&&) { n = 83; return *this; }
        AssignC& operator=(const R&&) { n = 84; return *this; }
    };
    struct AssignTag { int n = 0; };
    inline AssignTag& operator+=(AssignTag& t, R&&) { t.n = 85; return t; }
    inline AssignTag& operator+=(AssignTag& t, const R&&) { t.n = 86; return t; }
    struct AssignOnly { int n = 0; AssignOnly& operator+=(R&&) { n = 87; return *this; } };
    struct AssignTagOnly { int n = 0; };
    inline AssignTagOnly& operator+=(AssignTagOnly& t, R&&) { t.n = 88; return t; }
}

namespace cppi322
{
    struct LrefR { int value; };
    struct LrefHolder { LrefR value; };
    inline const LrefR lref_const_global{14};
    inline LrefR lref_mutable_global{14};
    inline const LrefHolder lref_const_holder{{14}};
    inline const LrefR& lref_const_ref() { return lref_const_global; }
    inline const LrefR* lref_const_ptr() { return &lref_const_global; }
    inline LrefR& lref_mutable_ref() { return lref_mutable_global; }
    inline LrefR lref_make() { return LrefR{14}; }
    inline int lref_mutable(LrefR&) { return 14; }
    inline int lref_const(const LrefR&) { return 24; }
    inline int lref_value(LrefR) { return 34; }
    inline int lref_rvalue(LrefR&&) { return 44; }
    inline int lref_write(LrefR& value) { value.value = 99; return value.value; }
    inline int lref_read_mutable() { return lref_mutable_global.value; }
    inline int lref_pick(LrefR&) { return 51; }
    inline int lref_pick(const LrefR&) { return 52; }
    struct LrefMember { int take(LrefR&) const { return 61; } };
    inline LrefMember lref_member;
    struct LrefCtor { int value; LrefCtor(LrefR&) : value(71) {} };
    struct LrefOperator {};
    inline LrefOperator lref_operator;
    inline int operator+(const LrefOperator&, LrefR&) { return 81; }
    inline int lref_fn_target(LrefR& value) { value.value = 97; return 91; }
    using LrefFn = int(*)(LrefR&);
    inline LrefFn lref_fn = &lref_fn_target;
    inline void lref_reset_mutable() { lref_mutable_global.value = 14; }
    inline int lref_const_fn_target(const LrefR&) { return 92; }
    using LrefConstFn = int(*)(const LrefR&);
    inline LrefConstFn lref_const_fn = &lref_const_fn_target;
    inline int lref_value_fn_target(LrefR) { return 93; }
    using LrefValueFn = int(*)(LrefR);
    inline LrefValueFn lref_value_fn = &lref_value_fn_target;
}

namespace cppi322
{
    using ScalarLrefCallback = int(*)(int&);
    using ScalarConstLrefCallback = int(*)(const int&);
    inline int call_scalar_lref(ScalarLrefCallback callback)
    {
        int value = 40;
        callback(value);
        return value;
    }
    inline int call_scalar_const_lref(ScalarConstLrefCallback callback)
    {
        int value = 33;
        return callback(value);
    }
}
