#pragma once
namespace cpp_derefcopy {
struct Cursor8 {
    int* next;
    explicit Cursor8(int* p) : next(p) {}
    Cursor8& operator*() { return *this; }
    Cursor8& operator=(int v) { *next++ = v; return *this; }
};
struct Cursor16 {
    int* next;
    long count;
    Cursor16(int* p = nullptr, long n = 0) : next(p), count(n) {}
    Cursor16& operator*() { return *this; }
    Cursor16& operator=(int v) { *next++ = v; ++count; return *this; }
    long count_value() const { return count; }
};
struct Cursor24 {
    int* next;
    long count;
    long marker;
    Cursor24(int* p, long n = 0) : next(p), count(n), marker(24) {}
    Cursor24& operator*() { return *this; }
    Cursor24& operator=(int v) { *next++ = v; ++count; return *this; }
    long size_marker() const { return marker; }
};
struct CursorNontrivial {
    int* next;
    long count;
    CursorNontrivial(int* p, long n = 0) : next(p), count(n) {}
    CursorNontrivial(const CursorNontrivial& other) : next(other.next), count(other.count) {}
    ~CursorNontrivial() {}
    CursorNontrivial& operator*() { return *this; }
    CursorNontrivial& operator=(int v) { *next++ = v; ++count; return *this; }
};
inline Cursor8 make8(int* p) { return Cursor8(p); }
inline Cursor16 make16(int* p) { return Cursor16(p); }
inline Cursor24 make24(int* p) { return Cursor24(p); }
inline CursorNontrivial make_nontrivial(int* p) { return CursorNontrivial(p); }
struct Holder { Cursor16 cursor; };
struct Node { int value; };
struct ArrowCursor {
    Node* next;
    explicit ArrowCursor(Node* p) : next(p) {}
    Node* operator->() { return next++; }
};
inline ArrowCursor make_arrow(Node* p) { return ArrowCursor(p); }
inline Cursor16 global_cursor;
struct Tag {
    int value;
    Tag(int v = 0) : value(v) {}
    bool operator==(int v) const { return value == v; }
    bool operator==(const Tag& o) const { return value == o.value; }
};
inline int refbox_kind = 0;
inline int refbox_last() { return refbox_kind; }
struct RefBox {
    Tag tag;
    explicit RefBox(int v) : tag(v) {}
    Tag& operator*() & { refbox_kind = 1; return tag; }
    const Tag& operator*() const& { refbox_kind = 2; return tag; }
    Tag&& operator*() && { refbox_kind = 3; return static_cast<Tag&&>(tag); }
    int peek() const { return tag.value; }
};
}
