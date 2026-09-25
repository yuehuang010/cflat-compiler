// C++ fixture for M6 - inheritance and virtual dispatch. Everything here is declared only;
// definitions live in the sibling cpp_interop_poly.cpp so every virtual function has a real
// out-of-line symbol and the vtable has a key function to anchor it.
//
// Shapes cover single inheritance: virtual dispatch through a base pointer, a virtual
// destructor reached through `delete`, and an inherited field.
// Left / Right / Both cover MULTIPLE inheritance: `Right` is a NON-PRIMARY base of `Both`, so
// its subobject sits at a non-zero offset and every `Both* -> Right*` crossing must adjust.
// Abstract exists only to arm Test/errors/err_cpp_abstract_ctor.cb.
// VirtBase exists only to arm Test/errors/err_cpp_virtual_base.cb.
#pragma once

namespace cpppoly
{
    class Shape
    {
    public:
        Shape() noexcept;
        virtual ~Shape() noexcept;
        virtual int area() const noexcept;
        int plain() const noexcept;
        int tag;
    };

    int call_area(const Shape* s) noexcept;
    int call_area_ref(const Shape& s) noexcept;
    int call_plain(const Shape* s) noexcept;
    unsigned long long sizeof_shape() noexcept;

    class Circle : public Shape
    {
    public:
        Circle() noexcept;
        ~Circle() noexcept override;
        int area() const noexcept override;
        int radius;
    };

    class Square : public Shape
    {
    public:
        Square() noexcept;
        ~Square() noexcept override;
        int area() const noexcept override;
        int side;
    };

    // kind 0 -> Circle, anything else -> Square. Built on the C++ side, so the object's vptr
    // was written by C++ and the CFlat side only reads it.
    Shape* make(int kind) noexcept;
    void destroy(Shape* s) noexcept;
    inline void destroy_through(Shape* s) noexcept { delete s; }

    void reset_poly_counts() noexcept;
    int shape_dtors() noexcept;
    int circle_dtors() noexcept;
    int square_dtors() noexcept;
    void reset_unique_adjust_counts() noexcept;
    int left_unique_adjust_dtors() noexcept;
    int right_unique_adjust_dtors() noexcept;
    int both_unique_adjust_dtors() noexcept;

    struct Left
    {
        Left() noexcept;
        virtual ~Left() noexcept;
        virtual int l() const noexcept;
        int lv;
    };

    struct Right
    {
        Right() noexcept;
        virtual ~Right() noexcept;
        virtual int r() const noexcept;
        int rv;
    };

    struct Both : Left, Right
    {
        Both() noexcept;
        ~Both() noexcept override;
        int l() const noexcept override;
        int r() const noexcept override;
        int both_only() const noexcept;
    };

    inline void destroy_right_through(Right* p) noexcept { delete p; }

    struct BothBox
    {
        Both value;
        Both& get() noexcept;
    };

    struct NoVirtualBase { int nv; };
    struct BothLate : Left, NoVirtualBase, Right
    {
        BothLate() noexcept;
        ~BothLate() noexcept override;
        int l() const noexcept override;
        int r() const noexcept override;
    };
    struct NoVirtualDerived : Left, NoVirtualBase
    {
        NoVirtualDerived() noexcept;
    };

    // Takes the NON-PRIMARY base. Passing a Both* here is only correct if the caller added
    // Right's base offset; reading rv proves it did.
    int read_right(const Right* p) noexcept;
    int call_right(const Right* p) noexcept;
    int take_right_ptr(Right* p) noexcept;
    int take_right_ref(Right& r) noexcept;
    int read_right_cref(const Right& r) noexcept;
    // Reference returns of a derived object for the RETURN-statement legs (3370-3389): a CFlat
    // `Right*` / `Shape*` function returning one must add the base offset (16 for Right).
    Both& both_ref() noexcept;
    Circle& circle_ref() noexcept;

    // M6b - class names that CONTAIN a declarator keyword as a substring ("const" inside
    // "constant", "class" inside "classBase"). Stripping the keyword as a substring loses the
    // class identity, the parameter decays to void*, and the base adjustment silently vanishes.
    // Both are NON-PRIMARY bases of QualNames, so reading their fields proves it happened.
    struct constant { int cv; };
    struct classBase { int bv; };

    struct QualNames : Left, constant, classBase
    {
        QualNames() noexcept;
        ~QualNames() noexcept override;
        int l() const noexcept override;
    };

    int read_constant(const constant* p) noexcept;
    int read_class_base(const classBase* p) noexcept;
    QualNames* make_qual() noexcept;
    void destroy_qual(QualNames* p) noexcept;

