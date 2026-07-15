#include "Level1Scenarios.h"

#include "../03_SemanticBuilder/SemanticBuilder.h"
#include "../20_ClassicalFrontend/ClassicalTriangle.h"
#include "../21_SdfFrontend/SdfFrontend.h"
#include "../22_PgaFrontend/PgaFrontend.h"
#include "../23_ExperimentDomain/ExperimentDomain.h"

#include <array>
#include <span>
#include <sstream>
#include <utility>

namespace sge::level1
{
namespace
{
using semantic::Effect;
using semantic::FormatMeaning;
using semantic::ProgramInterface;
using semantic::ProgramSource;
using semantic::ResourceId;
using semantic::ResourceUseId;
using semantic::SemanticBuilder;
using semantic::SemanticGraph;
using semantic::VertexInput;
using semantic::ViewRole;

constexpr std::uint64_t FrameConstantBytes = 64;
constexpr std::uint32_t FrameConstantAlignment = 16;
constexpr std::uint32_t ColorStride = 16;
constexpr std::uint32_t TextureWidth = 4;
constexpr std::uint32_t TextureHeight = 4;
constexpr std::uint32_t TextureRowBytes = TextureWidth * 4;

constexpr std::array<float, 4> InitialUploadProbe = {0.95f, 0.90f, 0.85f, 1.0f};
constexpr std::array<float, 4> InitialComputeColor = {0.72f, 0.80f, 0.96f, 1.0f};
constexpr std::array<float, 4> CopySourceColor = {0.82f, 0.95f, 0.78f, 1.0f};
constexpr std::array<float, 4> AliasPreparationColor = {0.25f, 0.20f, 0.35f, 1.0f};
constexpr std::array<float, 4> AliasedRasterColor = {0.92f, 0.78f, 1.0f, 1.0f};
constexpr std::array<std::byte, TextureRowBytes * TextureHeight> TextureBytes = {
    std::byte{0x20},std::byte{0x20},std::byte{0xff},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0x20},std::byte{0xff},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff},
    std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0xc0},std::byte{0x20},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0xc0},std::byte{0x20},std::byte{0xff},
    std::byte{0x20},std::byte{0x20},std::byte{0xff},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0x20},std::byte{0xff},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff},
    std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0xc0},std::byte{0x20},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0xc0},std::byte{0x20},std::byte{0xff}
};

struct Features final
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

Features FeaturesFor(Slice slice) noexcept
{
    const auto value = static_cast<std::uint32_t>(slice);
    Features result;
    result.dynamicData = value >= 2;
    result.extraBufferUpload = value >= 3;
    result.texture2D = value >= 4;
    result.depthAttachment = value >= 5;
    result.computeWork = value >= 6;
    result.copyWork = value >= 7;
    result.dedicatedCopyQueue = value >= 7;
    result.dedicatedComputeQueue = value >= 8;
    result.gpuWrittenFrameLocal = value >= 9;
    result.temporal = value >= 10;
    result.aliasing = value >= 11;
    result.externalResource = value >= 12;
    result.recoveryContract = value >= 13;
    return result;
}

ScenarioExpectations ExpectationsFor(Features features) noexcept
{
    return {
        features.dynamicData,
        features.extraBufferUpload,
        features.texture2D,
        features.depthAttachment,
        features.computeWork,
        features.copyWork,
        features.dedicatedComputeQueue,
        features.dedicatedCopyQueue,
        features.gpuWrittenFrameLocal,
        features.temporal,
        features.aliasing,
        features.externalResource,
        features.recoveryContract};
}

target::D3D12TargetProfile ProfileFor(Features features) noexcept
{
    target::D3D12TargetProfile profile;
    profile.computeQueueCount = features.dedicatedComputeQueue ? 1u : 0u;
    profile.copyQueueCount = features.dedicatedCopyQueue ? 1u : 0u;
    return profile;
}

std::string BuildShaderSource(Features features)
{
    std::ostringstream source;
    if (features.dynamicData)
    {
        source << R"hlsl(cbuffer FrameConstants : register(b0)
{
    float4 transformRow0;
    float4 transformRow1;
    float4 transformRow2;
    float4 transformRow3;
};

)hlsl";
    }

    std::uint32_t srvRegister = 0;
    if (features.texture2D)
    {
        source << "Texture2D<float4> MainTexture : register(t" << srvRegister++ << ");\n";
        source << "SamplerState MainSampler : register(s0);\n";
    }
    if (features.computeWork)
        source << "StructuredBuffer<float4> ComputeColorInput : register(t" << srvRegister++ << ");\n";
    if (features.copyWork)
        source << "StructuredBuffer<float4> CopiedColorInput : register(t" << srvRegister++ << ");\n";
    if (features.aliasing)
        source << "StructuredBuffer<float4> AliasedColorInput : register(t" << srvRegister++ << ");\n";
    if (features.externalResource)
        source << "StructuredBuffer<float4> ExternalColorInput : register(t" << srvRegister++ << ");\n";
    if (features.temporal)
        source << "StructuredBuffer<float4> PreviousFrameColor : register(t0);\n";
    if (features.computeWork)
        source << "RWStructuredBuffer<float4> ComputeColorOutput : register(u0);\n";

    source << R"hlsl(
struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float2 texCoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float2 texCoord : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
)hlsl";
    if (features.dynamicData)
    {
        source << R"hlsl(    const float4 sourcePosition = float4(input.position, 1.0f);
    output.position = float4(
        dot(transformRow0, sourcePosition),
        dot(transformRow1, sourcePosition),
        dot(transformRow2, sourcePosition),
        dot(transformRow3, sourcePosition));
)hlsl";
    }
    else
    {
        source << "    output.position = float4(input.position, 1.0f);\n";
    }
    source << R"hlsl(    output.color = input.color;
    output.texCoord = input.texCoord;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 result = input.color;
)hlsl";
    if (features.texture2D) source << "    result *= MainTexture.Sample(MainSampler, input.texCoord);\n";
    if (features.computeWork) source << "    result *= ComputeColorInput[0];\n";
    if (features.copyWork) source << "    result *= CopiedColorInput[0];\n";
    if (features.aliasing) source << "    result *= AliasedColorInput[0];\n";
    if (features.externalResource) source << "    result *= ExternalColorInput[0];\n";
    source << R"hlsl(    return result;
}
)hlsl";

    if (features.computeWork)
    {
        source << R"hlsl(
[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
)hlsl";
        if (features.dynamicData)
        {
            source << R"hlsl(    const float cosineMagnitude = abs(transformRow0.x);
    const float sineMagnitude = abs(transformRow0.y);
    const float4 target = float4(
        0.35f + 0.65f * cosineMagnitude,
        0.35f + 0.65f * sineMagnitude,
        0.65f + 0.35f * (1.0f - cosineMagnitude),
        1.0f);
)hlsl";
        }
        else
        {
            source << "    const float4 target = float4(0.8f, 0.8f, 1.0f, 1.0f);\n";
        }
        if (features.temporal)
            source << "    ComputeColorOutput[0] = lerp(PreviousFrameColor[0], target, 0.18f);\n";
        else
            source << "    ComputeColorOutput[0] = target;\n";
        source << "}\n";
    }
    return source.str();
}

template<class T>
base::Result<T, std::string> Fail(std::string message)
{
    return base::Result<T, std::string>::Failure(std::move(message));
}

