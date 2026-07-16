#pragma once

#include "../00_Base/Result.h"
#include "../02_SemanticModel/SemanticModel.h"
#include "../05_TargetModel/TargetModel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sge::level2
{
enum class StageHScenario : std::uint16_t
{
    ComputeCopyZigZag = 1,
    ComputeDiamond = 2,
    TwoPassRaster = 3
};

struct ExpectedExecution final
{
    semantic::WorkKind kind = semantic::WorkKind::Compute;
    std::uint32_t queue = 0;
};

struct StageHExpectations final
{
    std::vector<semantic::WorkId> canonicalWorkOrder;
    std::vector<ExpectedExecution> executionOrder;
    std::uint32_t resourceCount = 0;
    std::uint32_t resourceUseCount = 0;
    std::uint32_t sourceProgramCount = 0;
    std::uint32_t shaderCount = 0;
    std::uint32_t waitQueueCount = 0;
    bool surface = false;
    bool directCompletion = false;
    bool computeCompletion = false;
    bool copyCompletion = false;
};

struct StageHInput final
{
    StageHScenario scenario = StageHScenario::ComputeCopyZigZag;
    std::string name;
    semantic::SemanticGraph graph;
    target::D3D12TargetProfile targetProfile;
    StageHExpectations expectations;
};

[[nodiscard]] std::vector<StageHScenario> StageHInputs();
[[nodiscard]] base::Result<StageHInput, std::string> BuildStageHScenario(StageHScenario scenario);
}
