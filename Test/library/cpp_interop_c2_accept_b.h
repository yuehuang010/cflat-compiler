#pragma once
#include "cpp_interop_c2_accept_a.h"
namespace c2accept {
struct ForwardClass { int value; };
template<class T> struct ForwardTemplate { T value; };
template<class T> struct RedeclaredTemplate { T value; };
template<class T> struct IdenticalTemplate;
template<class T> struct IdenticalTemplate { T value; };
    template<class T> struct PartialSpecialized<T*> { long pad; T* value; };
    template<> struct Specialized<int> { int value; };
    template<> struct Specialized<long> { long pad; long value; };
inline int forward_class_value() { return 41; }
inline int forward_template_value() { return 42; }
inline int redeclared_template_value() { return 43; }
    inline int specialization_value() { return Specialized<int>{44}.value; }
    inline long specialization_long_value(Specialized<long>* value) { return value->value; }
    inline long specialization_long_size() { return sizeof(Specialized<long>); }
    inline int partial_specialization_value() { int value = 47; return *PartialSpecialized<int*>{0, &value}.value; }
    inline int partial_specialization_read(PartialSpecialized<int*>* value) { return *value->value; }
    inline long partial_specialization_size() { return sizeof(PartialSpecialized<int*>); }
inline int identical_template_value() { return 46; }
}
