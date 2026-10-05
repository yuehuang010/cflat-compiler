#include <string>
#include <complex>
#include <valarray>
#include <cstdio>
double dbl(double x) { return x * 2.0; }
int main()
{
    int failures = 0;
    std::complex<double> a(1.0, 2.0), b(3.0, -1.0);
    std::complex<double> s = a + b, p = a * b, q = a - b, sc = a * 2.0;
    if (s != std::complex<double>(4.0, 1.0) || p != std::complex<double>(5.0, 5.0) || q != std::complex<double>(-2.0, 3.0) || sc != std::complex<double>(2.0, 4.0) || !(a == std::complex<double>(1.0, 2.0)))
    { printf("FAIL complex arithmetic\n"); failures |= 1; }
    std::complex<double> c34(3.0, 4.0);
    std::complex<double> cj = std::conj(c34);
    if (std::norm(c34) != 25.0 || std::abs(c34) != 5.0 || cj.real() != 3.0 || cj.imag() != -4.0 || std::arg(std::complex<double>(1.0, 0.0)) != 0.0)
    { printf("FAIL complex functions\n"); failures |= 2; }
    std::complex<double> pl = std::polar(2.0, 0.0);
    if (pl.real() != 2.0 || pl.imag() != 0.0) { printf("FAIL polar\n"); failures |= 4; }
    std::valarray<int> v(5);
    v = 3;
    int arr[4] = {4, 1, 3, 2};
    std::valarray<int> w(arr, 4);
    if (v.size() != 5 || v[0] != 3 || v[4] != 3 || w.size() != 4 || w[0] != 4 || w.sum() != 10 || w.min() != 1 || w.max() != 4)
    { printf("FAIL valarray basic\n"); failures |= 8; }
    std::valarray<double> d(3);
    d[0] = 1.0; d[1] = 2.0; d[2] = 3.0;
    std::valarray<double> e = d.apply(dbl);
    if (e[0] != 2.0 || e[2] != 6.0) { printf("FAIL apply\n"); failures |= 32; }
    std::valarray<int> sh = w.shift(1);
    std::valarray<int> cs = w.cshift(1);
    if (sh[0] != 1 || sh[3] != 0 || cs[0] != 1 || cs[3] != 4) { printf("FAIL shift/cshift\n"); failures |= 64; }
    std::valarray<int> big(8);
    for (int i = 0; i < 8; i++) big[i] = i;
    std::valarray<int> sl = big[std::slice(1, 3, 2)];
    if (sl.size() != 3 || sl[0] != 1 || sl[1] != 3 || sl[2] != 5) { printf("FAIL slice\n"); failures |= 128; }
    std::valarray<size_t> sizes(2), strides(2);
    sizes[0] = 2; sizes[1] = 2; strides[0] = 4; strides[1] = 1;
    std::valarray<int> gs = big[std::gslice(0, sizes, strides)];
    if (gs.size() != 4 || gs[0] != 0 || gs[1] != 1 || gs[2] != 4 || gs[3] != 5) { printf("FAIL gslice\n"); failures |= 256; }
    std::valarray<size_t> idx(3);
    idx[0] = 7; idx[1] = 0; idx[2] = 2;
    std::valarray<int> ind = big[idx];
    if (ind.size() != 3 || ind[0] != 7 || ind[1] != 0 || ind[2] != 2) { printf("FAIL indirect\n"); failures |= 1024; }
    v.resize(2, 9);
    if (v.size() != 2 || v[1] != 9) { printf("FAIL resize\n"); failures |= 2048; }
    if (failures == 0) printf("PASS std_full_11_complex_valarray\n");
    return failures;
}
