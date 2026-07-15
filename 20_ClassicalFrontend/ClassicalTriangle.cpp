#include "ClassicalTriangle.h"

namespace sge::classical
{
namespace
{
experiment::TriangleVertex MakeVertex(
    float x,
    float y,
    const experiment::IsoscelesTriangleLayer& layer,
    float u,
    float v) noexcept
{
    return {{x, y, layer.depth},
            {layer.color.x, layer.color.y, layer.color.z, layer.color.w},
            {u, v}};
}

void WriteLayer(
    experiment::TriangleGeometry& geometry,
    std::size_t firstVertex,
    const experiment::IsoscelesTriangleLayer& layer) noexcept
{
    // Classical representation: explicit triangle vertices in draw order.
    geometry.vertices[firstVertex + 0] = MakeVertex(layer.centerX, layer.apexY, layer, 0.5f, 0.0f);
    geometry.vertices[firstVertex + 1] = MakeVertex(layer.centerX + layer.halfWidth, layer.baseY, layer, 1.0f, 1.0f);
    geometry.vertices[firstVertex + 2] = MakeVertex(layer.centerX - layer.halfWidth, layer.baseY, layer, 0.0f, 1.0f);
}
}

experiment::TriangleGeometry BuildTriangleGeometry() noexcept
{
    const auto definition = experiment::MakeTriangleExperiment();
    experiment::TriangleGeometry geometry;
    WriteLayer(geometry, 0, definition.nearLayer);
    WriteLayer(geometry, 3, definition.farLayer);
    return geometry;
}

base::Result<semantic::SemanticGraph, std::string> BuildTriangleGraph()
{
    return experiment::BuildCommonSemanticGraph(BuildTriangleGeometry());
}
}
