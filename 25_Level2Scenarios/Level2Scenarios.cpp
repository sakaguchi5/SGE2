#include "Level2Scenarios.h"

#include "../03_SemanticBuilder/SemanticBuilder.h"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace sge::level2
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

void Take(base::Result<void, std::string> result, std::string& error)
{
    if (!result && error.empty()) error = result.Error();
}

sem::ProgramInterface ComputeInterface(std::initializer_list<sem::ProgramParameter> parameters)
{
    sem::ProgramInterface result;
    result.parameters.assign(parameters.begin(), parameters.end());
    return result;
}

sem::ProgramInterface RasterInterface()
{
    sem::ProgramInterface result;
    result.vertexStrideBytes = 24;
    result.vertexInputs = {
        {sem::VertexInput::Meaning::Position, 3, 0},
        {sem::VertexInput::Meaning::Color, 3, 12}};
    return result;
}

std::array<float, 16> InputValues()
{
    return {
        1.0f, 2.0f, 3.0f, 4.0f,
        5.0f, 6.0f, 7.0f, 8.0f,
        9.0f, 10.0f, 11.0f, 12.0f,
        13.0f, 14.0f, 15.0f, 16.0f};
}

std::array<float, 18> FirstTriangle()
{
    return {
        -0.90f, -0.70f, 0.0f, 0.90f, 0.20f, 0.20f,
        -0.15f,  0.70f, 0.0f, 0.20f, 0.90f, 0.20f,
         0.15f, -0.70f, 0.0f, 0.20f, 0.20f, 0.90f};
}

std::array<float, 18> SecondTriangle()
{
    return {
        -0.15f, -0.70f, 0.0f, 0.95f, 0.75f, 0.15f,
         0.15f,  0.70f, 0.0f, 0.15f, 0.85f, 0.95f,
         0.90f, -0.70f, 0.0f, 0.85f, 0.15f, 0.80f};
}

