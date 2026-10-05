#include <string>
#include <random>
#include <cstdio>
int main()
{
    int failures = 0;
    std::minstd_rand mr;
    mr.discard(9999);
    unsigned long long m10k = mr();
    std::ranlux24 r24;
    r24.discard(9999);
    unsigned long long r24v = r24();
    std::knuth_b kb;
    kb.discard(9999);
    unsigned long long kbv = kb();
    std::mt19937 mt;
    mt.discard(9999);
    unsigned long long mtv = mt();
    if (m10k != 399268537ULL || r24v != 9901578ULL || kbv != 1112339016ULL || mtv != 4123659995ULL) { printf("FAIL 10000th values\n"); failures |= 1; }
    std::minstd_rand a(42), b(42);
    bool same = a == b;
    a.discard(3);
    bool differ = a != b;
    b.discard(3);
    bool again = a == b;
    a.seed(42);
    b.seed(42);
    bool reseeded = a() == b();
    if (!same || !differ || !again || !reseeded || std::minstd_rand::min() != 1 || std::minstd_rand::max() != 2147483646ULL || std::mt19937::min() != 0 || std::mt19937::max() != 4294967295ULL)
    { printf("FAIL engine ops\n"); failures |= 2; }
    std::discard_block_engine<std::ranlux24_base, 223, 23> dbe;
    std::discard_block_engine<std::ranlux24_base, 223, 23> dbe2;
    std::independent_bits_engine<std::mt19937, 16, unsigned int> ibe;
    unsigned int iv = ibe();
    std::shuffle_order_engine<std::minstd_rand0, 256> soe;
    unsigned long long sv = soe();
    std::ranlux24 ref;
    unsigned long long d1 = dbe();
    unsigned long long d2 = dbe2();
    unsigned long long r1 = ref();
    if (d1 != r1 || d2 != r1 || iv >= 65536u || sv == 0 || std::ranlux24::min() != 0 || std::ranlux24::max() != 16777215ULL)
    { printf("FAIL adaptors\n"); failures |= 4; }
    std::seed_seq seq{1, 2, 3};
    unsigned int out[4] = {0, 0, 0, 0};
    seq.generate(out, out + 4);
    std::seed_seq seq2{1, 2, 3};
    unsigned int out2[4] = {0, 0, 0, 0};
    seq2.generate(out2, out2 + 4);
    bool eq = out[0] == out2[0] && out[3] == out2[3] && seq.size() == 3 && (out[0] != 0 || out[1] != 0);
    std::seed_seq seq3{3, 2, 1};
    unsigned int out3[4] = {0, 0, 0, 0};
    seq3.generate(out3, out3 + 4);
    bool diff = out3[0] != out[0] || out3[1] != out[1];
    if (!eq || !diff) { printf("FAIL seed_seq\n"); failures |= 8; }
    std::mt19937 gen(5489u);
    std::uniform_int_distribution<int> ui(3, 9);
    std::uniform_real_distribution<double> ur(1.0, 2.0);
    std::bernoulli_distribution bd(0.5);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::uniform_int_distribution<int> u2(1, 6);
    bool inrange = true;
    for (int i = 0; i < 200; i++)
    {
        int x = ui(gen); double y = ur(gen); int z = u2(gen);
        if (x < 3 || x > 9 || y < 1.0 || y >= 2.0 || z < 1 || z > 6) inrange = false;
        (void)bd(gen); (void)nd(gen);
    }
    if (!inrange || ui.a() != 3 || ui.b() != 9 || ur.a() != 1.0 || ur.b() != 2.0 || ui.min() != 3 || ui.max() != 9 || bd.p() != 0.5 || nd.mean() != 0.0 || nd.stddev() != 1.0)
    { printf("FAIL distributions\n"); failures |= 16; }
    std::random_device rd;
    unsigned int rv = rd();
    (void)rv;
    if (std::random_device::max() == 0) { printf("FAIL random_device\n"); failures |= 32; }
    if (failures == 0) printf("PASS std_full_11_random\n");
    return failures;
}
