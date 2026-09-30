// C++20 equivalent of torch_03_modules.cb (clang++ parity baseline)
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <torch/torch.h>

using i64 = int64_t;

int leg_t6()
{
    float data[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    i64 dims[2] = {2, 3};
    c10::IntArrayRef s23 = c10::IntArrayRef(&dims[0], 2);
    at::Tensor x = torch::from_blob(&data[0], s23);
    double total = at::sum(x).item().toDouble();
    c10::TensorOptions opts = c10::TensorOptions(c10::ScalarType::Double);
    at::Tensor z = torch::zeros(s23, opts);
    torch::nn::Linear lin = torch::nn::Linear(3, 1);
    at::Tensor y = lin->forward(x);
    i64 y0 = y.size(0);
    i64 y1 = y.size(1);
    int zdtype = (int)z.scalar_type();
    std::printf("t6 total=%f zdtype=%d ysize0=%lld ysize1=%lld\n", total, zdtype, y0, y1);
    int fail = 0;
    if (total != 21.0) { std::printf("FAIL t6 total: got %f want 21\n", total); fail++; }
    if (zdtype != (int)c10::ScalarType::Double) { std::printf("FAIL t6 dtype: got %d want %d\n", zdtype, (int)c10::ScalarType::Double); fail++; }
    if (y0 != 2 || y1 != 1) { std::printf("FAIL t6 output shape: got %lldx%lld want 2x1\n", y0, y1); fail++; }
    return fail;
}

int leg_t11()
{
    torch::manual_seed(3);
    float img[16];
    for (int i = 0; i < 16; i++) img[i] = (float)(i + 1);
    i64 d[4] = {1, 1, 4, 4};
    c10::IntArrayRef s = c10::IntArrayRef(&d[0], 4);
    at::Tensor x = torch::from_blob(&img[0], s);
    torch::nn::Conv2d conv = torch::nn::Conv2d(1, 2, 3);
    {
        torch::NoGradGuard ng;
        conv->weight.fill_(1.0);
        conv->bias.fill_(0.0);
    }
    at::Tensor y = conv->forward(x);
    i64 ydim = y.dim();
    i64 y1 = y.size(1);
    i64 y2 = y.size(2);
    double ySum = at::sum(y).item().toDouble();
    i64 k[2] = {2, 2};
    c10::IntArrayRef ks = c10::IntArrayRef(&k[0], 2);
    at::Tensor p = at::max_pool2d(y, ks);
    double pSum = at::sum(p).item().toDouble();
    at::Tensor flat = at::flatten(p, 1);
    i64 f0 = flat.size(0);
    i64 f1 = flat.size(1);
    std::printf("t11 ydim=%lld y=%lldx%lld ySum=%f pSum=%f flat=%lldx%lld\n", ydim, y1, y2, ySum, pSum, f0, f1);
    int fail = 0;
    if (ydim != 4 || y1 != 2 || y2 != 2) { std::printf("FAIL t11 conv shape: got dim=%lld %lldx%lld want dim=4 2x2\n", ydim, y1, y2); fail++; }
    if (ySum != 612.0) { std::printf("FAIL t11 conv sum: got %f want 612\n", ySum); fail++; }
    if (pSum != 198.0) { std::printf("FAIL t11 pool sum: got %f want 198\n", pSum); fail++; }
    if (f0 != 1 || f1 != 2) { std::printf("FAIL t11 flat shape: got %lldx%lld want 1x2\n", f0, f1); fail++; }
    return fail;
}

int leg_t19()
{
    i64 shape[2] = {4, 8};
    c10::IntArrayRef sizes = c10::IntArrayRef(&shape[0], 2);
    at::Tensor x = torch::randn(sizes);
    torch::nn::BatchNorm1d bn = torch::nn::BatchNorm1d(8);
    torch::nn::Dropout dr = torch::nn::Dropout(0.5);
    at::Tensor y = bn->forward(x);
    bn->eval();
    dr->eval();
    at::Tensor z = dr->forward(y);
    i64 y0 = y.size(0);
    i64 y1 = y.size(1);
    i64 z0 = z.size(0);
    int training = (int)bn->is_training();
    int okShape = y1 == 8 && training == 0 && z0 == 4;
    at::Tensor d = x.to(torch::kFloat64);
    at::Tensor i = x.to(torch::kInt64);
    int dType = (int)d.scalar_type();
    int iType = (int)i.scalar_type();
    i64 s2[2] = {2, 3};
    c10::IntArrayRef sz2 = c10::IntArrayRef(&s2[0], 2);
    at::Tensor a = torch::arange(6).view(sz2);
    std::tuple<at::Tensor, at::Tensor> mx = torch::max(a, 1);
    at::Tensor vals = std::get<0>(mx);
    at::Tensor idxs = std::get<1>(mx);
    double v0 = vals[0].item<double>();
    i64 i0 = idxs[0].item<i64>();
    std::printf("t19 y=%dx%d d=%d i=%d v0=%f i0=%d\n", (int)y0, (int)y1, dType, iType, v0, (int)i0);
    int fail = 0;
    if (!okShape) { std::printf("FAIL t19 module shape/training: got %lldx%lld training=%d zrows=%lld want 4x8 0 4\n", y0, y1, training, z0); fail++; }
    if (dType != (int)c10::ScalarType::Double || iType != (int)c10::ScalarType::Long) { std::printf("FAIL t19 dtypes: got %d,%d want %d,%d\n", dType, iType, (int)c10::ScalarType::Double, (int)c10::ScalarType::Long); fail++; }
    if (v0 != 2.0 || i0 != 2) { std::printf("FAIL t19 max: got value=%f index=%d want 2,2\n", v0, (int)i0); fail++; }
    return fail;
}

int leg_t24()
{
    torch::manual_seed(3);
    torch::nn::LSTM lstm = torch::nn::LSTM(torch::nn::LSTMOptions(4, 6).num_layers(1));
    i64 shape[3] = {5, 2, 4};
    c10::IntArrayRef sizes = c10::IntArrayRef(&shape[0], 3);
    at::Tensor x = torch::randn(sizes);
    std::tuple<at::Tensor, std::tuple<at::Tensor, at::Tensor>> out = lstm->forward(x);
    at::Tensor y = std::get<0>(out);
    std::tuple<at::Tensor, at::Tensor> hc = std::get<1>(out);
    at::Tensor h = std::get<0>(hc);
    i64 y0 = y.size(0);
    i64 y1 = y.size(1);
    i64 y2 = y.size(2);
    i64 h0 = h.size(0);
    i64 h1 = h.size(1);
    i64 h2 = h.size(2);
    std::printf("t24 y=%dx%dx%d h=%dx%dx%d\n", (int)y0, (int)y1, (int)y2, (int)h0, (int)h1, (int)h2);
    int fail = 0;
    if (y0 != 5 || y1 != 2 || y2 != 6) { std::printf("FAIL t24 output: got %lldx%lldx%lld want 5x2x6\n", y0, y1, y2); fail++; }
    if (h0 != 1 || h1 != 2 || h2 != 6) { std::printf("FAIL t24 hidden: got %lldx%lldx%lld want 1x2x6\n", h0, h1, h2); fail++; }
    return fail;
}

int main()
{
    int fail = 0;
    fail += leg_t6();
    fail += leg_t11();
    fail += leg_t19();
    fail += leg_t24();
    if (!fail) std::printf("PASS torch_03_modules\n");
    return fail;
}
