// C++ interop fixture, bound with `import cpp` - the .h extension is deliberate: the keyword
// selects C++ mode, the extension never does. Declarations only; definitions live in the
// sibling cpp_interop_basic.cpp. Every entry point is noexcept except may_throw, which exists
// solely to arm Test/errors/err_cpp_may_throw.cb.
#pragma once

namespace cppi
{
    // Overload pair. The double leg adds 1000 so the SELECTED overload is observable
    // from the return value alone.
    int add(int a, int b) noexcept;
    int add(double a, double b) noexcept;

    // Reference parameters: same machine representation as a pointer at the boundary.
    int pick_ref(int& x) noexcept;
    int read_cref(const int& x) noexcept;

    int* ptr_ident(int* p) noexcept;

    // Reference RETURN: must arrive on the CFlat side as a plain pointer.
    int& ref_slot() noexcept;

    long long widen(int v) noexcept;
    unsigned char uc(unsigned char v) noexcept;

    enum class Mode : int { Off = 0, On = 1 };
    int mode_value(Mode m) noexcept;

    namespace inner
    {
        int twice(int v) noexcept;
    }

    // Deliberately NOT noexcept - calling it must be refused.
    int may_throw(int v);

    // A record reached by POINTER is always legal.
    struct Pair { int a; int b; };
    int read_pair(const Pair* p) noexcept;

    // ---- M3: trivially copyable records by value -------------------------------
    // Each shape lands on a DIFFERENT AArch64 arrangement, and every round trip adds 1 to
    // every field, so a wrong register assignment shows up as a wrong value, not just a
    // wrong type. sizeof/alignof are exported so the CFlat layout can be checked against
    // the C++ one rather than assumed.

    // 2 bytes: coerced to a single small integer.
    struct Small { signed char a; unsigned char b; };
    // 16 bytes, mixed int/FP: not an HFA, so two 8-byte chunks.
    struct Mixed { double d; int i; };
    // 24 bytes: too large for registers - indirect in, sret out.
    struct Large { long long x; long long y; long long z; };
    // Homogeneous float aggregate: 4 separate float registers under AAPCS64.
    struct Hfa { float a; float b; float c; float d; };
    // Packed: 5 bytes, alignment 1, `n` at byte offset 1.
    struct __attribute__((packed)) Packed { unsigned char c; int n; };
    // Over-aligned: sizeof is padded up to 32.
    struct alignas(32) Aligned { long long x; long long y; };

    Small  small_bump(Small v) noexcept;
    Mixed  mixed_bump(Mixed v) noexcept;
    Large  large_bump(Large v) noexcept;
    Hfa    hfa_bump(Hfa v) noexcept;
    Packed packed_bump(Packed v) noexcept;
    Aligned aligned_bump(Aligned v) noexcept;

    // Two aggregates with a scalar WEDGED BETWEEN them: proves the LLVM argument index
    // keeps up with slots that consume a different number of registers than they declare.
    long long mixed_args(Small s, int k, Large l) noexcept;

    // A by-value record alongside a by-pointer one in the same signature.
    int pair_and_small(const Pair* p, Small s) noexcept;

    unsigned long long size_of(int which) noexcept;
    unsigned long long align_of(int which) noexcept;

    // ---- M4: classes, members and access control -------------------------------
    // Trivially copyable class WITH members. Because copying it is raw bytes, it isolates the
    // member-call / static / access-control legs from construction and destruction.
    class Point
    {
    public:
        int x;
        int y;

        int sum() const noexcept;            // const instance method
        void bump(int d) noexcept;           // mutating instance method
        int scaled(int f, int off) noexcept;  // two arguments
        // const/non-const overload pair. CFlat drops const, so the RULING applies: an lvalue
        // picks the NON-const leg. The two legs return different values, so the selection is
        // observable from the result alone.
        int probe() noexcept;                // returns 1
        int probe() const noexcept;          // returns 2 - never selected for an lvalue

        static int origin_sum() noexcept;    // static method
        static int s_calls;                  // static data member, defined out of line

        int hidden() const noexcept;         // reads the private field below

    private:
        int hidden_;
    };

    // ---- M4b: nontrivial class lifetime instrumentation -------------------------
    // Every special member bumps an exported counter, so a CFlat test can assert the EXACT
    // number of constructions, copies, moves and destructions a construct performs. Bodies are
    // out of line so nothing needs C++ inline emission on the CFlat side.
    class Tracked
    {
    public:
        explicit Tracked(int payload) noexcept;
        Tracked(const Tracked& other) noexcept;
        Tracked(Tracked&& other) noexcept;
        ~Tracked() noexcept;
        Tracked& operator=(const Tracked& other) noexcept;
        Tracked& operator=(Tracked&& other) noexcept;

        int value() const noexcept;

    private:
        int payload_;
    };

    void reset_counts() noexcept;
    int ctor_count() noexcept;
    int copy_count() noexcept;
    int move_count() noexcept;
    int dtor_count() noexcept;
    int copy_assign_count() noexcept;
    int move_assign_count() noexcept;

    // A move-only class: the copy constructor is deleted, so a copy-init must be diagnosed.
    class NoCopy
    {
    public:
        explicit NoCopy(int v) noexcept;
        NoCopy(const NoCopy&) = delete;
        NoCopy(NoCopy&& other) noexcept;
        ~NoCopy() noexcept;
        int value() const noexcept;

    private:
        int v_;
    };

    // Nontrivial by value: return by sret, argument by caller-owned pointer.
    Tracked make_tracked(int payload) noexcept;
    int take_tracked(Tracked t) noexcept;
    int take_moved(Tracked t) noexcept;
}

extern "C" int cppi_c_linkage(int v) noexcept;
