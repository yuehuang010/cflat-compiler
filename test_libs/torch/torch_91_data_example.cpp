// C++20 equivalent of torch_91_data_example.cb (clang++ parity baseline)
#include <cstdint>
#include <cstdio>
#include <torch/torch.h>

using i64 = int64_t;

int leg_t13()
{
    int failures = 0;
    i64 d[2] = {4, 2};
    c10::IntArrayRef s = c10::IntArrayRef(&d[0], 2);
    at::Tensor x = torch::randn(s);
    torch::data::datasets::TensorDataset ds = torch::data::datasets::TensorDataset(x);
    i64 n = (i64)ds.size().value();
    torch::data::TensorExample ex = ds.get(1);
    std::printf("n=%lld ex0=%lld\n", n, ex.data.size(0));
    if (n != 4) { std::printf("FAIL t13 n: got %lld want 4\n", n); failures++; }
    if (ex.data.size(0) != 2) { std::printf("FAIL t13 ex0: got %lld want 2\n", ex.data.size(0)); failures++; }
    return failures;
}

int main()
{
    int failures = leg_t13();
    if (failures == 0) std::printf("PASS torch_91_data_example\n");
    return failures;
}
