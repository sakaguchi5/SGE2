#pragma once

#include <compare>
#include <cstdint>

namespace sge::base
{
inline constexpr std::uint32_t InvalidIndex = 0xffff'ffffu;

template<class Tag>
struct Id32 final
{
    std::uint32_t value = InvalidIndex;
    [[nodiscard]] constexpr bool IsValid() const noexcept { return value != InvalidIndex; }
    auto operator<=>(const Id32&) const = default;
};
}
