#pragma once

#include "../00_Base/Result.h"
#include "../02_SemanticModel/SemanticModel.h"
#include "../05_TargetModel/TargetModel.h"

#include <array>
#include <cstdint>
#include <string>

namespace sge::level2::runtime_scenarios
{
using Float4 = std::array<float, 4>;

enum class Scenario : std::uint8_t
{
    DynamicExternalPipeline = 0,
    CrossQueueTemporal = 1
};

struct Input final
{
    Scenario scenario = Scenario::DynamicExternalPipeline;
    std::string name;
    semantic::SemanticGraph graph;
    target::D3D12TargetProfile targetProfile;
    std::uint32_t dynamicSlotCount = 0;
    std::uint32_t externalSlotCount = 0;
};

[[nodiscard]] base::Result<Input, std::string> Build(Scenario scenario);
[[nodiscard]] Float4 PipelineExpected(const Float4& dynamicA,
                                      const Float4& dynamicB,
                                      const Float4& externalInput) noexcept;
[[nodiscard]] const Float4& TemporalSeed() noexcept;
}
