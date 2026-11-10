#pragma once

// Copy-assignment parity: a const lvalue source binds operator=(const CV&), a non-const lvalue binds
// operator=(CV&), as clang selects them.
namespace cppcp {
struct CV {
    inline static int mutableAssign = 0;
    inline static int constAssign = 0;
    int v = 0;
    CV(int x) : v(x) {}
    CV& operator=(CV& o) { ++mutableAssign; v = o.v + 10; return *this; }
    CV& operator=(const CV& o) { ++constAssign; v = o.v + 20; return *this; }
};
struct DF {
    inline static int born = 0;
    inline static int dead = 0;
    inline static int mutableAssign = 0;
    int v = 0;
    DF(int x) : v(x) { ++born; }
    ~DF() { ++dead; }
    DF& operator=(const DF&) = default;
    DF& operator=(DF& o) { ++mutableAssign; v = o.v + 40; return *this; }
    static void reset() { born = 0; dead = 0; mutableAssign = 0; }
};
struct ByValSteal {
    inline static int copies = 0;
    inline static int moves = 0;
    inline static int assigns = 0;
    int v = 0;
    ByValSteal(int x) : v(x) {}
    ByValSteal(const ByValSteal& o) : v(o.v) { ++copies; }
    ByValSteal(ByValSteal&& o) : v(o.v) { ++moves; o.v = -1; }
    ByValSteal& operator=(ByValSteal o) { ++assigns; v = o.v; return *this; }
    static const ByValSteal& cref() { static ByValSteal value(7); return value; }
    static void reset() { copies = 0; moves = 0; assigns = 0; }
};
struct DefaultCopyMoveAssign {
    inline static int moveAssigns = 0;
    int v = 0;
    DefaultCopyMoveAssign(int x) : v(x) {}
    DefaultCopyMoveAssign& operator=(const DefaultCopyMoveAssign&) = default;
    DefaultCopyMoveAssign& operator=(DefaultCopyMoveAssign& o) { v = o.v + 40; return *this; }
    DefaultCopyMoveAssign& operator=(DefaultCopyMoveAssign&& o) {
        ++moveAssigns; v = o.v + 6; return *this;
    }
    static const DefaultCopyMoveAssign& cref() {
        static DefaultCopyMoveAssign value(7); return value;
    }
};
struct DefaultCopyConstMoveAssign {
    inline static int moveAssigns = 0;
    int v = 0;
    DefaultCopyConstMoveAssign(int x) : v(x) {}
    DefaultCopyConstMoveAssign& operator=(const DefaultCopyConstMoveAssign&) = default;
    DefaultCopyConstMoveAssign& operator=(const DefaultCopyConstMoveAssign&& o) {
        ++moveAssigns; v = o.v + 6; return *this;
    }
};
struct DefaultCopyMoveOnlyAssign {
    inline static int moveAssigns = 0;
    int v = 0;
    DefaultCopyMoveOnlyAssign(int x) : v(x) {}
    DefaultCopyMoveOnlyAssign& operator=(const DefaultCopyMoveOnlyAssign&) = default;
    DefaultCopyMoveOnlyAssign& operator=(DefaultCopyMoveOnlyAssign&& o) {
        ++moveAssigns; v = o.v + 6; return *this;
    }
};
struct DefaultMutableCopyConstUser {
    inline static int constAssigns = 0;
    int v = 0;
    DefaultMutableCopyConstUser(int x) : v(x) {}
    DefaultMutableCopyConstUser& operator=(DefaultMutableCopyConstUser&) = default;
    DefaultMutableCopyConstUser& operator=(const DefaultMutableCopyConstUser& o) {
        ++constAssigns; v = o.v + 20; return *this;
    }
};
struct CopyFieldType {
    inline static int mutableAssign = 0;
    int v = 0;
    CopyFieldType(int x = 0) : v(x) {}
    CopyFieldType& operator=(const CopyFieldType&) = default;
    CopyFieldType& operator=(CopyFieldType& o) { ++mutableAssign; v = o.v + 40; return *this; }
};
struct CopyFieldHolder { CopyFieldType value; };
struct CvFieldHolder {
    CV value;
    CvFieldHolder() : value(0) {}
};
struct CopyFieldMutableDefault {
    inline static int born = 0;
    inline static int dead = 0;
    int v = 0;
    CopyFieldMutableDefault() { ++born; }
    CopyFieldMutableDefault(const CopyFieldMutableDefault& o) : v(o.v) { ++born; }
    ~CopyFieldMutableDefault() { ++dead; }
    CopyFieldMutableDefault& operator=(CopyFieldMutableDefault&) = default;
    static void reset() { born = 0; dead = 0; }
};
struct CopyFieldMutableDefaultHolder { CopyFieldMutableDefault value; };
struct PrivateCopyMoveAssign {
private:
    PrivateCopyMoveAssign& operator=(const PrivateCopyMoveAssign&) = default;
public:
    int v = 0;
    PrivateCopyMoveAssign() = default;
    PrivateCopyMoveAssign& operator=(PrivateCopyMoveAssign&& o) {
        v = o.v + 6; return *this;
    }
};
struct TemplateMutableCopyAssign {
    inline static int templateAssigns = 0;
    inline static int moveAssigns = 0;
    int v = 0;
    TemplateMutableCopyAssign& operator=(const TemplateMutableCopyAssign&) = default;
    template<class U> TemplateMutableCopyAssign& operator=(U& o) {
        ++templateAssigns; v = o.v + 40; return *this;
    }
    TemplateMutableCopyAssign& operator=(TemplateMutableCopyAssign&& o) {
        ++moveAssigns; v = o.v + 6; return *this;
    }
};
struct ForwardTemplateMutableCopyAssign {
    inline static int templateAssigns = 0;
    inline static int moveAssigns = 0;
    int v = 0;
    ForwardTemplateMutableCopyAssign& operator=(const ForwardTemplateMutableCopyAssign&) = default;
    template<class U> ForwardTemplateMutableCopyAssign& operator=(U&& o) {
        ++templateAssigns; v = o.v + 40; return *this;
    }
    ForwardTemplateMutableCopyAssign& operator=(ForwardTemplateMutableCopyAssign&& o) {
        ++moveAssigns; v = o.v + 6; return *this;
    }
};
struct UserConstThenDefaultMutable {
    inline static int constAssigns = 0;
    int v = 0;
    UserConstThenDefaultMutable(int x = 0) : v(x) {}
    UserConstThenDefaultMutable& operator=(const UserConstThenDefaultMutable& o) {
        ++constAssigns; v = o.v + 20; return *this;
    }
    UserConstThenDefaultMutable& operator=(UserConstThenDefaultMutable&) = default;
};
struct CopyMoveFieldHolder { DF value; CopyMoveFieldHolder(int x) : value(x) {} };
}
