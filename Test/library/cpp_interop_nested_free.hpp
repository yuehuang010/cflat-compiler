#pragma once
// Free functions over a nested member class of a class template specialization (NESTPROXY): the
// by-value nested return and nested parameter must bind without a prior use of the enclosing type.
namespace nfree {
inline int lastWhich = 0;
template<int N> struct Box {
    struct Item { int v; int twice() const { lastWhich = N; return v * 2; } };
    Item memberMake(int x) { return Item{x}; }
};
inline Box<3>::Item freeMake(int x) { return Box<3>::Item{x}; }
inline int consumeItem(Box<3>::Item x) { int v = x.twice(); lastWhich = 30; return v; }
}
inline nfree::Box<4>::Item nfrGlobalMake(int x) { return nfree::Box<4>::Item{x}; }
inline int nfrGlobalConsume(nfree::Box<4>::Item x) { int v = x.twice(); nfree::lastWhich = 40; return v; }
struct nfrPlainNest {
    struct Item { int v; int twice() const { nfree::lastWhich = 5; return v * 2; } };
};
inline nfrPlainNest::Item nfrPlainMake(int x) { return nfrPlainNest::Item{x}; }
