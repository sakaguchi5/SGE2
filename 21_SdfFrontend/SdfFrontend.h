#pragma once

#include "../00_Base/Result.h"
#include "../02_SemanticModel/SemanticModel.h"
#include "../23_ExperimentDomain/ExperimentDomain.h"

#include <array>
#include <string>

namespace sge::sdf
{
struct HalfSpace2D final
{
    // Interior points satisfy a*x + b*y + c <= 0.
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
};

struct TriangleField final
{
    std::array<HalfSpace2D, 3> halfSpaces{}; // base, right edge, left edge
    float depth = 0.0f;
    math::Float4 color{};
};

struct SdfTriangleDescription final
{
    TriangleField nearField;
    TriangleField farField;
};

[[nodiscard]] SdfTriangleDescription BuildTriangleDescription() noexcept;
[[nodiscard]] float EvaluateSignedDistance(const TriangleField& field, math::Float2 point) noexcept;
[[nodiscard]] base::Result<experiment::TriangleGeometry, std::string> BuildTriangleGeometry();
[[nodiscard]] base::Result<semantic::SemanticGraph, std::string> BuildTriangleGraph();
}