base::Result<SemanticGraph, std::string> BuildProgressiveGraph(
    Slice slice,
    const experiment::TriangleGeometry& geometry)
{
    const auto features = FeaturesFor(slice);
    SemanticBuilder builder;

    const auto vertexBytes = std::as_bytes(std::span<const experiment::TriangleVertex>(
        geometry.vertices.data(), geometry.vertices.size()));
    auto vertexBuffer = builder.AddImmutableBuffer("Level1Vertices", sizeof(experiment::TriangleVertex), vertexBytes);
    if (!vertexBuffer) return Fail<SemanticGraph>(vertexBuffer.Error());

    ResourceId frameConstants;
    if (features.dynamicData)
    {
        auto result = builder.AddDynamicBuffer("Level1FrameConstants", FrameConstantBytes, FrameConstantAlignment);
        if (!result) return Fail<SemanticGraph>(result.Error());
        frameConstants = result.Value();
    }

    if (features.extraBufferUpload)
    {
        auto uploadProbe = builder.AddImmutableBuffer(
            "Level1BufferUploadProbe", ColorStride, std::as_bytes(std::span(InitialUploadProbe)));
        if (!uploadProbe) return Fail<SemanticGraph>(uploadProbe.Error());
    }

    ResourceId texture;
    if (features.texture2D)
    {
        auto result = builder.AddImmutableTexture2D(
            "Level1Texture", TextureWidth, TextureHeight, FormatMeaning::Bgra8Unorm,
            TextureRowBytes, TextureBytes);
        if (!result) return Fail<SemanticGraph>(result.Error());
        texture = result.Value();
    }

    ResourceId depth;
    if (features.depthAttachment)
    {
        auto result = builder.AddDepthAttachmentTexture2D("Level1Depth", FormatMeaning::Depth32Float);
        if (!result) return Fail<SemanticGraph>(result.Error());
        depth = result.Value();
    }

    ResourceId computeColor;
    if (features.computeWork)
    {
        base::Result<ResourceId, std::string> result = features.temporal ?
            builder.AddTemporalGpuWrittenBuffer(
                "Level1TemporalColor", ColorStride, std::as_bytes(std::span(InitialComputeColor))) :
            builder.AddPersistentGpuWrittenBuffer(
                "Level1PersistentComputeColor", ColorStride, ColorStride);
        if (!result) return Fail<SemanticGraph>(result.Error());
        computeColor = result.Value();
    }

    ResourceId copySource;
    ResourceId copiedColor;
    if (features.copyWork)
    {
        auto sourceResult = builder.AddImmutableBuffer(
            "Level1CopySource", ColorStride, std::as_bytes(std::span(CopySourceColor)));
        if (!sourceResult) return Fail<SemanticGraph>(sourceResult.Error());
        copySource = sourceResult.Value();
        auto destinationResult = features.gpuWrittenFrameLocal ?
            builder.AddGpuWrittenBuffer(
                "Level1FrameLocalCopyDestination", ColorStride, ColorStride) :
            builder.AddPersistentGpuWrittenBuffer(
                "Level1PersistentCopyDestination", ColorStride, ColorStride);
        if (!destinationResult) return Fail<SemanticGraph>(destinationResult.Error());
        copiedColor = destinationResult.Value();
    }

    ResourceId aliasedColor;
    if (features.aliasing)
    {
        auto preparation = builder.AddPreparationBuffer(
            "Level1AliasPreparation", ColorStride, std::as_bytes(std::span(AliasPreparationColor)));
        if (!preparation) return Fail<SemanticGraph>(preparation.Error());
        auto target = builder.AddImmutableBuffer(
            "Level1AliasedColor", ColorStride, std::as_bytes(std::span(AliasedRasterColor)));
        if (!target) return Fail<SemanticGraph>(target.Error());
        aliasedColor = target.Value();
    }

    ResourceId externalColor;
    if (features.externalResource)
    {
        auto result = builder.AddExternalBuffer("Level1ExternalColor", ColorStride, ColorStride);
        if (!result) return Fail<SemanticGraph>(result.Error());
        externalColor = result.Value();
    }

    auto surface = builder.AddPresentationSurface("Level1Surface", FormatMeaning::Bgra8Unorm);
    if (!surface) return Fail<SemanticGraph>(surface.Error());

    auto vertexUse = builder.AddUse(vertexBuffer.Value(), Effect::Read, ViewRole::VertexData);
    auto colorUse = builder.AddUse(surface.Value(), Effect::Write, ViewRole::ColorAttachment);
    auto presentUse = builder.AddUse(surface.Value(), Effect::Read, ViewRole::PresentSource);
    if (!vertexUse || !colorUse || !presentUse)
        return Fail<SemanticGraph>("failed to create the base Level-1 resource uses");

    std::vector<ResourceUseId> rasterUses{vertexUse.Value()};
    if (features.dynamicData)
    {
        auto use = builder.AddUse(frameConstants, Effect::Read, ViewRole::ConstantData);
        if (!use) return Fail<SemanticGraph>(use.Error());
        rasterUses.push_back(use.Value());
    }
    if (features.texture2D)
    {
        auto use = builder.AddUse(texture, Effect::Read, ViewRole::SampledTexture);
        if (!use) return Fail<SemanticGraph>(use.Error());
        rasterUses.push_back(use.Value());
    }
    if (features.computeWork)
    {
        auto use = builder.AddUse(computeColor, Effect::Read, ViewRole::ComputedBuffer);
        if (!use) return Fail<SemanticGraph>(use.Error());
        rasterUses.push_back(use.Value());
    }
    if (features.copyWork)
    {
        auto use = builder.AddUse(copiedColor, Effect::Read, ViewRole::CopiedBuffer);
        if (!use) return Fail<SemanticGraph>(use.Error());
        rasterUses.push_back(use.Value());
    }
    if (features.aliasing)
    {
        auto use = builder.AddUse(aliasedColor, Effect::Read, ViewRole::AliasedBuffer);
        if (!use) return Fail<SemanticGraph>(use.Error());
        rasterUses.push_back(use.Value());
    }
    if (features.externalResource)
    {
        auto use = builder.AddUse(externalColor, Effect::Read, ViewRole::ExternalBuffer);
        if (!use) return Fail<SemanticGraph>(use.Error());
        rasterUses.push_back(use.Value());
    }
    rasterUses.push_back(colorUse.Value());
    if (features.depthAttachment)
    {
        auto use = builder.AddUse(depth, Effect::Write, ViewRole::DepthAttachment);
        if (!use) return Fail<SemanticGraph>(use.Error());
        rasterUses.push_back(use.Value());
    }
    rasterUses.push_back(presentUse.Value());

    ProgramInterface rasterInterface;
    rasterInterface.vertexStrideBytes = sizeof(experiment::TriangleVertex);
    rasterInterface.vertexInputs.push_back({VertexInput::Meaning::Position, 3, 0});
    rasterInterface.vertexInputs.push_back({VertexInput::Meaning::Color, 4, 12});
    rasterInterface.vertexInputs.push_back({VertexInput::Meaning::TexCoord, 2, 28});
    if (features.dynamicData)
    {
        rasterInterface.constantDataBytes = FrameConstantBytes;
        rasterInterface.constantDataAlignment = FrameConstantAlignment;
    }
    rasterInterface.sampledTextureCount = features.texture2D ? 1u : 0u;
    rasterInterface.sampledBufferCount =
        (features.computeWork ? 1u : 0u) +
        (features.copyWork ? 1u : 0u) +
        (features.aliasing ? 1u : 0u) +
        (features.externalResource ? 1u : 0u);

    const auto shaderSource = BuildShaderSource(features);
    auto rasterProgram = builder.AddRasterProgram(
        "Level1RasterProgram", std::move(rasterInterface),
        ProgramSource{shaderSource, "VSMain", "PSMain", {}});
    if (!rasterProgram) return Fail<SemanticGraph>(rasterProgram.Error());

    // Deliberately declare the final raster Work first. The general analysis must
    // derive Compute/Copy -> Raster from ResourceUse hazards rather than declaration order.
    auto rasterWork = builder.AddRasterWorkGeneric(
        "Level1RasterWork", rasterProgram.Value(), rasterUses,
        static_cast<std::uint32_t>(geometry.vertices.size()));
    if (!rasterWork) return Fail<SemanticGraph>(rasterWork.Error());

    if (features.computeWork)
    {
        std::vector<ResourceUseId> computeUses;
        if (features.dynamicData)
        {
            auto use = builder.AddUse(frameConstants, Effect::Read, ViewRole::ConstantData);
            if (!use) return Fail<SemanticGraph>(use.Error());
            computeUses.push_back(use.Value());
        }
        if (features.temporal)
        {
            auto previous = builder.AddUse(computeColor, Effect::Read, ViewRole::TemporalPreviousBuffer);
            if (!previous) return Fail<SemanticGraph>(previous.Error());
            computeUses.push_back(previous.Value());
        }
        auto output = builder.AddUse(computeColor, Effect::Write, ViewRole::StorageBuffer);
        if (!output) return Fail<SemanticGraph>(output.Error());
        computeUses.push_back(output.Value());

        ProgramInterface computeInterface;
        if (features.dynamicData)
        {
            computeInterface.constantDataBytes = FrameConstantBytes;
            computeInterface.constantDataAlignment = FrameConstantAlignment;
        }
        computeInterface.sampledBufferCount = features.temporal ? 1u : 0u;
        computeInterface.unorderedBufferCount = 1;
        auto computeProgram = builder.AddComputeProgram(
            "Level1ComputeProgram", std::move(computeInterface),
            ProgramSource{shaderSource, {}, {}, "CSMain"});
        if (!computeProgram) return Fail<SemanticGraph>(computeProgram.Error());
        auto computeWork = builder.AddComputeWorkGeneric(
            "Level1ComputeWork", computeProgram.Value(), computeUses, 1, 1, 1);
        if (!computeWork) return Fail<SemanticGraph>(computeWork.Error());
    }

    if (features.copyWork)
    {
        auto sourceUse = builder.AddUse(copySource, Effect::Read, ViewRole::CopySource);
        auto destinationUse = builder.AddUse(copiedColor, Effect::Write, ViewRole::CopyDestination);
        if (!sourceUse || !destinationUse)
            return Fail<SemanticGraph>("failed to create Level-1 Copy resource uses");
        auto copyWork = builder.AddCopyWork(
            "Level1CopyWork", sourceUse.Value(), destinationUse.Value(), ColorStride);
        if (!copyWork) return Fail<SemanticGraph>(copyWork.Error());
    }

    return base::Result<SemanticGraph, std::string>::Success(std::move(builder).Build());
}

