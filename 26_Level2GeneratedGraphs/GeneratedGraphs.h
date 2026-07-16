#pragma once

#include "../00_Base/Result.h"
#include "../02_SemanticModel/SemanticModel.h"
#include "../05_TargetModel/TargetModel.h"

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace sge::level2::generated
{
enum class NodeKind : std::uint16_t
{
    Compute = 1,
    Copy = 2
};

struct Edge final
{
    std::uint32_t producerNode = 0;
    std::uint32_t consumerNode = 0;
    auto operator<=>(const Edge&) const = default;
};

struct GeneratedSpec final
{
    std::string name;
    std::vector<NodeKind> kinds;
    std::vector<Edge> edges;
    std::uint64_t seed = 0;
    bool permuteStorage = true;
    bool dedicatedComputeQueue = true;
    bool dedicatedCopyQueue = true;
};

struct WorkEdge final
{
    semantic::WorkId producer;
    semantic::WorkId consumer;
    auto operator<=>(const WorkEdge&) const = default;
};

struct ExpectedWait final
{
    std::uint32_t consumerPosition = 0;
    std::uint32_t consumerQueue = 0;
    std::uint32_t producerQueue = 0;
    std::uint32_t producerSignalPoint = 0;
    auto operator<=>(const ExpectedWait&) const = default;
};

struct Oracle final
{
    std::vector<semantic::WorkId> canonicalWorkOrder;
    std::vector<WorkEdge> reducedEdges;
    std::vector<std::uint32_t> queueByPosition;
    std::vector<ExpectedWait> waits;
    std::uint32_t computeWorkCount = 0;
    std::uint32_t copyWorkCount = 0;
};

struct GeneratedCase final
{
    GeneratedSpec spec;
    semantic::SemanticGraph graph;
    target::D3D12TargetProfile targetProfile;
    std::vector<semantic::WorkId> workIdByNode;
    Oracle oracle;
};

[[nodiscard]] base::Result<GeneratedCase, std::string> BuildGeneratedCase(
    GeneratedSpec spec);

[[nodiscard]] GeneratedSpec MakeForwardMaskSpec(
    std::string name,
    std::uint32_t nodeCount,
    std::uint64_t edgeMask,
    std::uint64_t seed,
    bool mixedKinds);

[[nodiscard]] GeneratedSpec MakeSeededSpec(
    std::string name,
    std::uint32_t nodeCount,
    std::uint64_t seed,
    std::uint32_t edgePercent,
    bool mixedKinds);

[[nodiscard]] semantic::SemanticGraph PermuteStorage(
    semantic::SemanticGraph graph,
    std::uint64_t seed);
}
