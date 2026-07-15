#include "SdfFrontend.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sge::sdf
{
namespace
{
constexpr std::size_t BaseEdge = 0;
constexpr std::size_t RightEdge = 1;
constexpr std::size_t LeftEdge = 2;

float CanonicalZero(float value) noexcept
{
    return value == 0.0f ? 0.0f : value;
}

TriangleField MakeField(const experiment::IsoscelesTriangleLayer& layer) noexcept
{
    const float rise = layer.apexY - layer.baseY;

    TriangleField result;
    // base: y >= baseY
    result.halfSpaces[BaseEdge] = {0.0f, -1.0f, layer.baseY};
    // right edge: rise * (x - centerX) + halfWidth * (y - apexY) <= 0
    result.halfSpaces[RightEdge] = {
        rise,
        layer.halfWidth,
        -rise * layer.centerX - layer.halfWidth * layer.apexY};
    // left edge: -rise * (x - centerX) + halfWidth * (y - apexY) <= 0
    result.halfSpaces[LeftEdge] = {
        -rise,
        layer.halfWidth,
        rise * layer.centerX - layer.halfWidth * layer.apexY};
    result.depth = layer.depth;
    result.color = layer.color;
    return result;
}

base::Result<math::Float2, std::string> Intersect(
    const HalfSpace2D& first,
    const HalfSpace2D& second)
{
    const float determinant = first.a * second.b - second.a * first.b;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 0.000001f)
        return base::Result<math::Float2, std::string>::Failure("SDF half-space boundaries do not have a stable intersection");

    const float x = (first.b * second.c - second.b * first.c) / determinant;
    const float y = (first.c * second.a - second.c * first.a) / determinant;
    if (!std::isfinite(x) || !std::isfinite(y))
        return base::Result<math::Float2, std::string>::Failure("SDF boundary intersection is non-finite");

    return base::Result<math::Float2, std::string>::Success({CanonicalZero(x), CanonicalZero(y)});
}

experiment::TriangleVertex MakeVertex(
    math::Float2 point,
    const TriangleField& field,
    float u,
    float v) noexcept
{
    return {{point.x, point.y, field.depth},
            {field.color.x, field.color.y, field.color.z, field.color.w},
            {u, v}};
}

base::Result<void, std::string> WriteFieldGeometry(
    experiment::TriangleGeometry& geometry,
    std::size_t firstVertex,
    const TriangleField& field)
{
    const auto apex = Intersect(field.halfSpaces[LeftEdge], field.halfSpaces[RightEdge]);
    if (!apex) return base::Result<void, std::string>::Failure(apex.Error());
    const auto rightBase = Intersect(field.halfSpaces[RightEdge], field.halfSpaces[BaseEdge]);
    if (!rightBase) return base::Result<void, std::string>::Failure(rightBase.Error());
    const auto leftBase = Intersect(field.halfSpaces[BaseEdge], field.halfSpaces[LeftEdge]);
    if (!leftBase) return base::Result<void, std::string>::Failure(leftBase.Error());

    for (const auto point : {apex.Value(), rightBase.Value(), leftBase.Value()})
        if (EvaluateSignedDistance(field, point) > 0.000001f)
            return base::Result<void, std::string>::Failure("an SDF boundary intersection lies outside the triangle field");

    geometry.vertices[firstVertex + 0] = MakeVertex(apex.Value(), field, 0.5f, 0.0f);
    geometry.vertices[firstVertex + 1] = MakeVertex(rightBase.Value(), field, 1.0f, 1.0f);
    geometry.vertices[firstVertex + 2] = MakeVertex(leftBase.Value(), field, 0.0f, 1.0f);

    const math::Float2 centroid{
        (apex.Value().x + rightBase.Value().x + leftBase.Value().x) / 3.0f,
        (apex.Value().y + rightBase.Value().y + leftBase.Value().y) / 3.0f};
    if (EvaluateSignedDistance(field, centroid) > 0.000001f)
        return base::Result<void, std::string>::Failure("reconstructed SDF triangle centroid lies outside its field");

    return base::Result<void, std::string>::Success();
}
}

SdfTriangleDescription BuildTriangleDescription() noexcept
{
    const auto definition = experiment::MakeTriangleExperiment();
    return {MakeField(definition.nearLayer), MakeField(definition.farLayer)};
}

float EvaluateSignedDistance(const TriangleField& field, math::Float2 point) noexcept
{
    float result = -std::numeric_limits<float>::infinity();
    for (const auto& halfSpace : field.halfSpaces)
    {
        const float length = std::sqrt(halfSpace.a * halfSpace.a + halfSpace.b * halfSpace.b);
        if (length <= 0.0f || !std::isfinite(length)) return std::numeric_limits<float>::infinity();
        const float distance = (halfSpace.a * point.x + halfSpace.b * point.y + halfSpace.c) / length;
        result = std::max(result, distance);
    }
    return result;
}

base::Result<experiment::TriangleGeometry, std::string> BuildTriangleGeometry()
{
    const auto description = BuildTriangleDescription();
    experiment::TriangleGeometry geometry;

    auto nearResult = WriteFieldGeometry(geometry, 0, description.nearField);
    if (!nearResult)
        return base::Result<experiment::TriangleGeometry, std::string>::Failure(nearResult.Error());
    auto farResult = WriteFieldGeometry(geometry, 3, description.farField);
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
