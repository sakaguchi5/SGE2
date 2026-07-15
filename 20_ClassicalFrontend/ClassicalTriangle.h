#pragma once

#include "../00_Base/Result.h"
#include "../02_SemanticModel/SemanticModel.h"
#include "../23_ExperimentDomain/ExperimentDomain.h"

#include <string>

namespace sge::classical
{
[[nodiscard]] experiment::TriangleGeometry BuildTriangleGeometry() noexcept;
[[nodiscard]] base::Result<semantic::SemanticGraph, std::string> BuildTriangleGraph();
}
