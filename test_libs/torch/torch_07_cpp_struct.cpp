// C++20 equivalent of torch_07_cpp_struct.cb (clang++ parity baseline)
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include <torch/torch.h>

using i64 = int64_t;

struct Net29 : torch::nn::Module
{
    std::shared_ptr<torch::nn::LinearImpl> fc1 = nullptr;
    std::shared_ptr<torch::nn::LinearImpl> fc2 = nullptr;
    Net29()
    {
        fc1 = register_module("fc1", torch::nn::Linear(2, 8));
        fc2 = register_module("fc2", torch::nn::Linear(8, 1));
    }
    at::Tensor forward(at::Tensor x)
    {
        return fc2->forward(torch::relu(fc1->forward(x)));
    }
};

struct Block30 : torch::nn::Module
{
    std::shared_ptr<torch::nn::LinearImpl> fc = nullptr;
    Block30(int nin, int nout)
    {
        fc = register_module("fc", torch::nn::Linear(nin, nout));
    }
    at::Tensor forward(at::Tensor x)
    {
        return torch::relu(fc->forward(x));
    }
};

struct Net30 : torch::nn::Module
{
    std::shared_ptr<Block30> b1 = nullptr;
    std::shared_ptr<torch::nn::LinearImpl> out = nullptr;
    Net30()
    {
        b1 = register_module("b1", std::make_shared<Block30>(2, 8));
        out = register_module("out", torch::nn::Linear(8, 1));
    }
    at::Tensor forward(at::Tensor x)
    {
        return out->forward(b1->forward(x));
    }
};

struct Relu31 { int tag = 0; at::Tensor apply(at::Tensor x) { return torch::relu(x); } };
struct Tanh31 { int tag = 1; at::Tensor apply(at::Tensor x) { return torch::tanh(x); } };

template <class A>
struct Block31 : torch::nn::Module
{
    std::shared_ptr<torch::nn::LinearImpl> fc = nullptr;
    A act = {};
    Block31(int nin, int nout)
    {
        fc = register_module("fc", torch::nn::Linear(nin, nout));
    }
    at::Tensor forward(at::Tensor x)
    {
        return act.apply(fc->forward(x));
    }
};

struct Net31 : torch::nn::Module
{
    std::shared_ptr<Block31<Tanh31>> h1 = nullptr;
    std::shared_ptr<Block31<Relu31>> h2 = nullptr;
    std::shared_ptr<torch::nn::LinearImpl> out = nullptr;
    Net31()
    {
        h1 = register_module("h1", std::make_shared<Block31<Tanh31>>(2, 32));
        h2 = register_module("h2", std::make_shared<Block31<Relu31>>(32, 32));
        out = register_module("out", torch::nn::Linear(32, 2));
    }
    at::Tensor forward(at::Tensor x)
    {
        return out->forward(h2->forward(h1->forward(x)));
    }
};

int leg_t29()
{
    torch::manual_seed(1);
    float xs[8] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f};
    float ys[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    i64 dx[2] = {4, 2};
    i64 dy[2] = {4, 1};
    c10::IntArrayRef sx = c10::IntArrayRef(&dx[0], 2);
    c10::IntArrayRef sy = c10::IntArrayRef(&dy[0], 2);
    at::Tensor x = torch::from_blob(&xs[0], sx);
    at::Tensor y = torch::from_blob(&ys[0], sy);
    Net29 net = Net29();
    std::vector<at::Tensor> params = net.parameters();
    torch::optim::SGD opt = torch::optim::SGD(params, 0.1);
    double first = 0.0;
    double last = 0.0;
    for (int step = 0; step < 800; step++) {
        opt.zero_grad();
        at::Tensor pred = net.forward(x);
        at::Tensor loss = at::mse_loss(pred, y);
        loss.backward();
        opt.step();
        last = loss.item().toDouble();
        if (step == 0) first = last;
    }
    net.train(false);
    int training = net.is_training() ? 1 : 0;
    std::printf("nparams=%d training=%d first=%f last=%g\n", (int)params.size(), training, first, last);
    int failures = 0;
    if (params.size() != 4) { std::printf("FAIL t29 params: got %d want 4\n", (int)params.size()); failures++; }
    if (training != 0) { std::printf("FAIL t29 training: got %d want 0\n", training); failures++; }
    if (!(last < first && last < 0.05)) { std::printf("FAIL t29 loss: got %g want < first and < 0.05\n", last); failures++; }
    return failures;
}

