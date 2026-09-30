// C++20 equivalent of torch_90_init_list.cb (clang++ parity baseline)
#include <cstdint>
#include <cstdio>
#include <string>
#include <torch/torch.h>

using i64 = int64_t;

int leg_t4()
{
    int failures = 0;
    i64 dims2[2] = {2, 3};
    c10::IntArrayRef s23 = c10::IntArrayRef(&dims2[0], 2);
    i64 dims3[2] = {3, 1};
    c10::IntArrayRef s31 = c10::IntArrayRef(&dims3[0], 2);
    at::Tensor a = torch::ones(s23);
    at::Tensor w = torch::ones(s31);
    at::Tensor wg = w.requires_grad_(true);
    at::Tensor y = at::matmul(a, wg);
    at::Tensor loss = at::sum(y);
    loss.backward();
    at::Tensor g = wg.grad();
    double lv = loss.item().toDouble();
    double g0 = g.index({0, 0}).item().toDouble();
    std::printf("loss=%f g0=%f dim=%lld\n", lv, g0, g.dim());
    if (lv != 6.0) { std::printf("FAIL t4 loss: got %f want 6.0\n", lv); failures++; }
    if (g0 != 2.0) { std::printf("FAIL t4 g0: got %f want 2.0\n", g0); failures++; }
    if (g.dim() != 2) { std::printf("FAIL t4 dim: got %lld want 2\n", g.dim()); failures++; }
    return failures;
}

int leg_t27_braced()
{
    int failures = 0;
    at::Tensor x = torch::arange(12).reshape({3, 4});
    at::Tensor col = x.index({torch::indexing::Slice(), 1});
    at::Tensor row = x.index({0});
    at::Tensor sub = x.index({torch::indexing::Slice(0, 2), torch::indexing::Slice(1, 3)});
    i64 s = sub.sum().item<i64>();
    if (col.size(0) != 3) { std::printf("FAIL t27 col: got %lld want 3\n", (i64)col.size(0)); failures++; }
    if (row.size(0) != 4) { std::printf("FAIL t27 row: got %lld want 4\n", (i64)row.size(0)); failures++; }
    if (sub.size(0) != 2 || sub.size(1) != 2) { std::printf("FAIL t27 sub: got %lldx%lld want 2x2\n", (i64)sub.size(0), (i64)sub.size(1)); failures++; }
    if (s != 14) { std::printf("FAIL t27 sum: got %lld want 14\n", (i64)s); failures++; }
    return failures;
}

int main()
{
    int failures = leg_t4() + leg_t27_braced();
    if (failures == 0) std::printf("PASS torch_90_init_list\n");
    return failures;
}
