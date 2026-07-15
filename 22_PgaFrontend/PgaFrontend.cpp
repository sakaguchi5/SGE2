#include "PgaFrontend.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sge::pga
{
namespace
{
constexpr std::size_t BaseLine = 0;
constexpr std::size_t RightLine = 1;
constexpr std::size_t LeftLine = 2;
constexpr float ProjectiveTolerance = 0.000001f;

float CanonicalZero(float value) noexcept
{
    return value == 0.0f ? 0.0f : value;
}

bool IsFinite(Point2 point) noexcept
{
    return std::isfinite(point.e20) && std::isfinite(point.e01) && std::isfinite(point.e12);
}

bool IsFinite(Line2 line) noexcept
{
    return std::isfinite(line.e1) && std::isfinite(line.e2) && std::isfinite(line.e0);
}

TriangleLayer MakeLayer(const experiment::IsoscelesTriangleLayer& layer) noexcept
{
    const float rise = layer.apexY - layer.baseY;

    TriangleLayer result;
    // Oriented PGA line arrangement. Interior points evaluate <= 0.
    result.boundaryLines[BaseLine] = {0.0f, -1.0f, layer.baseY};
    result.boundaryLines[RightLine] = {
        rise,
        layer.halfWidth,
        -rise * layer.centerX - layer.halfWidth * layer.apexY};
    result.boundaryLines[LeftLine] = {
        -rise,
        layer.halfWidth,
        rise * layer.centerX - layer.halfWidth * layer.apexY};
    result.depth = layer.depth;
    result.color = layer.color;
    return result;
}

experiment::TriangleVertex MakeVertex(
    Point2 point,
    const TriangleLayer& layer,
    float u,
    float v) noexcept
{
    return {{point.e20, point.e01, layer.depth},
            {layer.color.x, layer.color.y, layer.color.z, layer.color.w},
            {u, v}};
}

base::Result<void, std::string> RequireIncidence(
    Line2 line,
    Point2 point,
    const char* label)
{
    if (std::abs(Incidence(line, point)) > ProjectiveTolerance)
        return base::Result<void, std::string>::Failure(
            std::string("PGA incidence contract failed for ") + label);
    return base::Result<void, std::string>::Success();
}

base::Result<void, std::string> WriteLayerGeometry(
    experiment::TriangleGeometry& geometry,
    std::size_t firstVertex,
    const TriangleLayer& layer)
{
    for (const auto line : layer.boundaryLines)
        if (!IsFinite(line))
            return base::Result<void, std::string>::Failure("PGA triangle contains a non-finite line");

    const auto apex = Meet(layer.boundaryLines[LeftLine], layer.boundaryLines[RightLine]);
    if (!apex) return base::Result<void, std::string>::Failure(apex.Error());
    const auto rightBase = Meet(layer.boundaryLines[RightLine], layer.boundaryLines[BaseLine]);
    if (!rightBase) return base::Result<void, std::string>::Failure(rightBase.Error());
    const auto leftBase = Meet(layer.boundaryLines[BaseLine], layer.boundaryLines[LeftLine]);
    if (!leftBase) return base::Result<void, std::string>::Failure(leftBase.Error());

    for (const auto& check : {
        RequireIncidence(layer.boundaryLines[LeftLine], apex.Value(), "apex/left"),
        RequireIncidence(layer.boundaryLines[RightLine], apex.Value(), "apex/right"),
        RequireIncidence(layer.boundaryLines[RightLine], rightBase.Value(), "right-base/right"),
        RequireIncidence(layer.boundaryLines[BaseLine], rightBase.Value(), "right-base/base"),
        RequireIncidence(layer.boundaryLines[BaseLine], leftBase.Value(), "left-base/base"),
        RequireIncidence(layer.boundaryLines[LeftLine], leftBase.Value(), "left-base/left")})
    {
        if (!check) return base::Result<void, std::string>::Failure(check.Error());
    }

    if (!ProjectivelyEquivalent(Join(apex.Value(), rightBase.Value()), layer.boundaryLines[RightLine]) ||
        !ProjectivelyEquivalent(Join(rightBase.Value(), leftBase.Value()), layer.boundaryLines[BaseLine]) ||
        !ProjectivelyEquivalent(Join(leftBase.Value(), apex.Value()), layer.boundaryLines[LeftLine]))
        return base::Result<void, std::string>::Failure("PGA join did not recover the original triangle line arrangement");

    const Point2 centroid{
        (apex.Value().e20 + rightBase.Value().e20 + leftBase.Value().e20) / 3.0f,
        (apex.Value().e01 + rightBase.Value().e01 + leftBase.Value().e01) / 3.0f,
        1.0f};
    for (const auto line : layer.boundaryLines)
        if (Incidence(line, centroid) > ProjectiveTolerance)
            return base::Result<void, std::string>::Failure("PGA triangle centroid lies outside the oriented line arrangement");

    geometry.vertices[firstVertex + 0] = MakeVertex(apex.Value(), layer, 0.5f, 0.0f);
    geometry.vertices[firstVertex + 1] = MakeVertex(rightBase.Value(), layer, 1.0f, 1.0f);
    geometry.vertices[firstVertex + 2] = MakeVertex(leftBase.Value(), layer, 0.0f, 1.0f);
    return base::Result<void, std::string>::Success();
}
}

Line2 Join(Point2 first, Point2 second) noexcept
{
    // Regressive product of two PGA points, represented as the homogeneous cross product.
    return {
        CanonicalZero(first.e01 * second.e12 - first.e12 * second.e01),
        CanonicalZero(first.e12 * second.e20 - first.e20 * second.e12),
        CanonicalZero(first.e20 * second.e01 - first.e01 * second.e20)};
}

base::Result<Point2, std::string> Meet(Line2 first, Line2 second)
{
    if (!IsFinite(first) || !IsFinite(second))
        return base::Result<Point2, std::string>::Failure("PGA meet received a non-finite line");

    // Outer product of two PGA lines. e12 is the homogeneous finite weight.
    const Point2 homogeneous{
        first.e2 * second.e0 - first.e0 * second.e2,
        first.e0 * second.e1 - first.e1 * second.e0,
        first.e1 * second.e2 - first.e2 * second.e1};

    if (!IsFinite(homogeneous) || std::abs(homogeneous.e12) <= ProjectiveTolerance)
        return base::Result<Point2, std::string>::Failure(
            "PGA line meet is ideal, parallel, or numerically unstable");

    const Point2 normalized{
        CanonicalZero(homogeneous.e20 / homogeneous.e12),
        CanonicalZero(homogeneous.e01 / homogeneous.e12),
        1.0f};
    if (!IsFinite(normalized))
        return base::Result<Point2, std::string>::Failure("PGA finite-point normalization produced a non-finite result");

    return base::Result<Point2, std::string>::Success(normalized);
}

float Incidence(Line2 line, Point2 point) noexcept
{
    return line.e1 * point.e20 + line.e2 * point.e01 + line.e0 * point.e12;
}

bool ProjectivelyEquivalent(Line2 first, Line2 second, float tolerance) noexcept
{
    if (!IsFinite(first) || !IsFinite(second) || tolerance < 0.0f || !std::isfinite(tolerance))
        return false;

    const float c0 = first.e2 * second.e0 - first.e0 * second.e2;
    const float c1 = first.e0 * second.e1 - first.e1 * second.e0;
    const float c2 = first.e1 * second.e2 - first.e2 * second.e1;
    const float firstScale = std::max({std::abs(first.e1), std::abs(first.e2), std::abs(first.e0), 1.0f});
    const float secondScale = std::max({std::abs(second.e1), std::abs(second.e2), std::abs(second.e0), 1.0f});
    const float bound = tolerance * firstScale * secondScale;
    return std::abs(c0) <= bound && std::abs(c1) <= bound && std::abs(c2) <= bound;
}

TriangleDescription BuildTriangleDescription() noexcept
{
    const auto definition = experiment::MakeTriangleExperiment();
    return {MakeLayer(definition.nearLayer), MakeLayer(definition.farLayer)};
}

base::Result<experiment::TriangleGeometry, std::string> BuildTriangleGeometry()
{
    const auto description = BuildTriangleDescription();
    experiment::TriangleGeometry geometry;

    auto nearResult = WriteLayerGeometry(geometry, 0, description.nearLayer);
    if (!nearResult)
        return base::Result<experiment::TriangleGeometry, std::string>::Failure(nearResult.Error());
    auto farResult = WriteLayerGeometry(geometry, 3, description.farLayer);
    if (!farResult)
        return base::Result<experiment::TriangleGeometry, std::string>::Failure(farResult.Error());

    return base::Result<experiment::TriangleGeometry, std::string>::Success(std::move(geometry));
}

base::Result<semantic::SemanticGraph, std::string> BuildTriangleGraph()
{
    auto geometry = BuildTriangleGeometry();
    if (!geometry)
        return base::Result<semantic::SemanticGraph, std::string>::Failure(geometry.Error());
    return experiment::BuildCommonSemanticGraph(geometry.Value());
}
}
