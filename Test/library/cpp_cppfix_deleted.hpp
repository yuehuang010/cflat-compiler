#pragma once
// A converting constructor deleted for rvalues of its source, with a viable const& one beside it.
// clang selects the deleted constructor for an rvalue (move, returned local), so those are errors.
struct CfxSrc { int v = 1; };
struct CfxDst
{
    int w = 0;
    CfxDst(const CfxSrc& s) : w(s.v) {}
    CfxDst(CfxSrc&& s) = delete;
};
// By-value parameter and prvalue source: the deleted rvalue constructor is clang's pick for them.
inline int cfxTake(CfxDst d) { return d.w; }
inline CfxSrc cfxMake() { return CfxSrc{}; }
// A deleted constructor TEMPLATE takes any argument category: clang picks it for rvalue and lvalue.
struct CfxTmpl
{
    int w = 0;
    CfxTmpl(const CfxSrc& s) : w(s.v) {}
    template<class U> CfxTmpl(U&& u) = delete;
};
// A deleted const&& constructor: an xvalue of CfxSrc binds it over the const& one.
struct CfxConst
{
    int w = 0;
    CfxConst(const CfxSrc& s) : w(s.v) {}
    CfxConst(const CfxSrc&& s) = delete;
};
