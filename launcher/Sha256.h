#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

class Sha256
{
public:
    Sha256();

    void Update(const void* data, size_t size);
    void Update(std::string_view data);
    std::array<uint8_t, 32> Final();

    static std::string Hex(const std::array<uint8_t, 32>& digest);
    static bool File(const std::filesystem::path& path,
                     std::array<uint8_t, 32>& digest,
                     uint64_t& size,
                     std::string& error);

private:
    void Transform(const uint8_t* block);

    uint32_t state_[8];
    uint64_t bitCount_;
    std::array<uint8_t, 64> buffer_;
    size_t bufferSize_;
};
