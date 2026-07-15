#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace sge::base
{
using Digest256 = std::array<std::byte, 32>;

[[nodiscard]] Digest256 Sha256(std::span<const std::byte> bytes);
[[nodiscard]] std::string ToHex(const Digest256& digest);
}