    class Abstract
    {
    public:
        virtual ~Abstract() noexcept;
        virtual int pure() const noexcept = 0;
    };

    class Unspellable
    {
    public:
        virtual ~Unspellable() noexcept;
        virtual int take(int (*fn)(int, int), int Unspellable::*pm) const noexcept = 0;
    };

    class Tagged
    {
    public:
        explicit Tagged(int t) noexcept;
        virtual ~Tagged() noexcept;
        int tag;
        virtual int get() const noexcept;
    protected:
        int prot;
        int prot_get() const noexcept;
    };

    int call_get(const Tagged* t) noexcept;
    int call_pure(const Abstract* a) noexcept;

    class Pinned
    {
    public:
        Pinned() noexcept;
        virtual ~Pinned() noexcept;
        virtual int v() const noexcept;
        Pinned(const Pinned&) = delete;
        Pinned& operator=(const Pinned&) = delete;
    };

    int call_v(const Pinned* p) noexcept;

    class Guarded
    {
    public:
        Guarded() noexcept;
        virtual ~Guarded() noexcept;
        int probe() const noexcept { return hidden(); }
    private:
        virtual int hidden() const noexcept = 0;
    };

    int call_hidden(const Guarded* g) noexcept;

    struct Sealed
    {
        virtual ~Sealed() noexcept;
        virtual int f() const noexcept final;
    };

    class Scaler
    {
    public:
        virtual ~Scaler() noexcept;
        virtual double scale(double x, int k) const noexcept;
        virtual const char* label() const noexcept;
        virtual int adjust(int x) noexcept;
    };

    double call_scale(const Scaler* s, double x, int k) noexcept;
    const char* call_label(const Scaler* s) noexcept;
    int call_adjust(Scaler* s, int x) noexcept;

    struct VBaseTop { virtual ~VBaseTop() noexcept; virtual int t() const noexcept; int tv; };
    struct VBaseMid : virtual VBaseTop { int mv; };

    // M41 - virtual members whose vtable SLOT cflat cannot name on its own. Under the MS ABI a
    // member introduced by a NON-PRIMARY base lives in a vfptr at a non-zero offset, and one
    // reached through a VIRTUAL base lives in a vfptr found through the vbtable; a covariant
    // return whose base conversion is not at offset zero needs an adjustment on the way out.
    // Clang emits all three inside a synthesized thunk, so the shapes below exercise it.

    // Instrumented value type, so the by-value-argument and by-value-return cells assert the
    // RESOURCE and not only the value. Kept local to this fixture - the M5 counters belong to
    // a different header and a different set of assertions.
    void reset_slot_counts() noexcept;
    int slot_ctors() noexcept;
    int slot_copies() noexcept;
    int slot_moves() noexcept;
    int slot_dtors() noexcept;

    struct SlotVal
    {
        explicit SlotVal(int v) noexcept;
        SlotVal(const SlotVal& o) noexcept;
        SlotVal(SlotVal&& o) noexcept;
        ~SlotVal() noexcept;
        int v;
    };

    struct Slot
    {
        Slot() noexcept;
        virtual ~Slot() noexcept;
        virtual int primary() const noexcept;
        // The BASELINE for the by-value cell: same call shape on the PRIMARY vfptr, whose slot
        // cflat names directly. The thunk's exact extra cost is the difference from this.
        virtual int primary_val(SlotVal t) const noexcept;
        int sv;
    };

    struct Second
    {
        Second() noexcept;
        virtual ~Second() noexcept;
        virtual int second() const noexcept;                    // no arguments
        virtual int second_add(int x, int y) const noexcept;    // scalar arguments
        virtual int second_val(SlotVal t) const noexcept;       // by-value class argument
        virtual SlotVal second_ret() const noexcept;            // class return by value
        virtual int second_pure() const noexcept = 0;           // pure virtual, overridden below
        int cv2;
    };

    /*
     * Second is the NON-PRIMARY base, so every member it introduces sits in the second vfptr.
     * A TEMPLATE spelling on purpose: a directly named derived class reaches these members
     * through the base's own registration with an adjusted `this`, which already works. A
     * template specialization is held as a sized blob whose members are bound from its own
     * record, and that is the binding the vtable slot gates.
     */
    template <class T>
    struct TwoSlots : Slot, Second
    {
        TwoSlots() noexcept { sv = 33; cv2 = 44; tsv = 0; }
        ~TwoSlots() noexcept override {}
        int primary() const noexcept override { return sv + 1000; }
        int primary_val(SlotVal t) const noexcept override { return sv + t.v * 3; }
        int second() const noexcept override { return cv2 + 2000; }
        int second_add(int x, int y) const noexcept override { return cv2 + x * 10 + y; }
        int second_val(SlotVal t) const noexcept override { return cv2 + t.v * 2; }
        SlotVal second_ret() const noexcept override { return SlotVal(cv2 + 5); }
        int second_pure() const noexcept override { return cv2 + 3000; }
        T tsv;
    };
    // A specialization is held as a sized BLOB, so its fields have no CFlat spelling. These read
    // each subobject from the C++ side instead, which also proves the pointer handed over was
    // adjusted to the base it is declared as.
    int read_second(const Second* p) noexcept;
    int read_slot(const Slot* p) noexcept;
    TwoSlots<int>* make_two_slots() noexcept;
    void destroy_two_slots(TwoSlots<int>* p) noexcept;