base::Result<StageHInput, std::string> BuildComputeCopyZigZag()
{
    sem::SemanticBuilder builder;
    std::string error;
    const auto inputValues = InputValues();
    const auto inputBytes = std::as_bytes(std::span<const float>(inputValues));

    const auto input = Take(builder.AddImmutableBuffer("StageH.ZigZag.Input", 16, inputBytes), error);
    const auto stageA = Take(builder.AddPersistentGpuWrittenBuffer("StageH.ZigZag.StageA", 64, 16), error);
    const auto stageB = Take(builder.AddGpuWrittenBuffer("StageH.ZigZag.StageB", 64, 16), error);
    const auto result = Take(builder.AddPersistentGpuWrittenBuffer("StageH.ZigZag.Result", 64, 16), error);

    // ResourceUse and Work registration intentionally begin at the consumer.
    const auto finalInput = Take(builder.AddUse(stageB, sem::Effect::Read, sem::ViewRole::ShaderBuffer), error);
    const auto finalOutput = Take(builder.AddUse(result, sem::Effect::Write, sem::ViewRole::StorageBuffer), error);
    const auto copySource = Take(builder.AddUse(stageA, sem::Effect::Read, sem::ViewRole::CopySource), error);
    const auto copyDestination = Take(builder.AddUse(stageB, sem::Effect::Write, sem::ViewRole::CopyDestination), error);
    const auto seedInput = Take(builder.AddUse(input, sem::Effect::Read, sem::ViewRole::ShaderBuffer), error);
    const auto seedOutput = Take(builder.AddUse(stageA, sem::Effect::Write, sem::ViewRole::StorageBuffer), error);

    const auto finalProgram = Take(builder.AddComputeProgram(
        "StageH.ZigZag.FinalProgram",
        ComputeInterface({
            {sem::ProgramParameterId{0}, "StageB", sem::ProgramParameterKind::ReadOnlyBuffer, sem::ShaderStage::Compute, 0, 0, 1},
            {sem::ProgramParameterId{1}, "Result", sem::ProgramParameterKind::UnorderedBuffer, sem::ShaderStage::Compute, 0, 0, 1}}),
        {R"hlsl(
StructuredBuffer<float4> StageB : register(t0);
RWStructuredBuffer<float4> Result : register(u0);
[numthreads(1, 1, 1)]
void CSFinalize(uint3 id : SV_DispatchThreadID)
{
    Result[id.x] = StageB[id.x] + float4(0.5f, 0.25f, 0.125f, 1.0f);
}
)hlsl", {}, {}, "CSFinalize"}), error);

    const auto seedProgram = Take(builder.AddComputeProgram(
        "StageH.ZigZag.SeedProgram",
        ComputeInterface({
            {sem::ProgramParameterId{0}, "Input", sem::ProgramParameterKind::ReadOnlyBuffer, sem::ShaderStage::Compute, 0, 0, 1},
            {sem::ProgramParameterId{1}, "StageA", sem::ProgramParameterKind::UnorderedBuffer, sem::ShaderStage::Compute, 0, 0, 1}}),
        {R"hlsl(
StructuredBuffer<float4> Input : register(t0);
RWStructuredBuffer<float4> StageA : register(u0);
[numthreads(1, 1, 1)]
void CSSeed(uint3 id : SV_DispatchThreadID)
{
    StageA[id.x] = Input[id.x] * 2.0f;
}
)hlsl", {}, {}, "CSSeed"}), error);

    const std::array finalOperands = {
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, finalInput, sem::ProgramParameterId{0}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, finalOutput, sem::ProgramParameterId{1}}};
    const auto finalWork = Take(builder.AddComputeWorkGeneric(
        "StageH.ZigZag.Finalize", finalProgram, finalOperands, 4, 1, 1), error);
    const auto copyWork = Take(builder.AddCopyWork(
        "StageH.ZigZag.CopyMiddle", copySource, copyDestination, 64), error);
    const std::array seedOperands = {
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, seedInput, sem::ProgramParameterId{0}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, seedOutput, sem::ProgramParameterId{1}}};
    const auto seedWork = Take(builder.AddComputeWorkGeneric(
        "StageH.ZigZag.Seed", seedProgram, seedOperands, 4, 1, 1), error);

    if (!error.empty()) return base::Result<StageHInput, std::string>::Failure(std::move(error));

    StageHInput output;
    output.scenario = StageHScenario::ComputeCopyZigZag;
    output.name = "Stage H non-historical Compute -> Copy -> Compute zig-zag";
    output.graph = std::move(builder).Build();
    output.targetProfile.computeQueueCount = 1;
    output.targetProfile.copyQueueCount = 1;
    output.targetProfile.surfaceImageCount = 0;
    output.targetProfile.rtvDescriptorCount = 0;
    output.targetProfile.dsvDescriptorCount = 0;
    output.targetProfile.shaderDescriptorCount = 8;
    output.expectations.canonicalWorkOrder = {seedWork, copyWork, finalWork};
    output.expectations.executionOrder = {
        {sem::WorkKind::Compute, 1},
        {sem::WorkKind::Copy, 2},
        {sem::WorkKind::Compute, 1}};
    output.expectations.resourceCount = 4;
    output.expectations.resourceUseCount = 6;
    output.expectations.sourceProgramCount = 2;
    output.expectations.shaderCount = 2;
    output.expectations.waitQueueCount = 2;
    output.expectations.computeCompletion = true;
    output.expectations.copyCompletion = true;
    return base::Result<StageHInput, std::string>::Success(std::move(output));
}

base::Result<StageHInput, std::string> BuildComputeDiamond()
{
    sem::SemanticBuilder builder;
    std::string error;
    const auto inputValues = InputValues();
    const auto inputBytes = std::as_bytes(std::span<const float>(inputValues));

    const auto input = Take(builder.AddImmutableBuffer("StageH.Diamond.Input", 16, inputBytes), error);
    const auto left = Take(builder.AddPersistentGpuWrittenBuffer("StageH.Diamond.Left", 64, 16), error);
    const auto right = Take(builder.AddPersistentGpuWrittenBuffer("StageH.Diamond.Right", 64, 16), error);
    const auto outputBuffer = Take(builder.AddPersistentGpuWrittenBuffer("StageH.Diamond.Output", 64, 16), error);

    const auto mergeLeft = Take(builder.AddUse(left, sem::Effect::Read, sem::ViewRole::ShaderBuffer), error);
    const auto mergeRight = Take(builder.AddUse(right, sem::Effect::Read, sem::ViewRole::ShaderBuffer), error);
    const auto mergeOutput = Take(builder.AddUse(outputBuffer, sem::Effect::Write, sem::ViewRole::StorageBuffer), error);
    const auto rightInput = Take(builder.AddUse(input, sem::Effect::Read, sem::ViewRole::ShaderBuffer), error);
    const auto rightOutput = Take(builder.AddUse(right, sem::Effect::Write, sem::ViewRole::StorageBuffer), error);
    const auto leftInput = Take(builder.AddUse(input, sem::Effect::Read, sem::ViewRole::ShaderBuffer), error);
    const auto leftOutput = Take(builder.AddUse(left, sem::Effect::Write, sem::ViewRole::StorageBuffer), error);

    const auto mergeProgram = Take(builder.AddComputeProgram(
        "StageH.Diamond.MergeProgram",
        ComputeInterface({
            {sem::ProgramParameterId{0}, "Left", sem::ProgramParameterKind::ReadOnlyBuffer, sem::ShaderStage::Compute, 0, 0, 1},
            {sem::ProgramParameterId{1}, "Right", sem::ProgramParameterKind::ReadOnlyBuffer, sem::ShaderStage::Compute, 1, 0, 1},
            {sem::ProgramParameterId{2}, "Output", sem::ProgramParameterKind::UnorderedBuffer, sem::ShaderStage::Compute, 0, 0, 1}}),
        {R"hlsl(
StructuredBuffer<float4> Left : register(t0);
StructuredBuffer<float4> Right : register(t1);
RWStructuredBuffer<float4> Output : register(u0);
[numthreads(1, 1, 1)]
void CSMerge(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = Left[id.x] + Right[id.x];
}
)hlsl", {}, {}, "CSMerge"}), error);

    const auto branchProgram = Take(builder.AddComputeProgram(
        "StageH.Diamond.SharedBranchProgram",
        ComputeInterface({
            {sem::ProgramParameterId{0}, "Input", sem::ProgramParameterKind::ReadOnlyBuffer, sem::ShaderStage::Compute, 0, 0, 1},
            {sem::ProgramParameterId{1}, "Branch", sem::ProgramParameterKind::UnorderedBuffer, sem::ShaderStage::Compute, 0, 0, 1}}),
        {R"hlsl(
StructuredBuffer<float4> Input : register(t0);
RWStructuredBuffer<float4> Branch : register(u0);
[numthreads(1, 1, 1)]
void CSBranch(uint3 id : SV_DispatchThreadID)
{
    Branch[id.x] = Input[id.x] + float4(1.0f, 2.0f, 3.0f, 4.0f);
}
)hlsl", {}, {}, "CSBranch"}), error);

    const std::array mergeOperands = {
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, mergeLeft, sem::ProgramParameterId{0}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, mergeRight, sem::ProgramParameterId{1}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, mergeOutput, sem::ProgramParameterId{2}}};
    const auto mergeWork = Take(builder.AddComputeWorkGeneric(
        "StageH.Diamond.Merge", mergeProgram, mergeOperands, 4, 1, 1), error);

    const std::array rightOperands = {
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, rightInput, sem::ProgramParameterId{0}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, rightOutput, sem::ProgramParameterId{1}}};
    const auto rightWork = Take(builder.AddComputeWorkGeneric(
        "StageH.Diamond.RightBranch", branchProgram, rightOperands, 4, 1, 1), error);

    const std::array leftOperands = {
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, leftInput, sem::ProgramParameterId{0}},
        sem::WorkOperand{sem::WorkOperandKind::ProgramParameter, leftOutput, sem::ProgramParameterId{1}}};
    const auto leftWork = Take(builder.AddComputeWorkGeneric(
        "StageH.Diamond.LeftBranch", branchProgram, leftOperands, 4, 1, 1), error);

    if (!error.empty()) return base::Result<StageHInput, std::string>::Failure(std::move(error));

    StageHInput output;
    output.scenario = StageHScenario::ComputeDiamond;
    output.name = "Stage H non-historical Compute fan-out / fan-in diamond";
    output.graph = std::move(builder).Build();
    output.targetProfile.computeQueueCount = 1;
    output.targetProfile.copyQueueCount = 0;
    output.targetProfile.surfaceImageCount = 0;
    output.targetProfile.rtvDescriptorCount = 0;
    output.targetProfile.dsvDescriptorCount = 0;
    output.targetProfile.shaderDescriptorCount = 8;
    output.expectations.canonicalWorkOrder = {rightWork, leftWork, mergeWork};
    output.expectations.executionOrder = {
        {sem::WorkKind::Compute, 1},
        {sem::WorkKind::Compute, 1},
        {sem::WorkKind::Compute, 1}};
    output.expectations.resourceCount = 4;
    output.expectations.resourceUseCount = 7;
    output.expectations.sourceProgramCount = 2;
    output.expectations.shaderCount = 2;
    output.expectations.waitQueueCount = 0;
    output.expectations.computeCompletion = true;
    return base::Result<StageHInput, std::string>::Success(std::move(output));
}

base::Result<StageHInput, std::string> BuildTwoPassRaster()
{
    sem::SemanticBuilder builder;
    std::string error;
    const auto firstTriangle = FirstTriangle();
    const auto secondTriangle = SecondTriangle();
    const auto firstBytes = std::as_bytes(std::span<const float>(firstTriangle));
    const auto secondBytes = std::as_bytes(std::span<const float>(secondTriangle));

    const auto firstVertex = Take(builder.AddImmutableBuffer("StageH.TwoPass.FirstVertex", 24, firstBytes), error);
    const auto secondVertex = Take(builder.AddImmutableBuffer("StageH.TwoPass.SecondVertex", 24, secondBytes), error);
    const auto surface = Take(builder.AddPresentationSurface("StageH.TwoPass.Surface", sem::FormatMeaning::Bgra8Unorm), error);

    // Pass-two uses are registered first to make use-table order unrelated to execution order.
    const auto secondVertexUse = Take(builder.AddUse(secondVertex, sem::Effect::Read, sem::ViewRole::VertexData), error);
    const auto secondColorUse = Take(builder.AddUse(surface, sem::Effect::Write, sem::ViewRole::ColorAttachment), error);
    const auto secondPresentUse = Take(builder.AddUse(surface, sem::Effect::Read, sem::ViewRole::PresentSource), error);
    const auto firstVertexUse = Take(builder.AddUse(firstVertex, sem::Effect::Read, sem::ViewRole::VertexData), error);
    const auto firstColorUse = Take(builder.AddUse(surface, sem::Effect::Write, sem::ViewRole::ColorAttachment), error);
    const auto firstPresentUse = Take(builder.AddUse(surface, sem::Effect::Read, sem::ViewRole::PresentSource), error);

    sem::ProgramSource source;
    source.hlslSource = R"hlsl(
struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR0;
};
struct VSOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};
VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0f);
    output.color = input.color;
    return output;
}
float4 PSMain(VSOutput input) : SV_Target0
{
    return float4(input.color, 1.0f);
}
)hlsl";
    source.vertexEntry = "VSMain";
    source.pixelEntry = "PSMain";
    const auto program = Take(builder.AddRasterProgram(
        "StageH.TwoPass.SharedRasterProgram", RasterInterface(), std::move(source)), error);

    const std::array firstOperands = {
        sem::WorkOperand{sem::WorkOperandKind::VertexData, firstVertexUse, {}},
        sem::WorkOperand{sem::WorkOperandKind::ColorAttachment, firstColorUse, {}},
        sem::WorkOperand{sem::WorkOperandKind::PresentSource, firstPresentUse, {}}};
    const auto firstWork = Take(builder.AddRasterWorkGeneric(
        "StageH.TwoPass.First", program, firstOperands, 3), error);

    const std::array secondOperands = {
        sem::WorkOperand{sem::WorkOperandKind::VertexData, secondVertexUse, {}},
        sem::WorkOperand{sem::WorkOperandKind::ColorAttachment, secondColorUse, {}},
        sem::WorkOperand{sem::WorkOperandKind::PresentSource, secondPresentUse, {}}};
    const auto secondWork = Take(builder.AddRasterWorkGeneric(
        "StageH.TwoPass.Second", program, secondOperands, 3), error);
    Take(builder.AddDependency(firstWork, secondWork), error);

    if (!error.empty()) return base::Result<StageHInput, std::string>::Failure(std::move(error));

    StageHInput output;
    output.scenario = StageHScenario::TwoPassRaster;
    output.name = "Stage H non-historical two-pass Raster Surface graph";
    output.graph = std::move(builder).Build();
    // Top-level storage order is deliberately unrelated to identity and execution order.
    std::reverse(output.graph.resources.begin(), output.graph.resources.end());
    std::reverse(output.graph.resourceUses.begin(), output.graph.resourceUses.end());
    std::reverse(output.graph.programs.begin(), output.graph.programs.end());
    std::reverse(output.graph.works.begin(), output.graph.works.end());
    output.targetProfile.computeQueueCount = 0;
    output.targetProfile.copyQueueCount = 0;
    output.targetProfile.surfaceImageCount = 2;
    output.targetProfile.rtvDescriptorCount = 4;
    output.targetProfile.dsvDescriptorCount = 0;
    output.targetProfile.shaderDescriptorCount = 0;
    output.expectations.canonicalWorkOrder = {firstWork, secondWork};
    output.expectations.executionOrder = {
        {sem::WorkKind::Raster, 0},
        {sem::WorkKind::Raster, 0}};
    output.expectations.resourceCount = 3;
    output.expectations.resourceUseCount = 6;
    output.expectations.sourceProgramCount = 1;
    output.expectations.shaderCount = 2;
    output.expectations.waitQueueCount = 0;
    output.expectations.surface = true;
    output.expectations.directCompletion = true;
    return base::Result<StageHInput, std::string>::Success(std::move(output));
}
}

std::vector<StageHScenario> StageHInputs()
{
    return {
        StageHScenario::ComputeCopyZigZag,
        StageHScenario::ComputeDiamond,
        StageHScenario::TwoPassRaster};
}

base::Result<StageHInput, std::string> BuildStageHScenario(StageHScenario scenario)
{
    switch (scenario)
    {
    case StageHScenario::ComputeCopyZigZag: return BuildComputeCopyZigZag();
    case StageHScenario::ComputeDiamond: return BuildComputeDiamond();
    case StageHScenario::TwoPassRaster: return BuildTwoPassRaster();
    default: return base::Result<StageHInput, std::string>::Failure("unknown Stage H scenario");
    }
}
}
