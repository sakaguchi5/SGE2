#include "RuntimeScenarios.h"

#include "../03_SemanticBuilder/SemanticBuilder.h"

#include <span>
#include <utility>

namespace sge::level2::runtime_scenarios
{
namespace
{
namespace sem = semantic;

template<class T>
T Take(base::Result<T, std::string> result, std::string& error)
{
    if (!result)
    {
        if (error.empty()) error = result.Error();
        return T{};
    }
    return std::move(result).Value();
}

sem::ProgramParameter Parameter(std::uint32_t id,
                                const char* name,
                                sem::ProgramParameterKind kind,
                                std::uint32_t shaderRegister,
                                std::uint64_t requiredBytes = 0,
                                std::uint32_t requiredAlignment = 1)
{
    return {{id}, name, kind, sem::ShaderStage::Compute, shaderRegister,
            requiredBytes, requiredAlignment};
}

base::Result<Input, std::string> BuildDynamicExternalPipeline()
{
    sem::SemanticBuilder builder;
    std::string error;

    const auto dynamicA = Take(builder.AddDynamicBuffer("StageK.Pipeline.DynamicA", 16, 16), error);
    const auto dynamicB = Take(builder.AddDynamicBuffer("StageK.Pipeline.DynamicB", 16, 16), error);
    const auto externalInput = Take(builder.AddExternalBuffer("StageK.Pipeline.ExternalInput", 16, 16), error);
    const auto stageA = Take(builder.AddGpuWrittenBuffer("StageK.Pipeline.FrameLocalA", 16, 16), error);
    const auto stageB = Take(builder.AddGpuWrittenBuffer("StageK.Pipeline.FrameLocalB", 16, 16), error);
    const auto externalOutputA = Take(builder.AddExternalBuffer("StageK.Pipeline.ExternalOutputA", 16, 16), error);
    const auto externalOutputB = Take(builder.AddExternalBuffer("StageK.Pipeline.ExternalOutputB", 16, 16), error);

    const auto dynamicASeed = Take(builder.AddUse(dynamicA, sem::Effect::Read, sem::ViewRole::ConstantData), error);
    const auto externalInputUse = Take(builder.AddUse(externalInput, sem::Effect::Read, sem::ViewRole::ShaderBuffer), error);
    const auto stageAWrite = Take(builder.AddUse(stageA, sem::Effect::Write, sem::ViewRole::StorageBuffer), error);
    const auto stageACopy = Take(builder.AddUse(stageA, sem::Effect::Read, sem::ViewRole::CopySource), error);
    const auto stageBCopy = Take(builder.AddUse(stageB, sem::Effect::Write, sem::ViewRole::CopyDestination), error);
    const auto stageBRead = Take(builder.AddUse(stageB, sem::Effect::Read, sem::ViewRole::ShaderBuffer), error);
    const auto dynamicBFinal = Take(builder.AddUse(dynamicB, sem::Effect::Read, sem::ViewRole::ConstantData), error);
    const auto outputAWrite = Take(builder.AddUse(externalOutputA, sem::Effect::Write, sem::ViewRole::StorageBuffer), error);
    const auto outputACopy = Take(builder.AddUse(externalOutputA, sem::Effect::Read, sem::ViewRole::CopySource), error);
    const auto outputBCopy = Take(builder.AddUse(externalOutputB, sem::Effect::Write, sem::ViewRole::CopyDestination), error);

    sem::ProgramInterface seedInterface;
    seedInterface.parameters = {
        Parameter(0, "DynamicA", sem::ProgramParameterKind::ConstantBuffer, 0, 16, 16),
        Parameter(1, "ExternalInput", sem::ProgramParameterKind::ReadOnlyBuffer, 0),
        Parameter(2, "StageA", sem::ProgramParameterKind::UnorderedBuffer, 0)};
    const auto seedProgram = Take(builder.AddComputeProgram(
        "StageK.Pipeline.SeedProgram", std::move(seedInterface),
        {R"hlsl(
cbuffer DynamicAData : register(b0) { float4 DynamicA; };
StructuredBuffer<float4> ExternalInput : register(t0);
RWStructuredBuffer<float4> StageA : register(u0);
[numthreads(1, 1, 1)]
void CSSeed(uint3 id : SV_DispatchThreadID)
{
    StageA[0] = DynamicA + ExternalInput[0];
}
)hlsl", {}, {}, "CSSeed"}), error);

    sem::ProgramInterface finalInterface;
    finalInterface.parameters = {
        Parameter(0, "StageB", sem::ProgramParameterKind::ReadOnlyBuffer, 0),
        Parameter(1, "DynamicB", sem::ProgramParameterKind::ConstantBuffer, 0, 16, 16),
        Parameter(2, "ExternalOutputA", sem::ProgramParameterKind::UnorderedBuffer, 0)};
    const auto finalProgram = Take(builder.AddComputeProgram(
        "StageK.Pipeline.FinalProgram", std::move(finalInterface),
        {R"hlsl(
StructuredBuffer<float4> StageB : register(t0);
cbuffer DynamicBData : register(b0) { float4 DynamicB; };
RWStructuredBuffer<float4> ExternalOutputA : register(u0);
[numthreads(1, 1, 1)]
void CSFinal(uint3 id : SV_DispatchThreadID)
{
    ExternalOutputA[0] = StageB[0] + DynamicB * 2.0f;
}
)hlsl", {}, {}, "CSFinal"}), error);

    const std::array seedOperands = {
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, dynamicASeed, {0}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, externalInputUse, {1}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, stageAWrite, {2}}};
    Take(builder.AddComputeWorkGeneric("StageK.Pipeline.Seed", seedProgram, seedOperands, 1, 1, 1), error);
    Take(builder.AddCopyWork("StageK.Pipeline.CopyAtoB", stageACopy, stageBCopy, 16), error);
    const std::array finalOperands = {
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, stageBRead, {0}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, dynamicBFinal, {1}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, outputAWrite, {2}}};
    Take(builder.AddComputeWorkGeneric("StageK.Pipeline.Final", finalProgram, finalOperands, 1, 1, 1), error);
    Take(builder.AddCopyWork("StageK.Pipeline.CopyOutput", outputACopy, outputBCopy, 16), error);

    if (!error.empty()) return base::Result<Input, std::string>::Failure(std::move(error));
    Input result;
    result.scenario = Scenario::DynamicExternalPipeline;
    result.name = "Stage K dynamic/external Compute-Copy semantic pipeline";
    result.graph = std::move(builder).Build();
    result.targetProfile.computeQueueCount = 1;
    result.targetProfile.copyQueueCount = 1;
    result.targetProfile.surfaceImageCount = 0;
    result.targetProfile.rtvDescriptorCount = 0;
    result.targetProfile.dsvDescriptorCount = 0;
    result.targetProfile.shaderDescriptorCount = 16;
    result.dynamicSlotCount = 2;
    result.externalSlotCount = 3;
    return base::Result<Input, std::string>::Success(std::move(result));
}

base::Result<Input, std::string> BuildCrossQueueTemporal()
{
    sem::SemanticBuilder builder;
    std::string error;
    const auto& seed = TemporalSeed();
    const auto seedBytes = std::as_bytes(std::span<const float>(seed));

    const auto dynamicValue = Take(builder.AddDynamicBuffer("StageK.Temporal.DynamicValue", 16, 16), error);
    const auto temporal = Take(builder.AddTemporalGpuWrittenBuffer("StageK.Temporal.History", 16, seedBytes), error);
    const auto externalOutput = Take(builder.AddExternalBuffer("StageK.Temporal.ExternalOutput", 16, 16), error);

    const auto previousSource = Take(builder.AddUse(temporal, sem::Effect::Read,
        sem::ViewRole::CopySource, sem::TemporalRelation::Previous), error);
    const auto externalDestination = Take(builder.AddUse(externalOutput, sem::Effect::Write,
        sem::ViewRole::CopyDestination), error);
    const auto dynamicRead = Take(builder.AddUse(dynamicValue, sem::Effect::Read,
        sem::ViewRole::ConstantData), error);
    const auto currentWrite = Take(builder.AddUse(temporal, sem::Effect::Write,
        sem::ViewRole::StorageBuffer, sem::TemporalRelation::Current), error);

    sem::ProgramInterface writerInterface;
    writerInterface.parameters = {
        Parameter(0, "DynamicValue", sem::ProgramParameterKind::ConstantBuffer, 0, 16, 16),
        Parameter(1, "TemporalCurrent", sem::ProgramParameterKind::UnorderedBuffer, 0)};
    const auto writerProgram = Take(builder.AddComputeProgram(
        "StageK.Temporal.WriterProgram", std::move(writerInterface),
        {R"hlsl(
cbuffer DynamicValueData : register(b0) { float4 DynamicValue; };
RWStructuredBuffer<float4> TemporalCurrent : register(u0);
[numthreads(1, 1, 1)]
void CSWriteTemporal(uint3 id : SV_DispatchThreadID)
{
    TemporalCurrent[0] = DynamicValue;
}
)hlsl", {}, {}, "CSWriteTemporal"}), error);

    // Copy is intentionally registered before the current-frame writer. It reads
    // the previous physical generation, while WaitTemporal references the writer
    // signal from the preceding frame.
    Take(builder.AddCopyWork("StageK.Temporal.CopyPrevious", previousSource, externalDestination, 16), error);
    const std::array writerOperands = {
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, dynamicRead, {0}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, currentWrite, {1}}};
    Take(builder.AddComputeWorkGeneric("StageK.Temporal.WriteCurrent", writerProgram,
        writerOperands, 1, 1, 1), error);

    if (!error.empty()) return base::Result<Input, std::string>::Failure(std::move(error));
    Input result;
    result.scenario = Scenario::CrossQueueTemporal;
    result.name = "Stage K cross-queue Temporal previous/current semantic pipeline";
    result.graph = std::move(builder).Build();
    result.targetProfile.framesInFlight = 2;
    result.targetProfile.computeQueueCount = 1;
    result.targetProfile.copyQueueCount = 1;
    result.targetProfile.surfaceImageCount = 0;
    result.targetProfile.rtvDescriptorCount = 0;
    result.targetProfile.dsvDescriptorCount = 0;
    result.targetProfile.shaderDescriptorCount = 4;
    result.dynamicSlotCount = 1;
    result.externalSlotCount = 1;
    return base::Result<Input, std::string>::Success(std::move(result));
}
}

base::Result<Input, std::string> Build(Scenario scenario)
{
    switch (scenario)
    {
    case Scenario::DynamicExternalPipeline: return BuildDynamicExternalPipeline();
    case Scenario::CrossQueueTemporal: return BuildCrossQueueTemporal();
    default: return base::Result<Input, std::string>::Failure("unknown Stage-K scenario");
    }
}

Float4 PipelineExpected(const Float4& dynamicA,
                        const Float4& dynamicB,
                        const Float4& externalInput) noexcept
{
    Float4 result{};
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = dynamicA[index] + dynamicB[index] * 2.0f + externalInput[index];
    return result;
}

const Float4& TemporalSeed() noexcept
{
    static constexpr Float4 seed{-10.0f, -20.0f, -30.0f, -40.0f};
    return seed;
}
}
