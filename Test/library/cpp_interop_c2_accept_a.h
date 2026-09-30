#pragma once
namespace c2accept {
struct ForwardClass;
template<class T> struct ForwardTemplate;
template<class T> struct RedeclaredTemplate;
template<class T> struct IdenticalTemplate;
template<class T> struct PartialSpecialized { T value; };
template<class T> struct Specialized { T value; };
}
