#pragma once

#include "../00_Base/Result.h"
#include "../02_SemanticModel/SemanticModel.h"

#include <string>
#include <vector>

namespace sge::analysis
{
struct Diagnostic final
{
    std::string message;
    std::uint32_t sourceIndex = base::InvalidIndex;
};

enum class DependencyKind : std::uint16_t
{
    Explicit = 1,
    ReadAfterWrite = 2,
    WriteAfterRead = 3,
    WriteAfterWrite = 4,
    Present = 5
};

struct DependencyEdge final
{
    semantic::WorkId producer;
    semantic::WorkId consumer;
    semantic::ResourceId resource;
    DependencyKind kind = DependencyKind::Explicit;
};

struct ResourceLifetime final
{
    semantic::ResourceId resource;
    std::uint32_t firstUse = base::InvalidIndex;
    std::uint32_t lastUse = base::InvalidIndex;
    bool usedByWork = false;
};

struct AnalyzedGraph final
{
    const semantic::SemanticGraph* source = nullptr;
    std::vector<semantic::ResourceId> canonicalResourceOrder;
    std::vector<semantic::ResourceUseId> canonicalResourceUseOrder;
    std::vector<semantic::ProgramId> canonicalProgramOrder;
    std::vector<semantic::WorkId> canonicalWorkOrder;
    std::vector<DependencyEdge> dependencies;
    std::vector<ResourceLifetime> resourceLifetimes;
};

[[nodiscard]] base::Result<AnalyzedGraph, std::vector<Diagnostic>> Analyze(
    const semantic::SemanticGraph& graph);
}
