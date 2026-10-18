#pragma once

#include "cpp_interop_basic.h"

extern "C" cppi::Tracked bridge_exported_for_test(int payload) noexcept;

namespace cpptw
{
    inline int call_bridge_exported(int payload) noexcept
    {
        return bridge_exported_for_test(payload).value();
    }

    inline int& ctor_counter() { static int value = 0; return value; }
    inline int& copy_counter() { static int value = 0; return value; }
    inline int& move_counter() { static int value = 0; return value; }
    inline int& dtor_counter() { static int value = 0; return value; }
    inline int& copy_assign_counter() { static int value = 0; return value; }
    inline int& move_assign_counter() { static int value = 0; return value; }
    inline int& live_allocation_counter() { static int value = 0; return value; }

    inline void reset_counts()
    {
        ctor_counter() = 0;
        copy_counter() = 0;
        move_counter() = 0;
        dtor_counter() = 0;
        copy_assign_counter() = 0;
        move_assign_counter() = 0;
        live_allocation_counter() = 0;
    }
    inline int ctor_count() { return ctor_counter(); }
    inline int copy_count() { return copy_counter(); }
    inline int move_count() { return move_counter(); }
    inline int dtor_count() { return dtor_counter(); }
    inline int copy_assign_count() { return copy_assign_counter(); }
    inline int move_assign_count() { return move_assign_counter(); }
    inline int live_allocations() { return live_allocation_counter(); }

    class Twin
    {
    public:
        int field;

        Twin() : data_(new int[3]), field(0)
        {
            data_[0] = 0; data_[1] = 1; data_[2] = 2;
            ++ctor_counter(); ++live_allocation_counter();
        }
        explicit Twin(int value) : data_(new int[3]), field(value)
        {
            data_[0] = value; data_[1] = value + 1; data_[2] = value + 2;
            ++ctor_counter(); ++live_allocation_counter();
        }
        Twin(const Twin& other) : data_(new int[3]), field(other.field)
        {
            data_[0] = other.data_[0]; data_[1] = other.data_[1]; data_[2] = other.data_[2];
            ++copy_counter(); ++live_allocation_counter();
        }
        Twin(Twin&& other) noexcept : data_(other.data_), field(other.field)
        {
            other.data_ = new int[3];
            other.data_[0] = -1; other.data_[1] = -1; other.data_[2] = -1;
            other.field = -1;
            ++move_counter(); ++live_allocation_counter();
        }
        Twin& operator=(const Twin& other)
        {
            if (this != &other)
            {
                data_[0] = other.data_[0]; data_[1] = other.data_[1]; data_[2] = other.data_[2];
                field = other.field;
            }
            ++copy_assign_counter();
            return *this;
        }
        Twin& operator=(Twin&& other) noexcept
        {
            if (this != &other)
            {
                delete[] data_;
                --live_allocation_counter();
                data_ = other.data_;
                field = other.field;
                other.data_ = new int[3];
                other.data_[0] = -1; other.data_[1] = -1; other.data_[2] = -1;
                other.field = -1;
                ++live_allocation_counter();
            }
            ++move_assign_counter();
            return *this;
        }
        ~Twin()
        {
            delete[] data_;
            --live_allocation_counter();
            ++dtor_counter();
        }

        int value() const { return field; }
        void set(int value)
        {
            field = value;
            data_[0] = value; data_[1] = value + 1; data_[2] = value + 2;
        }
        bool operator==(const Twin& other) const { return field == other.field; }
        int& operator[](int index) { return data_[index]; }
        int operator[](int index) const { return data_[index]; }
        operator bool() const { return field != 0; }
        int* begin() { return data_; }
        int* end() { return data_ + 3; }
        const int* begin() const { return data_; }
        const int* end() const { return data_ + 3; }

        static int static_value() { return 73; }

    private:
        int* data_;
    };

    inline int& tracked_iter_ctor_counter() { static int value = 0; return value; }
    inline int& tracked_iter_copy_counter() { static int value = 0; return value; }
    inline int& tracked_iter_move_counter() { static int value = 0; return value; }
    inline int& tracked_iter_dtor_counter() { static int value = 0; return value; }
    inline int& tracked_iter_bad_dtor_counter() { static int value = 0; return value; }
    inline int& tracked_iter_live_counter() { static int value = 0; return value; }
    inline int& tracked_range_ctor_counter() { static int value = 0; return value; }
    inline int& tracked_range_copy_counter() { static int value = 0; return value; }
    inline int& tracked_range_move_counter() { static int value = 0; return value; }
    inline int& tracked_range_dtor_counter() { static int value = 0; return value; }
    inline int& tracked_range_bad_dtor_counter() { static int value = 0; return value; }
    inline int& tracked_range_live_counter() { static int value = 0; return value; }

