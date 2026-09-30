// C++20 equivalent of torch_93_cross_entropy.cb (clang++ parity baseline)
#include <cstdint>
#include <cstdio>
#include <string>
#include <torch/torch.h>

using i64 = int64_t;

int leg_t23()
{
    int failures = 0;
    torch::manual_seed(7);
    at::Tensor x = torch::randn({4, 3});
    float* p = x.data_ptr<float>();
    double p0 = p[0];
    double item = x[0][0].item<double>();
    std::printf("p0=%f item=%f\n", p0, item);
    if (p0 < -0.146796 || p0 > -0.146794) { std::printf("FAIL t23 p0: got %f want -0.146795\n", p0); failures++; }
    if (item < -0.146796 || item > -0.146794) { std::printf("FAIL t23 item: got %f want -0.146795\n", item); failures++; }
    at::Tensor logits = torch::randn({4, 5});
    i64 tsz[1] = {4};
    at::Tensor target = torch::randint(0, 5, c10::IntArrayRef(&tsz[0], 1));
    at::Tensor loss = torch::nn::functional::cross_entropy(logits, target);
    double ce = loss.item<double>();
    std::printf("ce=%f\n", ce);
    if (ce < 2.463949 || ce > 2.463951) { std::printf("FAIL t23 ce: got %f want 2.463950\n", ce); failures++; }
    torch::nn::Linear lin = torch::nn::Linear(3, 5);
    torch::optim::SGD sgd = torch::optim::SGD(lin->parameters(), torch::optim::SGDOptions(0.1).momentum(0.9));
    for (int i = 0; i < 3; i++) {
        sgd.zero_grad();
        at::Tensor out = lin->forward(x);
        at::Tensor l = torch::nn::functional::cross_entropy(out, target);
        l.backward();
        sgd.step();
    }
    at::Tensor am = torch::argmax(logits, 1);
    int amsize = (int)am.size(0);
    int n = (int)am.sum().item<i64>();
    std::printf("am=%d n=%d\n", amsize, n);
    if (amsize != 4) { std::printf("FAIL t23 am: got %d want 4\n", amsize); failures++; }
    if (n != 10) { std::printf("FAIL t23 n: got %d want 10\n", n); failures++; }
    return failures;
}

int main()
{
    int failures = leg_t23();
    if (failures == 0) std::printf("PASS torch_93_cross_entropy\n");
    return failures;
}