base::Result<SemanticGraph, std::string> BuildFrontendGraph(ScenarioKey key)
{
    if (key.frontend == Frontend::Classical) return classical::BuildTriangleGraph();
    if (key.frontend == Frontend::Sdf) return sdf::BuildTriangleGraph();
    if (key.frontend == Frontend::Pga) return pga::BuildTriangleGraph();
    return Fail<SemanticGraph>("a frontend-backed Slice requires Classical, SDF, or PGA");
}
}

std::vector<ScenarioKey> StageAInputs()
{
    std::vector<ScenarioKey> result;
    for (std::uint32_t value = 1; value <= 13; ++value)
        result.push_back({static_cast<Slice>(value), Frontend::Neutral});
    result.push_back({Slice::Slice14ClassicalSdf, Frontend::Classical});
    result.push_back({Slice::Slice14ClassicalSdf, Frontend::Sdf});
    result.push_back({Slice::Slice15PgaFrontend, Frontend::Classical});
    result.push_back({Slice::Slice15PgaFrontend, Frontend::Sdf});
    result.push_back({Slice::Slice15PgaFrontend, Frontend::Pga});
    return result;
}

const char* SliceName(Slice slice) noexcept
{
    switch (slice)
    {
    case Slice::Slice01FixedTriangle: return "Slice 1 Fixed Triangle";
    case Slice::Slice02DynamicData: return "Slice 2 Dynamic constant slot / FrameInvocation";
    case Slice::Slice03BufferUpload: return "Slice 3 Buffer initial upload + Readback";
    case Slice::Slice04Texture2D: return "Slice 4 Texture2D + Texture Readback";
    case Slice::Slice05DepthAttachment: return "Slice 5 Depth attachment";
    case Slice::Slice06ComputeWork: return "Slice 6 Compute Work";
    case Slice::Slice07CopyWork: return "Slice 7 Copy Work / Copy queue";
    case Slice::Slice08MultipleQueues: return "Slice 8 Multiple queues / handoff";
    case Slice::Slice09FrameLocal: return "Slice 9 FrameLocal resource";
    case Slice::Slice10Temporal: return "Slice 10 Temporal resource";
    case Slice::Slice11Aliasing: return "Slice 11 Aliasing";
    case Slice::Slice12ExternalResource: return "Slice 12 External resource Acquire / Release";
    case Slice::Slice13DeviceRecovery: return "Slice 13 Device loss and reconstruction";
    case Slice::Slice14ClassicalSdf: return "Slice 14 Classical / SDF common experiment";
    case Slice::Slice15PgaFrontend: return "Slice 15 PGA direct frontend";
    default: return "Unknown Slice";
    }
}

