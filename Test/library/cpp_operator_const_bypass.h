#pragma once

struct OpbinConstK
{
    int value;
    OpbinConstK& operator+=(int amount) { value += amount; return *this; }
};

struct OpbinConstU
{
    int value;
    OpbinConstU& operator+=(int amount) { value += amount; return *this; }
};

struct OpbinConstZ {};

inline const OpbinConstK& opbin_const_ref(OpbinConstK& value) { return value; }
inline const OpbinConstU& opbin_const_u_ref(OpbinConstU& value) { return value; }
template<class T> OpbinConstK& operator+=(OpbinConstK& value, T) { return value; }
template<class T> OpbinConstZ& operator+=(OpbinConstZ& value, T) { return value; }

struct OpbinConstKB { int value; };
struct OpbinConstKD : OpbinConstKB {};
inline const OpbinConstKD& opbin_const_kd_ref(OpbinConstKD& value) { return value; }
template<class T> int operator+=(OpbinConstKB& value, T) { value.value = 5; return 3; }
