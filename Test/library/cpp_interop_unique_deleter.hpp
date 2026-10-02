#pragma once

namespace cppt {
template<class T, class D>
class FnUniquePtr {
    T* ptr_;
    D deleter_;
    int ctor_kind_;
public:
    FnUniquePtr(T* ptr, const D& deleter) : ptr_(ptr), deleter_(deleter), ctor_kind_(1) {}
    FnUniquePtr(T* ptr, D&& deleter)
        : ptr_(ptr), deleter_(static_cast<D&&>(deleter)), ctor_kind_(2) {}
    FnUniquePtr(FnUniquePtr&& other)
        : ptr_(other.ptr_), deleter_(static_cast<D&&>(other.deleter_)),
          ctor_kind_(other.ctor_kind_) { other.ptr_ = nullptr; }
    ~FnUniquePtr() { if (ptr_) deleter_(ptr_); }
    T& operator*() const { return *ptr_; }
    int ctor_kind() const { return ctor_kind_; }
};

// D defaults so `NoMoveFnUniquePtr<int>` and `<int, function<void(int*)>>` spell one C++ type.
template<class T, class D = void (*)(T*)>
class NoMoveFnUniquePtr {
    T* ptr_;
    D deleter_;
public:
    NoMoveFnUniquePtr(T* ptr, const D& deleter) : ptr_(ptr), deleter_(deleter) {}
    NoMoveFnUniquePtr(T* ptr, D&& deleter)
        : ptr_(ptr), deleter_(static_cast<D&&>(deleter)) {}
    ~NoMoveFnUniquePtr() { if (ptr_) deleter_(ptr_); }
    T& operator*() const { return *ptr_; }
};

template<class Sig>
struct FunctionTypeTag;
template<class R, class A>
struct FunctionTypeTag<R(A)> { static int arity() { return 1; } };
template<class R, class A>
struct FunctionTypeTag<R(*)(A)> { static int arity() { return 101; } };
}