const char* FrontendName(Frontend frontend) noexcept
{
    switch (frontend)
    {
    case Frontend::Neutral: return "Neutral";
    case Frontend::Classical: return "Classical";
    case Frontend::Sdf: return "SDF";
    case Frontend::Pga: return "PGA";
    default: return "Unknown";
    }
}

base::Result<ScenarioInput, std::string> BuildScenario(ScenarioKey key)
{
    const auto sliceValue = static_cast<std::uint32_t>(key.slice);
    if (sliceValue < 1 || sliceValue > 15)
        return Fail<ScenarioInput>("Level-1 scenario slice is outside 1..15");

    if (sliceValue <= 13 && key.frontend != Frontend::Neutral)
        return Fail<ScenarioInput>("Slices 1..13 use the neutral Level-1 scenario frontend");
    if (key.slice == Slice::Slice14ClassicalSdf &&
        key.frontend != Frontend::Classical && key.frontend != Frontend::Sdf)
        return Fail<ScenarioInput>("Slice 14 accepts only Classical or SDF");
    if (key.slice == Slice::Slice15PgaFrontend && key.frontend == Frontend::Neutral)
        return Fail<ScenarioInput>("Slice 15 requires a concrete frontend");

    const auto features = FeaturesFor(key.slice);
    base::Result<SemanticGraph, std::string> graph = sliceValue <= 13 ?
        BuildProgressiveGraph(key.slice, classical::BuildTriangleGeometry()) :
        BuildFrontendGraph(key);
    if (!graph) return Fail<ScenarioInput>(graph.Error());

    ScenarioInput input;
    input.key = key;
    input.name = std::string(SliceName(key.slice)) + " [" + FrontendName(key.frontend) + "]";
    input.graph = std::move(graph).Value();
    input.targetProfile = ProfileFor(features);
    input.expectations = ExpectationsFor(features);
    return base::Result<ScenarioInput, std::string>::Success(std::move(input));
}
}
