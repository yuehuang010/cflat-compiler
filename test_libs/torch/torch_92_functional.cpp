// C++20 equivalent of torch_92_functional.cb (clang++ parity baseline)
#include <cstdint>
#include <cstdio>
#include <string>
#include <torch/torch.h>

using i64 = int64_t;

int leg_t17()
{
    int failures = 0;
    torch::manual_seed(7);
    i64 shape[2] = {2, 3};
    c10::IntArrayRef sizes = c10::IntArrayRef(&shape[0], 2);
    at::Tensor x = torch::randn(sizes);
    at::Tensor r = torch::nn::functional::relu(x);
    at::Tensor t = torch::zeros(sizes);
    at::Tensor loss = torch::nn::functional::mse_loss(r, t);
    double lv = loss.item<double>();
    int loss_ok = lv >= 0.0;
    if (!loss_ok) { std::printf("FAIL t17 loss_ok: got 0 want 1\n"); failures++; }
    int req = 1;
    {
        torch::NoGradGuard ng;
        at::Tensor y = x * 2.0;
        req = (int)y.requires_grad();
    }
    if (req != 0) { std::printf("FAIL t17 req: got %d want 0\n", req); failures++; }

    i64 sq[2] = {2, 2};
    c10::IntArrayRef sq2 = c10::IntArrayRef(&sq[0], 2);
    at::Tensor o = torch::ones(sq2);
    at::Tensor d = o.to(c10::ScalarType::Double);
    at::Tensor m = torch::matmul(o, o);
    at::Tensor v = o.view(4);
    at::Tensor u = o.unsqueeze(0);
    int dtype = (int)d.scalar_type();
    double msum = m.sum().item<double>();
    i64 vsize = v.size(0);
    i64 udim = u.dim();
    if (dtype != 7) { std::printf("FAIL t17 dtype: got %d want 7\n", dtype); failures++; }
    if (msum != 8.0) { std::printf("FAIL t17 msum: got %f want 8.0\n", msum); failures++; }
    if (vsize != 4) { std::printf("FAIL t17 vsize: got %lld want 4\n", vsize); failures++; }
    if (udim != 3) { std::printf("FAIL t17 udim: got %lld want 3\n", udim); failures++; }
    i64 four = 4;
    at::Tensor vl = o.view(four);
    i64 vlsize = vl.size(0);
    if (vlsize != 4) { std::printf("FAIL t17 vlsize: got %lld want 4\n", vlsize); failures++; }

    at::Tensor w = torch::ones(sq2) * 4.0;
    torch::save(w, "t17.pt");
    at::Tensor back;
    torch::load(back, "t17.pt");
    double s = back.sum().item<double>();
    if (s != 16.0) { std::printf("FAIL t17 sum: got %f want 16.0\n", s); failures++; }

    std::string desc = o.toString();
    at::Tensor sel = o.index_select(0, torch::tensor(0));
    at::Tensor z = torch::zeros_like(o);
    z.fill_(3.0);
    int selsize = (int)sel.size(0);
    double zsum = z.sum().item<double>();
    int desc_ok = desc.size() > 0;
    std::printf("loss_ok=%d req=%d dtype=%d sum=%f desc_ok=%d sel=%d zsum=%f\n", loss_ok, req, dtype, s,
                desc_ok, selsize, zsum);
    if (!desc_ok) { std::printf("FAIL t17 desc: got empty string want non-empty\n"); failures++; }
    if (selsize != 1) { std::printf("FAIL t17 sel: got %d want 1\n", selsize); failures++; }
    if (zsum != 12.0) { std::printf("FAIL t17 zsum: got %f want 12.0\n", zsum); failures++; }
    return failures;
}

int main()
{
    int failures = leg_t17();
    if (failures == 0) std::printf("PASS torch_92_functional\n");
    return failures;
}
