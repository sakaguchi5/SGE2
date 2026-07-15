#pragma once

#include "Result.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace sge::base
{
[[nodiscard]] Result<std::vector<std::byte>, std::string> ReadAllBytes(const std::filesystem::path& path);
[[nodiscard]] Result<void, std::string> WriteAllBytes(const std::filesystem::path& path, std::span<const std::byte> bytes);
}