    inline void reset_tracked_range_counts()
    {
        tracked_iter_ctor_counter() = 0;
        tracked_iter_copy_counter() = 0;
        tracked_iter_move_counter() = 0;
        tracked_iter_dtor_counter() = 0;
        tracked_iter_bad_dtor_counter() = 0;
        tracked_iter_live_counter() = 0;
        tracked_range_ctor_counter() = 0;
        tracked_range_copy_counter() = 0;
        tracked_range_move_counter() = 0;
        tracked_range_dtor_counter() = 0;
        tracked_range_bad_dtor_counter() = 0;
        tracked_range_live_counter() = 0;
    }
    inline int tracked_iter_ctor_count() { return tracked_iter_ctor_counter(); }
    inline int tracked_iter_copy_count() { return tracked_iter_copy_counter(); }
    inline int tracked_iter_move_count() { return tracked_iter_move_counter(); }
    inline int tracked_iter_dtor_count() { return tracked_iter_dtor_counter(); }
    inline int tracked_iter_bad_dtor_count() { return tracked_iter_bad_dtor_counter(); }
    inline int tracked_iter_live_count() { return tracked_iter_live_counter(); }
    inline int tracked_iter_made_count()
    {
        return tracked_iter_ctor_count() + tracked_iter_copy_count() + tracked_iter_move_count();
    }
    inline int tracked_range_ctor_count() { return tracked_range_ctor_counter(); }
    inline int tracked_range_copy_count() { return tracked_range_copy_counter(); }
    inline int tracked_range_move_count() { return tracked_range_move_counter(); }
    inline int tracked_range_dtor_count() { return tracked_range_dtor_counter(); }
    inline int tracked_range_bad_dtor_count() { return tracked_range_bad_dtor_counter(); }
    inline int tracked_range_live_count() { return tracked_range_live_counter(); }
    inline int tracked_range_made_count()
    {
        return tracked_range_ctor_count() + tracked_range_copy_count() + tracked_range_move_count();
    }

    class TrackedRangeIterator
    {
    public:
        TrackedRangeIterator(int* current, int* finish)
            : current_(current), finish_(finish), destroyed_(false)
        {
            ++tracked_iter_ctor_counter();
            ++tracked_iter_live_counter();
        }
        TrackedRangeIterator(const TrackedRangeIterator& other)
            : current_(other.current_), finish_(other.finish_), destroyed_(false)
        {
            ++tracked_iter_copy_counter();
            ++tracked_iter_live_counter();
        }
        TrackedRangeIterator(TrackedRangeIterator&& other) noexcept
            : current_(other.current_), finish_(other.finish_), destroyed_(false)
        {
            ++tracked_iter_move_counter();
            ++tracked_iter_live_counter();
        }
        ~TrackedRangeIterator()
        {
            ++tracked_iter_dtor_counter();
            if (destroyed_)
            {
                ++tracked_iter_bad_dtor_counter();
                return;
            }
            destroyed_ = true;
            --tracked_iter_live_counter();
        }

        bool operator!=(const TrackedRangeIterator& other) const
        {
            return current_ != other.current_;
        }
        int operator*() const { return *current_; }
        TrackedRangeIterator& operator++()
        {
            ++current_;
            return *this;
        }

    private:
        int* current_;
        int* finish_;
        bool destroyed_;
    };

    class TrackedRange
    {
    public:
        TrackedRange() : destroyed_(false)
        {
            data_[0] = 3; data_[1] = 5; data_[2] = 7;
            ++tracked_range_ctor_counter();
            ++tracked_range_live_counter();
        }
        TrackedRange(const TrackedRange& other) : destroyed_(false)
        {
            data_[0] = other.data_[0]; data_[1] = other.data_[1]; data_[2] = other.data_[2];
            ++tracked_range_copy_counter();
            ++tracked_range_live_counter();
        }
        TrackedRange(TrackedRange&& other) noexcept : destroyed_(false)
        {
            data_[0] = other.data_[0]; data_[1] = other.data_[1]; data_[2] = other.data_[2];
            ++tracked_range_move_counter();
            ++tracked_range_live_counter();
        }
        ~TrackedRange()
        {
            ++tracked_range_dtor_counter();
            if (destroyed_)
            {
                ++tracked_range_bad_dtor_counter();
                return;
            }
            destroyed_ = true;
            --tracked_range_live_counter();
        }

