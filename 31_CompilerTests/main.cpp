#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../05_TargetModel/TargetModel.h"
#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../08_D3D12PackageSchema/D3D12Encoding.h"
#include "../20_ClassicalFrontend/ClassicalTriangle.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace
{
namespace sem = sge::semantic;
namespace pkg = sge::package::d3d12_v13;

bool Contains(std::span<const std::byte> bytes, std::string_view text)
{
    const auto* begin = reinterpret_cast<const char*>(bytes.data());
    return std::search(begin, begin + bytes.size(), text.begin(), text.end()) != begin + bytes.size();
}

sem::SemanticGraph RemapToSparseIds(sem::SemanticGraph graph)
{
    std::map<std::uint32_t, sem::ResourceId> resources;
    std::map<std::uint32_t, sem::ResourceUseId> uses;
    std::map<std::uint32_t, sem::ProgramId> programs;
    std::map<std::uint32_t, sem::WorkId> works;
    for (const auto& value : graph.resources) resources[value.id.value] = {value.id.value * 10u + 7u};
    for (const auto& value : graph.resourceUses) uses[value.id.value] = {value.id.value * 10u + 9u};
    for (const auto& value : graph.programs) programs[value.id.value] = {value.id.value * 10u + 11u};
    for (const auto& value : graph.works) works[value.id.value] = {value.id.value * 10u + 13u};

    for (auto& resource : graph.resources)
    {
        const auto oldId = resource.id;
        resource.id = resources.at(oldId.value);
        if (resource.aliasPreparation.IsValid())
            resource.aliasPreparation = resources.at(resource.aliasPreparation.value);
    }
    for (auto& use : graph.resourceUses)
    {
        use.id = uses.at(use.id.value);
        use.resource = resources.at(use.resource.value);
    }
    const auto mapUse = [&](sem::ResourceUseId id) { return id.IsValid() ? uses.at(id.value) : id; };
    const auto mapProgram = [&](sem::ProgramId id) { return id.IsValid() ? programs.at(id.value) : id; };
    for (auto& program : graph.programs) program.id = programs.at(program.id.value);
    for (auto& work : graph.works)
    {
        const auto oldWork = work.id;
        work.id = works.at(oldWork.value);
        for (auto& operand : work.operands) operand.use = mapUse(operand.use);
        for (auto& dependency : work.dependencies) dependency = works.at(dependency.value);
        work.raster.program = mapProgram(work.raster.program);
        work.compute.program = mapProgram(work.compute.program);
    }
    std::reverse(graph.resources.begin(), graph.resources.end());
    std::reverse(graph.resourceUses.begin(), graph.resourceUses.end());
    std::reverse(graph.programs.begin(), graph.programs.end());
    std::reverse(graph.works.begin(), graph.works.end());
    return graph;
}

const sem::WorkOperand& FindOperand(const sem::Work& work, sem::WorkOperandKind kind)
{
    return *std::find_if(work.operands.begin(), work.operands.end(), [kind](const auto& operand) {
        return operand.kind == kind;
    });
}

sem::SemanticGraph AddIndependentCopy(sem::SemanticGraph graph)
{
    const auto& oldCopy = *std::find_if(graph.works.begin(), graph.works.end(), [](const auto& work) {
        return work.kind == sem::WorkKind::Copy;
    });
    const auto& oldSourceUse = graph.resourceUses[FindOperand(oldCopy, sem::WorkOperandKind::CopySource).use.value];
    const auto& oldDestinationUse = graph.resourceUses[FindOperand(oldCopy, sem::WorkOperandKind::CopyDestination).use.value];

    auto source = graph.resources[oldSourceUse.resource.value];
    source.id = {100};
    source.aliasPreparation = {};
    source.debugName = "GenericExtraCopySource";
    auto destination = graph.resources[oldDestinationUse.resource.value];
    destination.id = {200};
    destination.aliasPreparation = {};
    destination.debugName = "GenericExtraCopyDestination";
    graph.resources.push_back(source);
    graph.resources.push_back(destination);

    sem::ResourceUse sourceUse{{100}, source.id, sem::Effect::Read, sem::ViewRole::CopySource, sem::TemporalRelation::Current};
    sem::ResourceUse destinationUse{{101}, destination.id, sem::Effect::Write, sem::ViewRole::CopyDestination, sem::TemporalRelation::Current};
    graph.resourceUses.push_back(sourceUse);
    graph.resourceUses.push_back(destinationUse);

    sem::Work work;
    work.id = {100};
    work.debugName = "GenericExtraCopy";
    work.kind = sem::WorkKind::Copy;
    work.operands = {
        {sem::WorkOperandKind::CopySource, sourceUse.id, {}},
        {sem::WorkOperandKind::CopyDestination, destinationUse.id, {}}};
    work.copy.bytes = oldCopy.copy.bytes;
    graph.works.push_back(work);
    return graph;
}

bool ReplaceOnce(std::string& text, std::string_view before, std::string_view after)
{
    const auto position = text.find(before);
    if (position == std::string::npos) return false;
    text.replace(position, before.size(), after);
    return true;
}

sem::SemanticGraph AddRasterBinding(sem::SemanticGraph graph)
{
    const auto source = std::find_if(graph.resources.begin(), graph.resources.end(), [](const auto& resource) {
        return resource.kind == sem::ResourceKind::Buffer &&
               resource.update == sem::UpdateIntent::Immutable &&
               resource.lifetime == sem::LifetimeIntent::Persistent &&
               resource.buffer.sizeBytes == 16 && !resource.aliasPreparation.IsValid();
    });
    auto resource = *source;
    resource.id = {300};
    resource.debugName = "GenericAdditionalRasterBinding";
    graph.resources.push_back(resource);
    sem::ResourceUse use{{300}, resource.id, sem::Effect::Read, sem::ViewRole::ShaderBuffer, sem::TemporalRelation::Current};
    graph.resourceUses.push_back(use);
    auto raster = std::find_if(graph.works.begin(), graph.works.end(), [](const auto& work) {
        return work.kind == sem::WorkKind::Raster;
    });
    auto program = std::find_if(graph.programs.begin(), graph.programs.end(), [&](const auto& value) {
        return value.id == raster->raster.program;
    });
    const sem::ProgramParameterId parameterId{static_cast<std::uint32_t>(program->interface.parameters.size())};
    program->interface.parameters.push_back({parameterId, "GenericAdditionalRasterBinding",
        sem::ProgramParameterKind::ReadOnlyBuffer, sem::ShaderStage::Pixel, 5, 0, 1});
    raster->operands.push_back({sem::WorkOperandKind::ProgramParameter, use.id, parameterId});
    if (!ReplaceOnce(program->source.hlslSource,
            "SamplerState MainSampler : register(s0);",
            "StructuredBuffer<float4> GenericAdditionalRasterBinding : register(t5);\nSamplerState MainSampler : register(s0);") ||
        !ReplaceOnce(program->source.hlslSource,
            " * ExternalColorInput[0];",
            " * ExternalColorInput[0] * GenericAdditionalRasterBinding[0];"))
        std::terminate();
    return graph;
}

std::uint32_t Count(std::span<const pkg::OperationView> operations, pkg::D3D12OperationCode code)
{
    return static_cast<std::uint32_t>(std::count_if(operations.begin(), operations.end(), [code](const auto& operation) {
        return operation.opcode == code;
    }));
}
}