int leg_t30()
{
    torch::manual_seed(1);
    float xs[8] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f};
    float ys[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    i64 dx[2] = {4, 2};
    i64 dy[2] = {4, 1};
    c10::IntArrayRef sx = c10::IntArrayRef(&dx[0], 2);
    c10::IntArrayRef sy = c10::IntArrayRef(&dy[0], 2);
    at::Tensor x = torch::from_blob(&xs[0], sx);
    at::Tensor y = torch::from_blob(&ys[0], sy);
    std::shared_ptr<Net30> net = std::make_shared<Net30>();
    std::shared_ptr<torch::nn::Module> base = net;
    std::vector<at::Tensor> params = base->parameters();
    torch::optim::SGD opt = torch::optim::SGD(params, 0.1);
    double first = 0.0;
    double last = 0.0;
    for (int step = 0; step < 800; step++) {
        opt.zero_grad();
        at::Tensor pred = net->forward(x);
        at::Tensor loss = at::mse_loss(pred, y);
        loss.backward();
        opt.step();
        last = loss.item().toDouble();
        if (step == 0) first = last;
    }
    base->train(false);
    int nchildren = (int)base->children().size();
    int training = net->is_training() ? 1 : 0;
    std::printf("nparams=%d children=%d training=%d first=%f last=%g\n", (int)params.size(), nchildren, training, first, last);
    int failures = 0;
    if (params.size() != 4) { std::printf("FAIL t30 params: got %d want 4\n", (int)params.size()); failures++; }
    if (nchildren != 2) { std::printf("FAIL t30 children: got %d want 2\n", nchildren); failures++; }
    if (training != 0) { std::printf("FAIL t30 training: got %d want 0\n", training); failures++; }
    if (!(last < first && last < 0.05)) { std::printf("FAIL t30 loss: got %g want < first and < 0.05\n", last); failures++; }
    return failures;
}

int leg_t31()
{
    torch::manual_seed(7);
    const int N = 256;
    float xs[512] = {};
    float ys[512] = {};
    for (int i = 0; i < N; i++) {
        float a = (float)((i * 37) % 101) / 100.0f;
        float b = (float)((i * 53 + 11) % 101) / 100.0f;
        xs[i * 2] = a;
        xs[i * 2 + 1] = b;
        ys[i * 2] = a + b;
        ys[i * 2 + 1] = a * b;
    }
    i64 dx[2] = {N, 2};
    c10::IntArrayRef sx = c10::IntArrayRef(&dx[0], 2);
    at::Tensor x = torch::from_blob(&xs[0], sx);
    at::Tensor y = torch::from_blob(&ys[0], sx);
    std::shared_ptr<Net31> net = std::make_shared<Net31>();
    std::shared_ptr<torch::nn::Module> base = net;
    std::vector<at::Tensor> params = base->parameters();
    torch::optim::Adam opt = torch::optim::Adam(params, torch::optim::AdamOptions(0.01));
    double first = 0.0;
    double last = 0.0;
    for (int step = 0; step < 1500; step++) {
        opt.zero_grad();
        at::Tensor loss = at::mse_loss(net->forward(x), y);
        loss.backward();
        opt.step();
        last = loss.item().toDouble();
        if (step == 0) first = last;
    }
    base->train(false);
    float qs[6] = {0.3f, 0.4f, 0.9f, 0.5f, 0.25f, 0.25f};
    i64 dq[2] = {3, 2};
    c10::IntArrayRef sq = c10::IntArrayRef(&dq[0], 2);
    at::Tensor q = torch::from_blob(&qs[0], sq);
    at::Tensor pred = net->forward(q);
    int failures = 0;
    for (int r = 0; r < 3; r++) {
        float a = qs[r * 2];
        float b = qs[r * 2 + 1];
        double sum = pred[r][0].item().toDouble();
        double prod = pred[r][1].item().toDouble();
        std::printf("%.2f + %.2f = %.3f (want %.3f)   %.2f * %.2f = %.3f (want %.3f)\n", a, b, sum, a + b, a, b, prod, a * b);
        if (sum < a + b - 0.05 || sum > a + b + 0.05) { std::printf("FAIL t31 sum row %d: got %f want %f +/- 0.05\n", r, sum, a + b); failures++; }
        if (prod < a * b - 0.05 || prod > a * b + 0.05) { std::printf("FAIL t31 product row %d: got %f want %f +/- 0.05\n", r, prod, a * b); failures++; }
    }
    int children = (int)base->children().size();
    std::printf("nparams=%d children=%d first=%f last=%g\n", (int)params.size(), children, first, last);
    if (params.size() != 6) { std::printf("FAIL t31 params: got %d want 6\n", (int)params.size()); failures++; }
    if (children != 3) { std::printf("FAIL t31 children: got %d want 3\n", children); failures++; }
    if (!(last < first)) { std::printf("FAIL t31 loss: got %g want < first (%f)\n", last, first); failures++; }
    return failures;
}

int main()
{
    int failures = 0;
    failures += leg_t29();
    failures += leg_t30();
    failures += leg_t31();
    if (failures == 0) std::printf("PASS torch_07_cpp_struct\n");
    return failures;
}