        TrackedRangeIterator begin()
        {
            return TrackedRangeIterator(data_, data_ + 3);
        }
        TrackedRangeIterator end()
        {
            return TrackedRangeIterator(data_ + 3, data_ + 3);
        }

    private:
        int data_[3];
        bool destroyed_;
    };

    inline TrackedRange make_tracked_range()
    {
        return TrackedRange();
    }

    class RawPointerRange
    {
    public:
        RawPointerRange() : first_(4), second_(7)
        {
            values_[0] = &first_;
            values_[1] = &second_;
        }
        int** begin() { return values_; }
        int** end() { return values_ + 2; }

    private:
        int first_;
        int second_;
        int* values_[2];
    };

    class PrvalueTwinIterator
    {
    public:
        PrvalueTwinIterator(int value, int finish) : value_(value), finish_(finish) {}
        bool operator!=(const PrvalueTwinIterator& other) const
        {
            return value_ != other.value_;
        }
        Twin operator*() const { return Twin(value_); }
        PrvalueTwinIterator& operator++()
        {
            ++value_;
            return *this;
        }

    private:
        int value_;
        int finish_;
    };

    class PrvalueTwinRange
    {
    public:
        PrvalueTwinIterator begin() { return PrvalueTwinIterator(1, 3); }
        PrvalueTwinIterator end() { return PrvalueTwinIterator(3, 3); }
    };

    // User copy constructor and destructor, clang's implicit copy assignment stays TRIVIAL (no
    // operator= symbol exists): a CFlat struct holding one assigns the field by byte copy.
    inline int& trivial_assign_copy_counter() { static int value = 0; return value; }
    inline int& trivial_assign_dtor_counter() { static int value = 0; return value; }
    inline int trivial_assign_copy_count() { return trivial_assign_copy_counter(); }
    inline int trivial_assign_dtor_count() { return trivial_assign_dtor_counter(); }
    class TrivialAssignTwin
    {
    public:
        int field;
        TrivialAssignTwin() : field(0) {}
        explicit TrivialAssignTwin(int value) : field(value) {}
        TrivialAssignTwin(const TrivialAssignTwin& other) : field(other.field)
        {
            ++trivial_assign_copy_counter();
        }
        ~TrivialAssignTwin() { ++trivial_assign_dtor_counter(); }
        int value() const { return field; }
        void set(int value) { field = value; }
    };
    /*
     * [stmt.ranged] lookup shapes. FreeTrackedRange has no begin/end members: its namespace-scope
     * begin/end are found by argument-dependent lookup only. SelfRangeIter is the
     * std::filesystem::directory_iterator shape: the iterator is its own range, free begin/end take
     * it BY VALUE, and end() is a default-constructed iterator. FriendRange's begin/end are hidden
     * friends. MemberWinsRange has members AND namespace-scope begin/end: the members win.
     */
    class FreeTrackedRange
    {
    public:
        FreeTrackedRange() { data_[0] = 2; data_[1] = 4; data_[2] = 6; }
        int* data() { return data_; }

    private:
        int data_[3];
    };
    inline TrackedRangeIterator begin(FreeTrackedRange& range)
    {
        return TrackedRangeIterator(range.data(), range.data() + 3);
    }
    inline TrackedRangeIterator end(FreeTrackedRange& range)
    {
        return TrackedRangeIterator(range.data() + 3, range.data() + 3);
    }

    class SelfRangeIter
    {
    public:
        SelfRangeIter() : value_(0), finish_(0) {}
        SelfRangeIter(int first, int finish) : value_(first), finish_(finish) {}
        const int& operator*() const { return value_; }
        SelfRangeIter& operator++()
        {
            ++value_;
            if (value_ == finish_) value_ = finish_ = 0;
            return *this;
        }

    private:
        int value_;
        int finish_;
        friend bool operator==(const SelfRangeIter& a, const SelfRangeIter& b)
        {
            return a.value_ == b.value_ && a.finish_ == b.finish_;
        }
    };
    inline SelfRangeIter begin(SelfRangeIter iter) { return iter; }
    inline SelfRangeIter end(SelfRangeIter) { return SelfRangeIter(); }

    class FriendRange
    {
    public:
        FriendRange() { data_[0] = 7; data_[1] = 8; }
        friend int* begin(FriendRange& range) { return range.data_; }
        friend int* end(FriendRange& range) { return range.data_ + 2; }

    private:
        int data_[2];
    };

