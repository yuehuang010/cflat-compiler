#pragma once
#include "cpp_interop_c2_accept_a.h"
namespace c2accept {
struct ForwardClass { int value; };
template<class T> struct ForwardTemplate { T value; };
template<class T> struct RedeclaredTemplate { T value; };
template<class T> struct IdenticalTemplate;
template<class T> struct IdenticalTemplate { T value; };
template<class T> struct PartialSpecialized<T*> { T* value; };
template<> struct Specialized<int> { int value; };
inline int forward_class_value() { return 41; }
inline int forward_template_value() { return 42; }
inline int redeclared_template_value() { return 43; }
inline int specialization_value() { return Specialized<int>{44}.value; }
inline int partial_specialization_value() { int value = 47; return *PartialSpecialized<int*>{&value}.value; }
inline int identical_template_value() { return 46; }
}
