// A derived class that declares a name itself hides every base declaration of that name: the
// refusal names the derived member (private field, unmatched overload), not the private base.
#pragma once
namespace cppi_pbs
{
    struct PbsBase { int v = 7; int get() const { return v; } };
    struct PbsHide : private PbsBase
    {
    private:
        int v = 9;
    public:
        int get(int, int) const { return 99; }
    };
    // A private using-declaration re-declares the base field in the derived class.
    struct PbsUsing : private PbsBase
    {
    private:
        using PbsBase::v;
    };
    // A public using-declaration re-exposes a field of a private base (clang: d.v is 7).
    struct PbsPubUsing : private PbsBase
    {
    public:
        using PbsBase::v;
    };
}
