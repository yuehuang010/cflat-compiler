#pragma once

namespace rangeadl_counted {
inline int objectBorn = 0;
inline int objectDied = 0;
inline int intGetCalls = 0;
inline int unsignedGetCalls = 0;
inline void reset() {
    objectBorn = 0;
    objectDied = 0;
    intGetCalls = 0;
    unsignedGetCalls = 0;
}
inline int born() { return objectBorn; }
inline int died() { return objectDied; }
inline int int_get_calls() { return intGetCalls; }
inline int unsigned_get_calls() { return unsignedGetCalls; }
struct Counted {
    Counted() { ++objectBorn; }
    ~Counted() { ++objectDied; }
    int count() { return 3; }
    int get(int i) { ++intGetCalls; return i + 1; }
    int get(unsigned i) { ++unsignedGetCalls; return 100 + static_cast<int>(i); }
};
}

namespace rangeadl_adl {
inline int objectBorn = 0;
inline int objectDied = 0;
inline int beginCalls = 0;
inline int endCalls = 0;
inline void reset() {
    objectBorn = 0;
    objectDied = 0;
    beginCalls = 0;
    endCalls = 0;
}
inline int born() { return objectBorn; }
inline int died() { return objectDied; }
inline int begin_calls() { return beginCalls; }
inline int end_calls() { return endCalls; }
struct AdlRange {
    int values[2];
    AdlRange() : values{6, 7} { ++objectBorn; }
    ~AdlRange() { ++objectDied; }
};
inline int* begin(AdlRange& value) { ++beginCalls; return value.values; }
inline int* end(AdlRange& value) { ++endCalls; return value.values + 2; }
}

namespace rangeadl_nested {
inline int plainValues[2] = {1, 2};
inline int templateValues[2] = {4, 5};
struct PlainOuter {
    struct Inner {
        int count() { return 2; }
        int get(int i) { return 10 + i; }
    };
    friend int* begin(Inner&) { return plainValues; }
    friend int* end(Inner&) { return plainValues + 2; }
};
struct TemplateOuter {
    struct Inner {
        int count() { return 2; }
        int get(int i) { return 10 + i; }
    };
    template<class U = int> friend int* begin(Inner&) { return templateValues; }
    template<class U = int> friend int* end(Inner&) { return templateValues + 2; }
};
}

struct RangeAdlGlobalOuter {
    struct Inner {
        int count() { return 2; }
        int get(int i) { return 20 + i; }
    };
    friend int* begin(Inner& value) { return values; }
    friend int* end(Inner& value) { return values + 2; }
    inline static int values[2] = {1, 2};
};
