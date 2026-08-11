#include "Sha256.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

namespace
{
constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

constexpr uint32_t RotateRight(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32u - count));
}

constexpr uint32_t Ch(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

constexpr uint32_t Maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}
}

Sha256::Sha256()
    : state_{ 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u },
      bitCount_(0), buffer_{}, bufferSize_(0)
{
}

void Sha256::Transform(const uint8_t* block)
{
    uint32_t words[64] = {};
    for (size_t i = 0; i < 16; ++i)
    {
        words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (size_t i = 16; i < 64; ++i)
    {
        uint32_t s0 = RotateRight(words[i - 15], 7) ^
                      RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 = RotateRight(words[i - 2], 17) ^
                      RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (size_t i = 0; i < 64; ++i)
    {
        uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        uint32_t temp1 = h + s1 + Ch(e, f, g) + kRoundConstants[i] + words[i];
        uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        uint32_t temp2 = s0 + Maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::Update(const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    bitCount_ += static_cast<uint64_t>(size) * 8u;

    while (size > 0)
    {
        size_t copied = std::min(size, buffer_.size() - bufferSize_);
        std::copy(bytes, bytes + copied, buffer_.begin() + bufferSize_);
        bufferSize_ += copied;
        bytes += copied;
        size -= copied;
        if (bufferSize_ == buffer_.size())
        {
            Transform(buffer_.data());
            bufferSize_ = 0;
        }
    }
}

void Sha256::Update(std::string_view data)
{
    Update(data.data(), data.size());
}

std::array<uint8_t, 32> Sha256::Final()
{
    uint64_t originalBitCount = bitCount_;
    uint8_t padding = 0x80;
    Update(&padding, 1);
    uint8_t zero = 0;
    while (bufferSize_ != 56)
        Update(&zero, 1);

    uint8_t length[8];
    for (size_t i = 0; i < 8; ++i)
        length[7 - i] = static_cast<uint8_t>(originalBitCount >> (i * 8));
    Update(length, sizeof(length));

    std::array<uint8_t, 32> result{};
    for (size_t i = 0; i < 8; ++i)
    {
        result[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
        result[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        result[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        result[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
    return result;
}

std::string Sha256::Hex(const std::array<uint8_t, 32>& digest)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (uint8_t byte : digest)
    {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

bool Sha256::File(const std::filesystem::path& path,
                  std::array<uint8_t, 32>& digest,
                  uint64_t& size,
                  std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "could not open " + path.string();
        return false;
    }

    Sha256 hash;
    std::vector<char> buffer(1024 * 1024);
    size = 0;
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize count = input.gcount();
        if (count > 0)
        {
            hash.Update(buffer.data(), static_cast<size_t>(count));
            size += static_cast<uint64_t>(count);
        }
    }
    if (!input.eof())
    {
        error = "could not read " + path.string();
        return false;
    }
    digest = hash.Final();
    return true;
}
