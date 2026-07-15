#pragma once

#include <cstdint>
#include <limits>

namespace sge::base
{
[[nodiscard]] inline bool CheckedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept
{
    if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
    out = a + b;
    return true;
}

[[nodiscard]] inline bool IsPowerOfTwo(std::uint32_t value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] inline std::uint64_t AlignUp(std::uint64_t value, std::uint32_t alignment) noexcept
{
    const auto mask = static_cast<std::uint64_t>(alignment - 1u);
    return (value + mask) & ~mask;
}
}
