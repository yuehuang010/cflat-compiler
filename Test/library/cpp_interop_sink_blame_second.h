namespace sbl2 {
template<class T> struct Shared
{
    int h() { return helper() + 1; }
    int h2() { return helper() + 2; }
    int h3() { T x = "abc"; return helper() + x; }
    int helper() { return T::nope; }
};
}