    class MemberWinsRange
    {
    public:
        MemberWinsRange() { data_[0] = 10; data_[1] = 20; other_[0] = 1; other_[1] = 2; }
        int* begin() { return data_; }
        int* end() { return data_ + 2; }
        int* other() { return other_; }

    private:
        int data_[2];
        int other_[2];
    };
    inline int* begin(MemberWinsRange& range) { return range.other(); }
    inline int* end(MemberWinsRange& range) { return range.other() + 2; }

    /*
     * A range view's shape: the iterator classes are PRIVATE nested classes of a class template
     * (filter_view::__iterator, transform_view::__iterator<Const>), so no code outside the view
     * can name them. Equality is a hidden friend; != is the C++20 rewrite of ==.
     */
    template <class T>
    class PrivateIterSeq
    {
        template <bool Const>
        class Iter
        {
        public:
            explicit Iter(T value) : value_(value) {}
            T operator*() const { return Const ? value_ * 100 : value_; }
            Iter& operator++() { ++value_; return *this; }
            friend bool operator==(const Iter& a, const Iter& b) { return a.value_ == b.value_; }

        private:
            T value_;
        };

    public:
        PrivateIterSeq(T first, T last) : first_(first), last_(last) {}
        Iter<false> begin() { return Iter<false>(first_); }
        Iter<false> end() { return Iter<false>(last_); }
        Iter<true> begin() const { return Iter<true>(first_); }
        Iter<true> end() const { return Iter<true>(last_); }

    private:
        T first_;
        T last_;
    };
    // [stmt.ranged] uses members only when BOTH begin and end are members: one member name
    // alone still iterates through the namespace-scope pair (ADL).
    struct BeginOnlyRange
    {
        int values[2] = { 2, 4 };
        int* begin() { return values; }
    };
    inline int* begin(BeginOnlyRange& range) { return range.values; }
    inline int* end(BeginOnlyRange& range) { return range.values + 2; }
    struct EndOnlyRange
    {
        int values[2] = { 2, 4 };
        int* end() { return values + 2; }
    };
    inline int* begin(EndOnlyRange& range) { return range.values; }
    inline int* end(EndOnlyRange& range) { return range.values + 2; }

    // __range is a named reference: a temporary collection still selects begin(R&), not R&&.
    struct ValueCategoryRange
    {
        int values[3] = { 1, 2, 3 };
    };
    inline int* begin(ValueCategoryRange& range) { return range.values; }
    inline int* begin(ValueCategoryRange&& range) { return range.values + 1; }
    inline int* end(ValueCategoryRange& range) { return range.values + 3; }
    inline int* end(ValueCategoryRange&& range) { return range.values + 3; }

    // A private nested iterator whose members evaluate access-dependent template decisions on
    // a private member and a private constructor: binding the iterator must not make them pass.
    template <class T>
    class AccessProbeSecret
    {
        AccessProbeSecret() {}
        int hidden() { return 7; }
    };
    template <class T>
    concept AccessProbeReachable = requires(T& t) { t.hidden(); };
    template <class T>
    class AccessProbeSeq
    {
        template <bool B>
        class Iter
        {
            int n_;

        public:
            Iter() : n_(1) {}
            int requires_result() const noexcept(AccessProbeReachable<AccessProbeSecret<Iter>>)
            {
                return AccessProbeReachable<AccessProbeSecret<Iter>>;
            }
            int ctor_result() const noexcept(__is_constructible(AccessProbeSecret<Iter>))
            {
                return __is_constructible(AccessProbeSecret<Iter>);
            }
            int noexcept_result() const { return noexcept(requires_result()); }
            template <class U>
            static constexpr auto sfinae(U* u, int) -> decltype(u->hidden(), bool()) { return true; }
            template <class U>
            static constexpr bool sfinae(U*, ...) { return false; }
            int sfinae_result() const noexcept(sfinae((AccessProbeSecret<Iter>*)nullptr, 0))
            {
                return sfinae((AccessProbeSecret<Iter>*)nullptr, 0);
            }
        };

    public:
        AccessProbeSeq() {}
        Iter<false> begin() { return Iter<false>(); }
    };
    template <class T>
    int access_probe_later() { return AccessProbeReachable<AccessProbeSecret<T>>; }

    // begin/end at GLOBAL scope are not associated with cpptw: range-for must not find them.
    struct OrdinaryLookupRange
    {
        int values[2] = { 3, 7 };
    };

