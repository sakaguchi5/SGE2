#include "ExperimentDomain.h"

#include "../03_SemanticBuilder/SemanticBuilder.h"

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

namespace sge::experiment
{
namespace
{
constexpr std::uint32_t TextureWidth = 4;
constexpr std::uint32_t TextureHeight = 4;
constexpr std::uint32_t TextureRowBytes = TextureWidth * 4;
constexpr std::array<std::byte, TextureRowBytes * TextureHeight> TextureBytes = {
    std::byte{0x20},std::byte{0x20},std::byte{0xff},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0x20},std::byte{0xff},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff},
    std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0xc0},std::byte{0x20},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0xc0},std::byte{0x20},std::byte{0xff},
    std::byte{0x20},std::byte{0x20},std::byte{0xff},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0x20},std::byte{0xff},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff},
    std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0xc0},std::byte{0x20},std::byte{0xff}, std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}, std::byte{0x20},std::byte{0xc0},std::byte{0x20},std::byte{0xff}
};

constexpr std::uint64_t FrameConstantBytes = 64;
constexpr std::uint32_t FrameConstantAlignment = 16;
constexpr std::uint32_t ComputeColorStride = 16;
constexpr std::array<float, 4> InitialTemporalColor = {0.55f, 0.55f, 0.75f, 1.0f};
constexpr std::array<float, 4> CopyColorSource = {0.82f, 0.95f, 0.78f, 1.0f};
constexpr std::array<float, 4> AliasWarmupColor = {0.25f, 0.20f, 0.35f, 1.0f};
constexpr std::array<float, 4> AliasedRasterColor = {0.92f, 0.78f, 1.0f, 1.0f};
constexpr std::uint64_t CopyColorBytes = sizeof(CopyColorSource);
constexpr std::uint32_t CopyColorStride = sizeof(CopyColorSource);

constexpr const char* ShaderSource = R"hlsl(
cbuffer FrameConstants : register(b0)
{
    float4 transformRow0;
    float4 transformRow1;
    float4 transformRow2;
    float4 transformRow3;
};

StructuredBuffer<float4> PreviousFrameColor : register(t0);
RWStructuredBuffer<float4> ComputeColorOutput : register(u0);
Texture2D<float4> MainTexture : register(t0);
StructuredBuffer<float4> ComputeColorInput : register(t1);
StructuredBuffer<float4> CopiedColorInput : register(t2);
StructuredBuffer<float4> AliasedColorInput : register(t3);
StructuredBuffer<float4> ExternalColorInput : register(t4);
SamplerState MainSampler : register(s0);

[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const float cosineMagnitude = abs(transformRow0.x);
    const float sineMagnitude = abs(transformRow0.y);
    const float4 target = float4(
        0.35f + 0.65f * cosineMagnitude,
        0.35f + 0.65f * sineMagnitude,
        0.65f + 0.35f * (1.0f - cosineMagnitude),
        1.0f);
    ComputeColorOutput[0] = lerp(PreviousFrameColor[0], target, 0.18f);
}

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
    const float4 sourcePosition = float4(input.position, 1.0f);
    PSInput output;
    output.position = float4(
        dot(transformRow0, sourcePosition),
        dot(transformRow1, sourcePosition),
        dot(transformRow2, sourcePosition),
        dot(transformRow3, sourcePosition));
    output.color = input.color;
    output.texCoord = input.texCoord;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return MainTexture.Sample(MainSampler, input.texCoord) * input.color * ComputeColorInput[0] * CopiedColorInput[0] * AliasedColorInput[0] * ExternalColorInput[0];
}
)hlsl";

bool IsFinite(const TriangleVertex& vertex) noexcept
{
    for (float value : vertex.position) if (!std::isfinite(value)) return false;
    for (float value : vertex.color) if (!std::isfinite(value)) return false;
    for (float value : vertex.texCoord) if (!std::isfinite(value)) return false;
    return true;
}

float SignedTwiceArea(
    const TriangleVertex& a,
    const TriangleVertex& b,
    const TriangleVertex& c) noexcept
{
    return (b.position[0] - a.position[0]) * (c.position[1] - a.position[1]) -
           (b.position[1] - a.position[1]) * (c.position[0] - a.position[0]);
}

base::Result<void, std::string> ValidateGeometry(const TriangleGeometry& geometry)
{
    for (const auto& vertex : geometry.vertices)
        if (!IsFinite(vertex))
            return base::Result<void, std::string>::Failure("common experiment geometry contains a non-finite vertex component");

    if (std::abs(SignedTwiceArea(geometry.vertices[0], geometry.vertices[1], geometry.vertices[2])) <= 0.000001f ||
        std::abs(SignedTwiceArea(geometry.vertices[3], geometry.vertices[4], geometry.vertices[5])) <= 0.000001f)
        return base::Result<void, std::string>::Failure("common experiment contains a degenerate triangle");

    return base::Result<void, std::string>::Success();
}
}

