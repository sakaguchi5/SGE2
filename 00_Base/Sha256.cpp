#include "Sha256.h"

#include <array>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

namespace sge::base
{
namespace
{
constexpr std::array<std::uint32_t, 64> K = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

constexpr std::uint32_t Ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (~x & z); }
constexpr std::uint32_t Maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
constexpr std::uint32_t S0(std::uint32_t x) { return std::rotr(x,2) ^ std::rotr(x,13) ^ std::rotr(x,22); }
constexpr std::uint32_t S1(std::uint32_t x) { return std::rotr(x,6) ^ std::rotr(x,11) ^ std::rotr(x,25); }
constexpr std::uint32_t s0(std::uint32_t x) { return std::rotr(x,7) ^ std::rotr(x,18) ^ (x >> 3); }
constexpr std::uint32_t s1(std::uint32_t x) { return std::rotr(x,17) ^ std::rotr(x,19) ^ (x >> 10); }
}

Digest256 Sha256(std::span<const std::byte> bytes)
{
    std::vector<std::byte> data(bytes.begin(), bytes.end());
    const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8u;
    data.push_back(std::byte{0x80});
    while ((data.size() % 64) != 56) data.push_back(std::byte{0});
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::byte>((bitLength >> (i * 8)) & 0xffu));

    std::array<std::uint32_t, 8> h = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    std::array<std::uint32_t, 64> w{};
    for (std::size_t chunk = 0; chunk < data.size(); chunk += 64)
    {
        for (std::size_t i = 0; i < 16; ++i)
        {
            const auto p = chunk + i * 4;
            w[i] = (std::to_integer<std::uint32_t>(data[p]) << 24) |
                   (std::to_integer<std::uint32_t>(data[p+1]) << 16) |
                   (std::to_integer<std::uint32_t>(data[p+2]) << 8) |
                   std::to_integer<std::uint32_t>(data[p+3]);
        }
        for (std::size_t i = 16; i < 64; ++i) w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];

        auto a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
        for (std::size_t i = 0; i < 64; ++i)
        {
            const auto t1 = hh + S1(e) + Ch(e,f,g) + K[i] + w[i];
            const auto t2 = S0(a) + Maj(a,b,c);
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    Digest256 out{};
    for (std::size_t i = 0; i < h.size(); ++i)
        for (std::size_t j = 0; j < 4; ++j)
            out[i*4+j] = static_cast<std::byte>((h[i] >> ((3-j)*8)) & 0xffu);
    return out;
}

std::string ToHex(const Digest256& digest)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (auto byte : digest) stream << std::setw(2) << static_cast<unsigned>(std::to_integer<std::uint8_t>(byte));
    return stream.str();
}
}