    // [stmt.ranged]: member lookup finds begin AND end (any kind) -> r.begin()/r.end(); not
    // callable here, so CFlat refuses (clang: called object type is not a function, etc.).
    struct EnumBothRange { enum { begin, end }; int values[3] = { 1, 2, 3 }; };
    struct TypeBothRange { struct begin {}; struct end {}; int values[3] = { 1, 2, 3 }; };
    struct PrivateTypesRange { private: struct begin {}; struct end {}; public: int values[3] = { 1, 2, 3 }; };
    struct InheritEnumRange : EnumBothRange {};
    inline int* begin(EnumBothRange& range) { return range.values; }
    inline int* end(EnumBothRange& range) { return range.values + 3; }
    inline int* begin(TypeBothRange& range) { return range.values; }
    inline int* end(TypeBothRange& range) { return range.values + 3; }
    inline int* begin(PrivateTypesRange& range) { return range.values; }
    inline int* end(PrivateTypesRange& range) { return range.values + 3; }
    inline int* begin(InheritEnumRange& range) { return range.values; }
    inline int* end(InheritEnumRange& range) { return range.values + 3; }

    // Only begin is a member (ignored: both come from ADL); member begin starts at values + 1.
    struct OneMemberRange
    {
        int values[3] = { 1, 2, 3 };
        int* begin() { return values + 1; }
    };
    inline int* begin(OneMemberRange& range) { return range.values; }
    inline int* end(OneMemberRange& range) { return range.values + 3; }

    // Member begin/end inherited from a base count as found.
    struct OrdinaryBaseRange
    {
        int values[3] = { 1, 2, 3 };
        int* begin() { return values; }
        int* end() { return values + 3; }
    };
    struct InheritMemberRange : OrdinaryBaseRange {};
}

// Global-scope begin/end: ordinary unqualified lookup would find these, [stmt.ranged] does not
// (cpptw::OrdinaryLookupRange). GlobalScopeRange lives in the global namespace, so ADL does.
inline int* begin(cpptw::OrdinaryLookupRange& range) { return range.values; }
inline int* end(cpptw::OrdinaryLookupRange& range) { return range.values + 2; }
struct GlobalScopeRange
{
    int values[2] = { 5, 6 };
};
inline int* begin(GlobalScopeRange& range) { return range.values; }
inline int* end(GlobalScopeRange& range) { return range.values + 2; }

// T306: a C++ ctor taking Twin by value, for `Holder((default))`.
namespace cppt306
{
    class Holder
    {
    public:
        explicit Holder(cpptw::Twin value) : value_(value) {}
        int value() const { return value_.value(); }
    private:
        cpptw::Twin value_;
    };
}

// T306 round 2: a by-value Twin ctor param followed by a defaulted param.
namespace cppt306
{
    class DefaultArgHolder
    {
    public:
        explicit DefaultArgHolder(cpptw::Twin value, int bias = 5) : value_(value.value() + bias) {}
        int value() const { return value_; }
    private:
        int value_;
    };
}

// T306 round 3: a second by-value constructor, so a `default` argument has no unique parameter type,
// and a derived Twin for a ternary arm that slices into the Twin parameter.
namespace cppt306
{
    struct PairTag
    {
        PairTag() {}
        PairTag(const PairTag&) {}
        ~PairTag() {}
    };
    class DefaultPairHolder
    {
    public:
        explicit DefaultPairHolder(cpptw::Twin value) : value_(value.value()) {}
        explicit DefaultPairHolder(PairTag) : value_(-1) {}
        int value() const { return value_; }
    private:
        int value_;
    };
    struct DerivedTwin : cpptw::Twin { DerivedTwin() {} };
    inline DerivedTwin derived_twin() { return DerivedTwin{}; }
}

namespace cppt306
{
    // A nontrivial class with a non-explicit converting constructor from Twin (a converting cast target).
    struct ConvTwin
    {
        cpptw::Twin t;
        ConvTwin() {}
        ConvTwin(cpptw::Twin v) : t(static_cast<cpptw::Twin&&>(v)) {}
        int value() const { return t.value(); }
    };
    class ConvTwinHolder
    {
    public:
        explicit ConvTwinHolder(ConvTwin value) : value_(value.value()) {}
        int value() const { return value_; }
    private:
        int value_;
    };
}

namespace cppt306
{
    // T306 round 4: an unrelated nontrivial class that converts to Twin (a borrowed lvalue arm of another type).
    struct LookTwin
    {
        int field;
        LookTwin() : field(4) {}
        LookTwin(const LookTwin& other) : field(other.field) {}
        ~LookTwin() {}
        operator cpptw::Twin() const { return cpptw::Twin(field); }
    };
}
