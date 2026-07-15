#pragma once

#include "../00_Base/Result.h"
#include "../02_SemanticModel/SemanticModel.h"
#include "../05_TargetModel/TargetModel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sge::level1
{
enum class Slice : std::uint32_t
{
    Slice01FixedTriangle = 1,
    Slice02DynamicData = 2,
    Slice03BufferUpload = 3,
    Slice04Texture2D = 4,
    Slice05DepthAttachment = 5,
    Slice06ComputeWork = 6,
    Slice07CopyWork = 7,
    Slice08MultipleQueues = 8,
    Slice09FrameLocal = 9,
    Slice10Temporal = 10,
    Slice11Aliasing = 11,
    Slice12ExternalResource = 12,
    Slice13DeviceRecovery = 13,
    Slice14ClassicalSdf = 14,
    Slice15PgaFrontend = 15
};

enum class Frontend : std::uint16_t
{
    Neutral = 1,
    Classical = 2,
    Sdf = 3,
    Pga = 4
};

struct ScenarioKey final
{
    Slice slice = Slice::Slice01FixedTriangle;
    Frontend frontend = Frontend::Neutral;
    auto operator<=>(const ScenarioKey&) const = default;
};

struct ScenarioExpectations final
{
    bool dynamicData = false;
    bool extraBufferUpload = false;
    bool texture2D = false;
    bool depthAttachment = false;
    bool computeWork = false;
    bool copyWork = false;
    bool dedicatedComputeQueue = false;
    bool dedicatedCopyQueue = false;
    bool gpuWrittenFrameLocal = false;
    bool temporal = false;
    bool aliasing = false;
    bool externalResource = false;
    bool recoveryContract = false;
};

struct ScenarioInput final
{
    ScenarioKey key;
    std::string name;
    semantic::SemanticGraph graph;
    target::D3D12TargetProfile targetProfile;
    ScenarioExpectations expectations;
};

[[nodiscard]] std::vector<ScenarioKey> StageAInputs();
[[nodiscard]] const char* SliceName(Slice slice) noexcept;
[[nodiscard]] const char* FrontendName(Frontend frontend) noexcept;
[[nodiscard]] base::Result<ScenarioInput, std::string> BuildScenario(ScenarioKey key);
}