int main()
{
    auto graphResult = sge::classical::BuildTriangleGraph();
    if (!graphResult) return 1;
    const auto& graph = graphResult.Value();
    sge::target::D3D12TargetProfile profile;

    auto validatedStage = sge::compiler::d3d12::ValidateSourceStage(graph, profile);
    if (!validatedStage || validatedStage.Value().source != &graph ||
        validatedStage.Value().analyzed.canonicalWorkOrder.size() != graph.works.size())
        return 2;
    auto programStage = sge::compiler::d3d12::CompileProgramStage(validatedStage.Value());
    if (!programStage || programStage.Value().programs.size() != 2)
    {
        if (!programStage) std::cerr << programStage.Error().stage << ": " << programStage.Error().message << '\n';
        return 3;
    }
    std::size_t reflectedShaderCount = 0;
    std::size_t reflectedBindingCount = 0;
    std::size_t reflectedVertexInputCount = 0;
    for (const auto& program : programStage.Value().programs)
        for (const auto& shader : program.shaders)
        {
            ++reflectedShaderCount;
            reflectedBindingCount += shader.bindings.size();
            reflectedVertexInputCount += shader.vertexInputs.size();
        }
    if (reflectedShaderCount != 3 || reflectedBindingCount != 10 || reflectedVertexInputCount != 3)
        return 4;

    auto first = sge::compiler::d3d12::Compile(graph, profile);
    auto second = sge::compiler::d3d12::Compile(graph, profile);
    if (!first || !second)
    {
        const auto& error = !first ? first.Error() : second.Error();
        std::cerr << error.stage << ": " << error.message << '\n';
        return 2;
    }
    const std::vector<std::string> expectedStages = {
        "source-validation", "shader-compilation-reflection",
        "package-lowering", "package-serialization-validation"};
    if (first.Value().completedStages != expectedStages || second.Value().completedStages != expectedStages)
        return 5;
    if (first.Value().packageBytes != second.Value().packageBytes ||
        first.Value().executionDigestHex != second.Value().executionDigestHex)
        return 6;

    auto sparse = RemapToSparseIds(graph);
    auto sparsePackage = sge::compiler::d3d12::Compile(sparse, profile);
    if (!sparsePackage || sparsePackage.Value().packageBytes != first.Value().packageBytes)
    {
        if (!sparsePackage) std::cerr << sparsePackage.Error().stage << ": " << sparsePackage.Error().message << '\n';
        std::cerr << "canonical lowering depends on vector order or dense Source IDs\n";
        return 4;
    }

    for (const auto text : {"VSMain", "PSMain", "CSMain", "ComputeColorOutput", "PreviousFrameColor",
                            "CopiedColorInput", "TemporalColorHistory", "AliasWarmup", "AliasedRasterColor",
                            "ExternalFrameColor", "CommonExperimentVertices", "DrawCommonExperiment",
                            "Classical", "Sdf", "HalfSpace2D", "TriangleField"})
        if (Contains(first.Value().packageBytes, text))
        {
            std::cerr << "source leaked: " << text << '\n';
            return 5;
        }

    auto frozen = sge::package::PackageReader::Read(first.Value().packageBytes);
    if (!frozen) { std::cerr << frozen.Error().message << '\n'; return 6; }
    if (frozen.Value().Header().targetSchemaVersion != 14 ||
        frozen.Value().Header().minimumRuntimeVersion != 14)
        return 7;
    auto decoded = pkg::D3D12PackageView::Decode(frozen.Value());
    if (!decoded) { std::cerr << decoded.Error().message << '\n'; return 8; }
    const auto& view = decoded.Value();

    const auto rasterWorks = static_cast<std::size_t>(std::count_if(graph.works.begin(), graph.works.end(), [](const auto& work) {
        return work.kind == sem::WorkKind::Raster;
    }));
    const auto computeWorks = static_cast<std::size_t>(std::count_if(graph.works.begin(), graph.works.end(), [](const auto& work) {
        return work.kind == sem::WorkKind::Compute;
    }));
    const auto externalResources = static_cast<std::size_t>(std::count_if(graph.resources.begin(), graph.resources.end(), [](const auto& resource) {
        return resource.lifetime == sem::LifetimeIntent::External && resource.kind != sem::ResourceKind::SurfaceImage;
    }));
    const auto surfaceResources = static_cast<std::size_t>(std::count_if(graph.resources.begin(), graph.resources.end(), [](const auto& resource) {
        return resource.kind == sem::ResourceKind::SurfaceImage;
    }));
    const auto dynamicResources = static_cast<std::size_t>(std::count_if(graph.resources.begin(), graph.resources.end(), [](const auto& resource) {
        return resource.update == sem::UpdateIntent::DynamicPerFrame;
    }));
    if (view.Resources().size() != graph.resources.size() || view.Views().size() != graph.resourceUses.size() ||
        view.Executables().size() != rasterWorks || view.RasterCommands().size() != rasterWorks ||
        view.ComputeExecutables().size() != computeWorks || view.ComputeCommands().size() != computeWorks ||
        view.Programs().size() != rasterWorks + computeWorks ||
        view.BindingLayouts().size() != rasterWorks + computeWorks ||
        view.ExternalSlots().size() != externalResources || view.SurfaceSlots().size() != surfaceResources ||
        view.DynamicSlots().size() != dynamicResources)
    {
        std::cerr << "artifact cardinalities were not derived from the graph\n";
        return 9;
    }

    std::vector<const pkg::ResourceArtifact*> aliased;
    for (const auto& resource : view.Resources())
        if ((resource.flags & static_cast<std::uint32_t>(pkg::ResourceFlags::Aliased)) != 0)
            aliased.push_back(&resource);
    if (aliased.size() != 2 || aliased[0]->allocation != aliased[1]->allocation)
        return 10;
    const auto& aliasAllocation = view.Allocations()[aliased[0]->allocation.value];
    if (aliasAllocation.kind != pkg::AllocationKind::Placed ||
        aliasAllocation.aliasGroup == sge::package::InvalidIndex ||
        aliasAllocation.alignment != 65536)
        return 11;

    std::vector<pkg::ActivateAliasPayload> activations;
    for (const auto& operation : view.LoadOperations())
    {
        if (operation.opcode != pkg::D3D12OperationCode::ActivateAlias) continue;
        auto payload = pkg::DecodeActivateAlias(operation.payload);
        if (!payload) return 12;
        activations.push_back(payload.Value());
    }
    if (activations.size() != 2 || activations[0].before.IsValid() ||
        activations[0].after != aliased[0]->id ||
        activations[1].before != aliased[0]->id || activations[1].after != aliased[1]->id)
        return 13;

    std::uint32_t rootParameterCount = 0;
    for (const auto& layout : view.BindingLayouts())
    {
        rootParameterCount += layout.parameterRange.count;
        for (std::uint32_t index = layout.parameterRange.first;
             index < layout.parameterRange.first + layout.parameterRange.count; ++index)
        {
            const auto& parameter = view.RootParameters()[index];
            if (parameter.rootParameterIndex != index - layout.parameterRange.first) return 14;
            if (parameter.kind != pkg::RootParameterKind::ConstantBuffer && !parameter.staticView.IsValid()) return 15;
        }
    }
    if (rootParameterCount != view.RootParameters().size()) return 16;

    const auto& frame = view.FrameOperations();
    if (Count(frame, pkg::D3D12OperationCode::ExecuteRaster) != rasterWorks ||
        Count(frame, pkg::D3D12OperationCode::ExecuteCompute) != computeWorks ||
        Count(frame, pkg::D3D12OperationCode::ExecuteCopy) != 1 ||
        Count(frame, pkg::D3D12OperationCode::AcquireExternal) != externalResources ||
        Count(frame, pkg::D3D12OperationCode::ReleaseExternal) != externalResources ||
        Count(frame, pkg::D3D12OperationCode::PresentSurface) != surfaceResources ||
        Count(frame, pkg::D3D12OperationCode::ApplyDynamicData) != dynamicResources)
        return 17;
    if (Count(frame, pkg::D3D12OperationCode::WaitQueue) == 0 ||
        Count(frame, pkg::D3D12OperationCode::WaitTemporal) == 0)
        return 18;

    const auto transitionCount = Count(frame, pkg::D3D12OperationCode::Transition);
    if (transitionCount == 0) return 19;
    for (const auto& operation : frame)
    {
        if (operation.opcode != pkg::D3D12OperationCode::Transition) continue;
        auto transition = pkg::DecodeTransition(operation.payload);
        if (!transition || transition.Value().before == transition.Value().after ||
            !transition.Value().view.IsValid() || transition.Value().view.value >= view.Views().size())
            return 20;
    }

    auto extendedGraph = AddIndependentCopy(graph);
    auto extended = sge::compiler::d3d12::Compile(extendedGraph, profile);
    if (!extended)
    {
        std::cerr << extended.Error().stage << ": " << extended.Error().message << '\n';
        return 21;
    }
    auto extendedFrozen = sge::package::PackageReader::Read(extended.Value().packageBytes);
    if (!extendedFrozen) return 22;
    auto extendedViewResult = pkg::D3D12PackageView::Decode(extendedFrozen.Value());
    if (!extendedViewResult) return 23;
    const auto& extendedView = extendedViewResult.Value();
    if (extendedView.Resources().size() != view.Resources().size() + 2 ||
        extendedView.Views().size() != view.Views().size() + 2 ||
        Count(extendedView.FrameOperations(), pkg::D3D12OperationCode::ExecuteCopy) != 2)
        return 24;

    auto bindingGraph = AddRasterBinding(graph);
    auto bindingPackage = sge::compiler::d3d12::Compile(bindingGraph, profile);
    if (!bindingPackage)
    {
        std::cerr << bindingPackage.Error().stage << ": " << bindingPackage.Error().message << '\n';
        return 25;
    }
    auto bindingFrozen = sge::package::PackageReader::Read(bindingPackage.Value().packageBytes);
    if (!bindingFrozen) return 26;
    auto bindingViewResult = pkg::D3D12PackageView::Decode(bindingFrozen.Value());
    if (!bindingViewResult || bindingViewResult.Value().Views().size() != view.Views().size() + 1 ||
        bindingViewResult.Value().RootParameters().size() != view.RootParameters().size() + 1)
        return 27;

    auto multiQueueProfile = profile;
    multiQueueProfile.computeQueueCount = 1;
    auto multiQueue = sge::compiler::d3d12::Compile(graph, multiQueueProfile);
    if (!multiQueue) return 28;
    auto multiFrozen = sge::package::PackageReader::Read(multiQueue.Value().packageBytes);
    if (!multiFrozen) return 29;
    auto multiViewResult = pkg::D3D12PackageView::Decode(multiFrozen.Value());
    if (!multiViewResult) return 30;
    bool computeOnCompute = false;
    bool copyOnCopy = false;
    for (const auto& operation : multiViewResult.Value().FrameOperations())
    {
        if (operation.opcode == pkg::D3D12OperationCode::ExecuteCompute && operation.queue.value == 1)
            computeOnCompute = true;
        if (operation.opcode == pkg::D3D12OperationCode::ExecuteCopy && operation.queue.value == 2)
            copyOnCopy = true;
    }
    if (!computeOnCompute || !copyOnCopy ||
        Count(multiViewResult.Value().FrameOperations(), pkg::D3D12OperationCode::WaitQueue) < 2)
        return 31;

    auto invalidQueues = profile;
    invalidQueues.directQueueCount = 2;
    auto queueCapability = sge::compiler::d3d12::ValidateLevel2Capability(graph, invalidQueues);
    if (queueCapability || queueCapability.Error().stage != "target-feasibility") return 32;

    auto temporalSingleFrame = profile;
    temporalSingleFrame.framesInFlight = 1;
    if (sge::compiler::d3d12::ValidateLevel2Capability(graph, temporalSingleFrame)) return 33;

    auto multiSurfaceGraph = graph;
    auto extraSurface = *std::find_if(multiSurfaceGraph.resources.begin(), multiSurfaceGraph.resources.end(), [](const auto& resource) {
        return resource.kind == sem::ResourceKind::SurfaceImage;
    });
    extraSurface.id = {500};
    extraSurface.debugName = "ForbiddenSecondSurface";
    multiSurfaceGraph.resources.push_back(extraSurface);
    if (sge::compiler::d3d12::ValidateLevel2Capability(multiSurfaceGraph, profile)) return 34;

    auto multiMipGraph = graph;
    auto texture = std::find_if(multiMipGraph.resources.begin(), multiMipGraph.resources.end(), [](const auto& resource) {
        return resource.kind == sem::ResourceKind::Texture2D && resource.update == sem::UpdateIntent::Immutable;
    });
    texture->texture2D.mipLevels = 2;
    if (sge::compiler::d3d12::ValidateLevel2Capability(multiMipGraph, profile)) return 35;

    auto standalonePresent = graph;
    sem::Work presentWork;
    presentWork.id = {900};
    presentWork.kind = sem::WorkKind::Present;
    standalonePresent.works.push_back(presentWork);
    if (sge::compiler::d3d12::ValidateLevel2Capability(standalonePresent, profile)) return 36;

    auto registerMismatch = graph;
    auto mismatchedRaster = std::find_if(registerMismatch.programs.begin(), registerMismatch.programs.end(), [](const auto& program) {
        return program.kind == sem::ProgramKind::Raster;
    });
    mismatchedRaster->interface.parameters[1].shaderRegister = 7;
    auto rejectedRegister = sge::compiler::d3d12::Compile(registerMismatch, profile);
    if (rejectedRegister || rejectedRegister.Error().stage != "shader-reflection")
    {
        std::cerr << "ProgramInterface/HLSL register mismatch was not rejected by reflection\n";
        return 37;
    }

    auto vertexMismatch = graph;
    auto mismatchedVertexProgram = std::find_if(vertexMismatch.programs.begin(), vertexMismatch.programs.end(), [](const auto& program) {
        return program.kind == sem::ProgramKind::Raster;
    });
    mismatchedVertexProgram->interface.vertexInputs[0].componentCount = 2;
    auto rejectedVertex = sge::compiler::d3d12::Compile(vertexMismatch, profile);
    if (rejectedVertex || rejectedVertex.Error().stage != "shader-reflection")
    {
        std::cerr << "ProgramInterface/HLSL vertex-input mismatch was not rejected by reflection\n";
        return 38;
    }

    std::cout << "Stage-E typed pipeline, shader reflection, static completeness, capability rejection, and generic Package lowering tests passed.\n";
    return 0;
}
