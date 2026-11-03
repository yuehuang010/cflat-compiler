#ifndef TEST_C_FNABI2_LONG_H
#define TEST_C_FNABI2_LONG_H

#include <stddef.h>

typedef int (*Fnabi2LongCallback)(long*);
typedef int (*Fnabi2SizeCallback)(size_t*);
typedef long* (*Fnabi2LongReturnCallback)(void);
typedef struct Fnabi2LongFields {
    long signed_value;
    unsigned long unsigned_value;
} Fnabi2LongFields;
typedef struct {
    long signed_value;
    unsigned long unsigned_value;
} Fnabi2AnonLongFields;

static inline int fnabi2_c_take_long(long* value) { return (int)*value + 100; }
static inline int fnabi2_c_call_long(Fnabi2LongCallback callback)
{
    long value = 7;
    return callback(&value);
}
static inline int fnabi2_c_call_size(Fnabi2SizeCallback callback)
{
    size_t value = 9;
    return (int)callback(&value);
}
static inline long* fnabi2_c_return_long(void)
{
    static long value = 31;
    return &value;
}
static inline long fnabi2_c_call_return(Fnabi2LongReturnCallback callback)
{
    long* value = callback();
    return value == 0 ? -1 : *value;
}
static inline long fnabi2_c_return_scalar(void) { return 41; }
static inline long fnabi2_c_add_long(long value) { return value + 13; }
static inline int fnabi2_c_long_fields_size(void) { return (int)sizeof(Fnabi2LongFields); }
static inline int fnabi2_c_long_fields_sum(const Fnabi2LongFields* value)
{
    return (int)value->signed_value + (int)value->unsigned_value;
}
static inline void fnabi2_c_long_fields_fill(Fnabi2LongFields* value)
{
    value->signed_value = 31;
    value->unsigned_value = 37;
}
static inline int fnabi2_c_anon_fields_size(void) { return (int)sizeof(Fnabi2AnonLongFields); }
static inline int fnabi2_c_anon_fields_sum(const Fnabi2AnonLongFields* value)
{
    return (int)value->signed_value + (int)value->unsigned_value;
}
static inline void fnabi2_c_anon_fields_fill(Fnabi2AnonLongFields* value)
{
    value->signed_value = 41;
    value->unsigned_value = 43;
}

#endif