    // Covariant return: CloneDer<T>* -> CloneBase* crosses a non-primary base, so the result
    // needs adjusting relative to the overridden declaration.
    struct CloneBase
    {
        CloneBase() noexcept;
        virtual ~CloneBase() noexcept;
        virtual CloneBase* clone() const noexcept;
        int bvv;
    };
    template <class T>
    struct CloneDer : Slot, CloneBase
    {
        CloneDer() noexcept { sv = 66; bvv = 77; cdv = 0; }
        ~CloneDer() noexcept override {}
        CloneDer<T>* clone() const noexcept override
        { CloneDer<T>* p = new CloneDer<T>(); p->bvv = bvv + 1; return p; }
        T cdv;
    };
    int read_clone(const CloneBase* p) noexcept;
    CloneDer<int>* make_clone_der() noexcept;
    void destroy_clone(CloneBase* p) noexcept;

    /*
     * VIRTUAL base. A directly named class with virtual bases is refused at LAYOUT, before its
     * vtable slot is ever consulted, so the only spelling that reaches the slot is a template
     * specialization - which cflat holds as a sized blob. The object is built and released on
     * the C++ side because cflat cannot call a constructor that takes an implicit most-derived
     * argument; what it CAN do is dispatch into one and destroy it.
     */
    void note_vbase_dtor() noexcept;
    void reset_vbase_dtors() noexcept;
    int vbase_dtors() noexcept;

    template <class T>
    struct VBaseUse : virtual VBaseTop
    {
        VBaseUse() noexcept : uv(0) { tv = 7; uv = 9; }
        // Counted, so `delete` proves the destructor RAN and did not merely free the storage.
        ~VBaseUse() noexcept override { note_vbase_dtor(); }
        int t() const noexcept override { return tv + 100; }
        virtual int u() const noexcept { return uv; }
        T uv;
    };
    VBaseUse<int>* make_vbase_use() noexcept;
    void destroy_vbase_use(VBaseUse<int>* p) noexcept;

    /*
     * Constructor axes for a class with virtual bases. cflat reaches every one of these through
     * the placement-new thunk, which is what makes clang pass the implicit most-derived flag /
     * VTT argument. A SECOND virtual base and a class DERIVING from one are separate shapes:
     * the flag must be 1 only for the complete object, which the thunk's `new` expression owns.
     */
    struct VBaseSide { virtual ~VBaseSide() noexcept; virtual int s() const noexcept; int sv; };

    template <class T>
    struct VBaseArgs : virtual VBaseTop
    {
        VBaseArgs() noexcept : av(1) { tv = 7; }
        explicit VBaseArgs(int a) noexcept : av(a) { tv = 7; }
        VBaseArgs(int a, int b) noexcept : av(b) { tv = a; }
        ~VBaseArgs() noexcept override { note_vbase_dtor(); }
        int t() const noexcept override { return tv + 100; }
        virtual int a() const noexcept { return av; }
        T av;
    };

    template <class T>
    struct VBaseDiamond : virtual VBaseTop, virtual VBaseSide
    {
        VBaseDiamond() noexcept : dv(5) { tv = 3; sv = 4; }
        explicit VBaseDiamond(int d) noexcept : dv(d) { tv = 3; sv = 4; }
        ~VBaseDiamond() noexcept override { note_vbase_dtor(); }
        int t() const noexcept override { return tv + 10; }
        int s() const noexcept override { return sv + 20; }
        virtual int d() const noexcept { return dv; }
        T dv;
    };

    template <class T>
    struct VBaseDer : VBaseArgs<T>
    {
        VBaseDer() noexcept : VBaseArgs<T>(2, 3) { }
        explicit VBaseDer(int x) noexcept : VBaseArgs<T>(x, x + 1) { }
        ~VBaseDer() noexcept override { note_vbase_dtor(); }
        int a() const noexcept override { return this->av + 1000; }
    };
}
