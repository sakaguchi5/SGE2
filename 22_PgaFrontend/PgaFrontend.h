#pragma once

#include "../00_Base/Result.h"
#include "../02_SemanticModel/SemanticModel.h"
#include "../23_ExperimentDomain/ExperimentDomain.h"

#include <array>
#include <string>

namespace sge::pga
{
// 2D projective geometric algebra coordinates.
// A finite point is P = x*e20 + y*e01 + w*e12.
struct Point2 final
{
    float e20 = 0.0f;
    float e01 = 0.0f;
    float e12 = 1.0f;
};

// A line is l = a*e1 + b*e2 + c*e0 and represents a*x + b*y + c*w = 0.
struct Line2 final
{
    float e1 = 0.0f;
    float e2 = 0.0f;
    float e0 = 0.0f;
};

struct TriangleLayer final
{
    std::array<Line2, 3> boundaryLines{}; // base, right edge, left edge
    float depth = 0.0f;
    math::Float4 color{};
};

struct TriangleDescription final
{
    TriangleLayer nearLayer;
    TriangleLayer farLayer;
};

[[nodiscard]] Line2 Join(Point2 first, Point2 second) noexcept;
[[nodiscard]] base::Result<Point2, std::string> Meet(Line2 first, Line2 second);
[[nodiscard]] float Incidence(Line2 line, Point2 point) noexcept;
[[nodiscard]] bool ProjectivelyEquivalent(Line2 first, Line2 second, float tolerance = 0.000001f) noexcept;

[[nodiscard]] TriangleDescription BuildTriangleDescription() noexcept;
[[nodiscard]] base::Result<experiment::TriangleGeometry, std::string> BuildTriangleGeometry();
[[nodiscard]] base::Result<semantic::SemanticGraph, std::string> BuildTriangleGraph();
}
