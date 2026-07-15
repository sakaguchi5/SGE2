#pragma once

#include "../00_Base/Result.h"
#include "../01_MathVocabulary/MathTypes.h"
#include "../02_SemanticModel/SemanticModel.h"

#include <array>
#include <cstddef>
#include <string>

namespace sge::experiment
{
struct IsoscelesTriangleLayer final
{
    float centerX = 0.0f;
    float apexY = 0.5f;
    float baseY = -0.5f;
    float halfWidth = 0.5f;
    float depth = 0.25f;
    math::Float4 color{};
};

struct TriangleExperiment final
{
    math::ExperimentTarget target{960, 720};
    IsoscelesTriangleLayer nearLayer;
    IsoscelesTriangleLayer farLayer;
};

struct TriangleVertex final
{
    float position[3]{};
    float color[4]{};
    float texCoord[2]{};
};

struct TriangleGeometry final
{
    std::array<TriangleVertex, 6> vertices{};
};

static_assert(sizeof(TriangleVertex) == 36, "The common experiment vertex ABI must remain stable.");
static_assert(sizeof(TriangleGeometry) == 216, "The common experiment geometry must not contain padding.");

[[nodiscard]] TriangleExperiment MakeTriangleExperiment() noexcept;

[[nodiscard]] bool GeometryBitwiseEqual(
    const TriangleGeometry& left,
    const TriangleGeometry& right) noexcept;

[[nodiscard]] base::Result<semantic::SemanticGraph, std::string> BuildCommonSemanticGraph(
    const TriangleGeometry& geometry);
}