TriangleExperiment MakeTriangleExperiment() noexcept
{
    TriangleExperiment result;
    result.target = {960, 720};
    result.nearLayer = {
        0.0f, 0.5f, -0.5f, 0.5f, 0.25f,
        {1.0f, 0.35f, 0.22f, 1.0f}};
    result.farLayer = {
        0.0f, 0.75f, -0.75f, 0.75f, 0.75f,
        {0.22f, 0.45f, 1.0f, 1.0f}};
    return result;
}

bool GeometryBitwiseEqual(
    const TriangleGeometry& left,
    const TriangleGeometry& right) noexcept
{
    return std::memcmp(&left, &right, sizeof(TriangleGeometry)) == 0;
}

base::Result<semantic::SemanticGraph, std::string> BuildCommonSemanticGraph(
    const TriangleGeometry& geometry)
{
    const auto validated = ValidateGeometry(geometry);
    if (!validated)
        return base::Result<semantic::SemanticGraph, std::string>::Failure(validated.Error());

    semantic::SemanticBuilder builder;
    const auto vertexBytes = std::as_bytes(std::span<const TriangleVertex>(
        geometry.vertices.data(), geometry.vertices.size()));

    auto vertexBuffer = builder.AddImmutableBuffer("CommonExperimentVertices", sizeof(TriangleVertex), vertexBytes);
    if (!vertexBuffer) return base::Result<semantic::SemanticGraph, std::string>::Failure(vertexBuffer.Error());
    auto frameConstants = builder.AddDynamicBuffer("CommonExperimentFrameConstants", FrameConstantBytes, FrameConstantAlignment);
    if (!frameConstants) return base::Result<semantic::SemanticGraph, std::string>::Failure(frameConstants.Error());
    auto texture = builder.AddImmutableTexture2D("CommonExperimentTexture", TextureWidth, TextureHeight, semantic::FormatMeaning::Bgra8Unorm, TextureRowBytes, TextureBytes);
    if (!texture) return base::Result<semantic::SemanticGraph, std::string>::Failure(texture.Error());
    auto depth = builder.AddDepthAttachmentTexture2D("MainDepthAttachment", semantic::FormatMeaning::Depth32Float);
    if (!depth) return base::Result<semantic::SemanticGraph, std::string>::Failure(depth.Error());
    auto computeColor = builder.AddTemporalGpuWrittenBuffer("TemporalColorHistory", ComputeColorStride, std::as_bytes(std::span(InitialTemporalColor)));
    if (!computeColor) return base::Result<semantic::SemanticGraph, std::string>::Failure(computeColor.Error());
    auto copyColorSource = builder.AddImmutableBuffer("CopyColorSource", CopyColorStride, std::as_bytes(std::span(CopyColorSource)));
    if (!copyColorSource) return base::Result<semantic::SemanticGraph, std::string>::Failure(copyColorSource.Error());
    auto copiedColor = builder.AddGpuWrittenBuffer("CopiedColor", CopyColorBytes, CopyColorStride);
    if (!copiedColor) return base::Result<semantic::SemanticGraph, std::string>::Failure(copiedColor.Error());
    auto surface = builder.AddPresentationSurface("MainPresentationSurface", semantic::FormatMeaning::Bgra8Unorm);
    if (!surface) return base::Result<semantic::SemanticGraph, std::string>::Failure(surface.Error());
    auto aliasWarmup = builder.AddPreparationBuffer("AliasWarmup", sizeof(AliasWarmupColor), std::as_bytes(std::span(AliasWarmupColor)));
    if (!aliasWarmup) return base::Result<semantic::SemanticGraph, std::string>::Failure(aliasWarmup.Error());
    auto aliasedColor = builder.AddImmutableBuffer("AliasedRasterColor", sizeof(AliasedRasterColor), std::as_bytes(std::span(AliasedRasterColor)));
    if (!aliasedColor) return base::Result<semantic::SemanticGraph, std::string>::Failure(aliasedColor.Error());
    auto externalColor = builder.AddExternalBuffer("ExternalFrameColor", 16, 16);
    if (!externalColor) return base::Result<semantic::SemanticGraph, std::string>::Failure(externalColor.Error());

    auto vertexUse = builder.AddUse(vertexBuffer.Value(), semantic::Effect::Read, semantic::ViewRole::VertexData);
    auto rasterConstantUse = builder.AddUse(frameConstants.Value(), semantic::Effect::Read, semantic::ViewRole::ConstantData);
    auto textureUse = builder.AddUse(texture.Value(), semantic::Effect::Read, semantic::ViewRole::SampledTexture);
    auto computedReadUse = builder.AddUse(computeColor.Value(), semantic::Effect::Read, semantic::ViewRole::ComputedBuffer);
    auto copiedReadUse = builder.AddUse(copiedColor.Value(), semantic::Effect::Read, semantic::ViewRole::CopiedBuffer);
    auto aliasedReadUse = builder.AddUse(aliasedColor.Value(), semantic::Effect::Read, semantic::ViewRole::AliasedBuffer);
    auto externalReadUse = builder.AddUse(externalColor.Value(), semantic::Effect::Read, semantic::ViewRole::ExternalBuffer);
    auto colorUse = builder.AddUse(surface.Value(), semantic::Effect::Write, semantic::ViewRole::ColorAttachment);
    auto depthUse = builder.AddUse(depth.Value(), semantic::Effect::Write, semantic::ViewRole::DepthAttachment);
    auto presentUse = builder.AddUse(surface.Value(), semantic::Effect::Read, semantic::ViewRole::PresentSource);
    auto computeConstantUse = builder.AddUse(frameConstants.Value(), semantic::Effect::Read, semantic::ViewRole::ConstantData);
    auto temporalPreviousUse = builder.AddUse(computeColor.Value(), semantic::Effect::Read, semantic::ViewRole::TemporalPreviousBuffer);
    auto computeOutputUse = builder.AddUse(computeColor.Value(), semantic::Effect::Write, semantic::ViewRole::StorageBuffer);
    auto copySourceUse = builder.AddUse(copyColorSource.Value(), semantic::Effect::Read, semantic::ViewRole::CopySource);
    auto copyDestinationUse = builder.AddUse(copiedColor.Value(), semantic::Effect::Write, semantic::ViewRole::CopyDestination);
    if (!vertexUse || !rasterConstantUse || !textureUse || !computedReadUse || !copiedReadUse || !aliasedReadUse || !externalReadUse || !colorUse || !depthUse || !presentUse || !computeConstantUse || !temporalPreviousUse || !computeOutputUse || !copySourceUse || !copyDestinationUse)
        return base::Result<semantic::SemanticGraph, std::string>::Failure("failed to create common experiment resource uses");

    semantic::ProgramInterface rasterInterface;
    rasterInterface.vertexStrideBytes = sizeof(TriangleVertex);
    rasterInterface.vertexInputs.push_back({semantic::VertexInput::Meaning::Position, 3, 0});
    rasterInterface.vertexInputs.push_back({semantic::VertexInput::Meaning::Color, 4, 12});
    rasterInterface.vertexInputs.push_back({semantic::VertexInput::Meaning::TexCoord, 2, 28});
    rasterInterface.constantDataBytes = FrameConstantBytes;
    rasterInterface.constantDataAlignment = FrameConstantAlignment;
    rasterInterface.sampledTextureCount = 1;
    rasterInterface.sampledBufferCount = 4;
    auto rasterProgram = builder.AddRasterProgram("CommonExperimentRasterProgram", std::move(rasterInterface), {ShaderSource, "VSMain", "PSMain", {}});
    if (!rasterProgram) return base::Result<semantic::SemanticGraph, std::string>::Failure(rasterProgram.Error());

    semantic::ProgramInterface computeInterface;
    computeInterface.constantDataBytes = FrameConstantBytes;
    computeInterface.constantDataAlignment = FrameConstantAlignment;
    computeInterface.sampledBufferCount = 1;
    computeInterface.unorderedBufferCount = 1;
    auto computeProgram = builder.AddComputeProgram("CommonExperimentComputeProgram", std::move(computeInterface), {ShaderSource, {}, {}, "CSMain"});
    if (!computeProgram) return base::Result<semantic::SemanticGraph, std::string>::Failure(computeProgram.Error());

    // Declaration order is intentionally not execution order. SemanticAnalysis derives Copy -> Compute -> Raster.
    auto rasterWork = builder.AddRasterWork("DrawCommonExperiment", rasterProgram.Value(), vertexUse.Value(), rasterConstantUse.Value(), textureUse.Value(), computedReadUse.Value(), copiedReadUse.Value(), aliasedReadUse.Value(), externalReadUse.Value(), colorUse.Value(), depthUse.Value(), presentUse.Value(), static_cast<std::uint32_t>(geometry.vertices.size()));
    if (!rasterWork) return base::Result<semantic::SemanticGraph, std::string>::Failure(rasterWork.Error());
    auto computeWork = builder.AddComputeWork("AccumulateTemporalColor", computeProgram.Value(), computeConstantUse.Value(), temporalPreviousUse.Value(), computeOutputUse.Value(), 1, 1, 1);
    if (!computeWork) return base::Result<semantic::SemanticGraph, std::string>::Failure(computeWork.Error());
    auto copyWork = builder.AddCopyWork("CopyStaticColor", copySourceUse.Value(), copyDestinationUse.Value(), CopyColorBytes);
    if (!copyWork) return base::Result<semantic::SemanticGraph, std::string>::Failure(copyWork.Error());

    return base::Result<semantic::SemanticGraph, std::string>::Success(std::move(builder).Build());
}
}
