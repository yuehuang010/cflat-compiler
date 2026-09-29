#pragma once
// Fixture for assignment INTO a live nontrivial C++ object from a temporary (codes 3320-3339).
// Header-only; every special member bumps an inline static counter, so a leg asserts the EXACT
// number of constructions, copies, moves, assignments and destructions an assignment performs.

namespace cppas {

class Life
{
public:
    static inline int ctors = 0;
    static inline int copies = 0;
    static inline int moves = 0;
    static inline int dtors = 0;
    static inline int copy_assigns = 0;
    static inline int move_assigns = 0;
    static inline int last_dtor = 0;
    static inline void reset() noexcept
    {
        ctors = 0; copies = 0; moves = 0; dtors = 0; copy_assigns = 0; move_assigns = 0;
        last_dtor = 0;
    }
    inline Life() noexcept : v_(0) { ++ctors; }
    inline explicit Life(int v) noexcept : v_(v) { ++ctors; }
    inline Life(const Life& o) noexcept : v_(o.v_) { ++copies; }
    inline Life(Life&& o) noexcept : v_(o.v_) { o.v_ = -1; ++moves; }
    inline ~Life() noexcept { last_dtor = v_; v_ = -99; ++dtors; }
    inline Life& operator=(const Life& o) noexcept { v_ = o.v_; ++copy_assigns; return *this; }
    inline Life& operator=(Life&& o) noexcept { v_ = o.v_; o.v_ = -1; ++move_assigns; return *this; }
    inline int value() const noexcept { return v_; }
    // By-value MEMBER call result.
    inline Life twice() const noexcept { return Life(v_ * 2); }
    inline Life& ref() noexcept { return *this; }
    inline const Life& const_ref() const noexcept { return *this; }
    inline Life* ptr() noexcept { return this; }
    // By-value MEMBER operator result.
    inline Life operator-(const Life& o) const noexcept { return Life(v_ - o.v_); }
    int v_;
};

// Copy assignment DELETED: only a temporary or an explicit move may be assigned in.
class MoveOnly
{
public:
    static inline int move_assigns = 0;
    static inline int dtors = 0;
    inline explicit MoveOnly(int v) noexcept : v_(v) {}
    MoveOnly(const MoveOnly&) = delete;
    inline MoveOnly(MoveOnly&& o) noexcept : v_(o.v_) { o.v_ = -1; }
    inline ~MoveOnly() noexcept { v_ = -99; ++dtors; }
    MoveOnly& operator=(const MoveOnly&) = delete;
    inline MoveOnly& operator=(MoveOnly&& o) noexcept { v_ = o.v_; o.v_ = -1; ++move_assigns; return *this; }
    inline int value() const noexcept { return v_; }
    int v_;
};
inline MoveOnly operator+(const MoveOnly& a, const MoveOnly& b) noexcept { return MoveOnly(a.v_ + b.v_); }

// By-value FREE operator result.
inline Life operator+(const Life& a, const Life& b) noexcept { return Life(a.v_ + b.v_); }
// By-value free-function result.
inline Life make_life(int v) noexcept { return Life(v); }
inline int read_life(Life value) noexcept { return value.value(); }

// Assignment EXPRESSION used as a value (`||`, `&&`, `!`, `?:`, `if`, `while` operand). The result
// is the destination itself; only a class with a bool conversion (implicit, or explicit in a
// contextual position) is a legal condition. TruthNone declares none, so clang refuses it.
class TruthImplicit
{
public:
    static inline int assigns = 0;
    inline TruthImplicit() noexcept : v_(0) {}
    inline TruthImplicit(const TruthImplicit& o) : v_(o.v_) {}
    inline ~TruthImplicit() { v_ = -99; }
    inline TruthImplicit& operator=(int x) { v_ = x; ++assigns; return *this; }
    inline TruthImplicit& operator=(const TruthImplicit& o) { v_ = o.v_; assigns += 100; return *this; }
    inline operator bool() const { return v_ != 0; }
    int v_;
};
class TruthExplicit
{
public:
    static inline int assigns = 0;
    inline TruthExplicit() noexcept : v_(0) {}
    inline TruthExplicit(const TruthExplicit& o) noexcept : v_(o.v_) {}
    inline ~TruthExplicit() noexcept { v_ = -99; }
    inline TruthExplicit& operator=(int x) noexcept { v_ = x; ++assigns; return *this; }
    inline explicit operator bool() const noexcept { return v_ != 0; }
    int v_;
};
class TruthNone
{
public:
    static inline int assigns = 0;
    inline TruthNone() noexcept : v_(0) {}
    inline TruthNone(const TruthNone& o) : v_(o.v_) {}
    inline ~TruthNone() { v_ = -99; }
    inline TruthNone& operator=(int x) { v_ = x; ++assigns; return *this; }
    inline TruthNone& operator=(const TruthNone& o) { v_ = o.v_; return *this; }
    int v_;
};

// Assignment EXPRESSION used as an initializer / return value / argument: the result is the
// destination lvalue (`T&`), so a by-value consumer copy-constructs from it (clang: copies == 1).
class Tally
{
public:
    static inline int ctors = 0;
    static inline int copies = 0;
    static inline int moves = 0;
    static inline int dtors = 0;
    static inline int assigns = 0;
    static inline int copy_assigns = 0;
    static inline int move_assigns = 0;
    static inline void reset() noexcept
    {
        ctors = 0; copies = 0; moves = 0; dtors = 0; assigns = 0; copy_assigns = 0; move_assigns = 0;
    }
    inline Tally() noexcept : v_(0) { ++ctors; }
    inline explicit Tally(int v) noexcept : v_(v) { ++ctors; }
    inline Tally(const Tally& o) noexcept : v_(o.v_) { ++copies; }
    inline Tally(Tally&& o) noexcept : v_(o.v_) { o.v_ = -1; ++moves; }
    inline ~Tally() noexcept { v_ = -99; ++dtors; }
    inline Tally& operator=(int x) noexcept { v_ = x; ++assigns; return *this; }
    inline Tally& operator=(const Tally& o) noexcept { v_ = o.v_; ++copy_assigns; return *this; }
    inline Tally& operator=(Tally&& o) noexcept { v_ = o.v_; o.v_ = -1; ++move_assigns; return *this; }
    inline Tally& operator+=(int x) noexcept { v_ += x; ++assigns; return *this; }
    inline int value() const noexcept { return v_; }
    int v_;
};
inline Tally make_tally(int v) noexcept { return Tally(v); }
inline int read_tally(Tally t) noexcept { return t.v_; }
inline int peek_tally(const Tally& t) noexcept { return t.v_; }

// The same with BY-VALUE assignment operators (`return *this;` copies): the result is a prvalue,
// so a declarator / return is built straight from it (clang: copies == 1, no extra destroy).
class TallyByValue
{
public:
    static inline int copies = 0;
    static inline int moves = 0;
    static inline int dtors = 0;
    static inline int assigns = 0;
    static inline void reset() noexcept { copies = 0; moves = 0; dtors = 0; assigns = 0; }
    inline TallyByValue() noexcept : v_(0) {}
    inline TallyByValue(const TallyByValue& o) noexcept : v_(o.v_) { ++copies; }
    inline TallyByValue(TallyByValue&& o) noexcept : v_(o.v_) { o.v_ = -1; ++moves; }
    inline ~TallyByValue() noexcept { v_ = -99; ++dtors; }
    inline TallyByValue operator=(int x) noexcept { v_ = x; ++assigns; return *this; }
    inline TallyByValue operator+=(int x) noexcept { v_ += x; ++assigns; return *this; }
    inline TallyByValue& operator=(const TallyByValue& o) noexcept { v_ = o.v_; return *this; }
    int v_;
};

} // namespace cppas
