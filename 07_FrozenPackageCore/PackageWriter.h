#pragma once

#include "../00_Base/Result.h"
#include "PackageFormat.h"

#include <cstddef>
#include <vector>

namespace sge::package
{
class PackageWriter final
{
public:
    [[nodiscard]] static base::Result<std::vector<std::byte>, PackageError> Write(PackageBuildInput input);
};
}
