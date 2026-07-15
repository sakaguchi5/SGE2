#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../08_D3D12PackageSchema/D3D12Encoding.h"
#include "../24_Level1Scenarios/Level1Scenarios.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace
{
namespace sem = sge::semantic;
namespace pkg = sge::package::d3d12_v13;
namespace lvl = sge::level1;

std::size_t Count(
    std::span<const pkg::OperationView> operations,
    pkg::D3D12OperationCode code)
{
    return static_cast<std::size_t>(std::count_if(
        operations.begin(), operations.end(),
        [code](const auto& operation) { return operation.opcode == code; }));
}

bool HasResource(const sem::SemanticGraph& graph, const auto& predicate)
{
    return std::any_of(graph.resources.begin(), graph.resources.end(), predicate);
}

bool HasUse(const sem::SemanticGraph& graph, sem::ViewRole role)
{
    return std::any_of(graph.resourceUses.begin(), graph.resourceUses.end(),
        [role](const auto& use) { return use.role == role; });
}

bool HasWork(const sem::SemanticGraph& graph, sem::WorkKind kind)
{
    return std::any_of(graph.works.begin(), graph.works.end(),
        [kind](const auto& work) { return work.kind == kind; });
}

bool HasPackageResourceFlag(
    const pkg::D3D12PackageView& view,
    pkg::ResourceFlags flag)
{
    const auto bit = static_cast<std::uint32_t>(flag);
    return std::any_of(view.Resources().begin(), view.Resources().end(),
        [bit](const auto& resource) { return (resource.flags & bit) != 0; });
}

bool HasPackageViewClass(
    const pkg::D3D12PackageView& view,
    pkg::ViewClass viewClass)
{
    return std::any_of(view.Views().begin(), view.Views().end(),
        [viewClass](const auto& value) { return value.viewClass == viewClass; });
}

bool HasOperationOnQueue(
    std::span<const pkg::OperationView> operations,
    pkg::D3D12OperationCode code,
    std::uint32_t queue)
{
    return std::any_of(operations.begin(), operations.end(),
        [=](const auto& operation) {
            return operation.opcode == code && operation.queue.IsValid() &&
                   operation.queue.value == queue;
        });
}

bool HasTransitionAfterState(
    std::span<const pkg::OperationView> operations,
    std::uint32_t queue,
    pkg::ExplicitStateBits state)
{
    const auto expected = pkg::ResourceState{pkg::StateClass::Explicit, 0,
        static_cast<std::uint32_t>(state)};
    for (const auto& operation : operations)
    {
        if (operation.opcode != pkg::D3D12OperationCode::Transition ||
            !operation.queue.IsValid() || operation.queue.value != queue)
            continue;
        auto payload = pkg::DecodeTransition(operation.payload);
        if (payload && payload.Value().after == expected) return true;
    }
    return false;
}

int ValidateGraph(const lvl::ScenarioInput& input)
{
    const auto& graph = input.graph;
    const auto& expected = input.expectations;

    auto analyzed = sge::analysis::Analyze(graph);
    if (!analyzed)
    {
        std::cerr << input.name << ": SemanticAnalysis rejected the scenario";
        for (const auto& diagnostic : analyzed.Error())
            std::cerr << "; " << diagnostic.message;
        std::cerr << '\n';
        return 1;
    }

    const bool hasDynamic = HasResource(graph, [](const auto& resource) {
        return resource.update == sem::UpdateIntent::DynamicPerFrame;
    });
    const auto immutableBufferCount = static_cast<std::size_t>(std::count_if(
        graph.resources.begin(), graph.resources.end(), [](const auto& resource) {
            return resource.kind == sem::ResourceKind::Buffer &&
                   resource.update == sem::UpdateIntent::Immutable &&
                   resource.lifetime == sem::LifetimeIntent::Persistent;
        }));
    const bool hasTexture = HasResource(graph, [](const auto& resource) {
        return resource.kind == sem::ResourceKind::Texture2D &&
               resource.update == sem::UpdateIntent::Immutable;
    });
    const bool hasDepth = HasResource(graph, [](const auto& resource) {
        return resource.kind == sem::ResourceKind::Texture2D &&
               resource.texture2D.formatMeaning == sem::FormatMeaning::Depth32Float;
    });
    const bool hasGpuFrameLocal = HasResource(graph, [](const auto& resource) {
        return resource.lifetime == sem::LifetimeIntent::FrameLocal &&
               resource.update == sem::UpdateIntent::GpuWritten;
    });
    const bool hasTemporal = HasResource(graph, [](const auto& resource) {
        return resource.lifetime == sem::LifetimeIntent::Temporal;
    });
    const bool hasPreparation = HasResource(graph, [](const auto& resource) {
        return resource.lifetime == sem::LifetimeIntent::Preparation;
    });
    const bool hasExternal = HasResource(graph, [](const auto& resource) {
        return resource.lifetime == sem::LifetimeIntent::External &&
               resource.kind != sem::ResourceKind::SurfaceImage;
    });

    if (hasDynamic != expected.dynamicData ||
        (immutableBufferCount >= 2) != expected.extraBufferUpload ||
        hasTexture != expected.texture2D ||
        hasDepth != expected.depthAttachment ||
        HasWork(graph, sem::WorkKind::Compute) != expected.computeWork ||
        HasWork(graph, sem::WorkKind::Copy) != expected.copyWork ||
        hasGpuFrameLocal != expected.gpuWrittenFrameLocal ||
        hasTemporal != expected.temporal ||
        (hasPreparation && HasUse(graph, sem::ViewRole::AliasedBuffer)) != expected.aliasing ||
        hasExternal != expected.externalResource)
    {
        std::cerr << input.name << ": graph features do not match the Slice contract\n";
        return 2;
    }

    if ((input.targetProfile.computeQueueCount != 0) != expected.dedicatedComputeQueue ||
        (input.targetProfile.copyQueueCount != 0) != expected.dedicatedCopyQueue)
    {
        std::cerr << input.name << ": target queue topology does not match the Slice contract\n";
        return 3;
    }
    return 0;
}

int ValidatePackage(
    const lvl::ScenarioInput& input,
    const pkg::D3D12PackageView& view)
{
    const auto& expected = input.expectations;
    const auto& load = view.LoadOperations();
    const auto& frame = view.FrameOperations();

    if ((Count(frame, pkg::D3D12OperationCode::ApplyDynamicData) != 0) != expected.dynamicData ||
        (Count(load, pkg::D3D12OperationCode::UploadBuffer) >= 2) != expected.extraBufferUpload ||
        (Count(load, pkg::D3D12OperationCode::UploadTexture) != 0) != expected.texture2D ||
        HasPackageViewClass(view, pkg::ViewClass::DepthStencil) != expected.depthAttachment ||
        (Count(frame, pkg::D3D12OperationCode::ExecuteCompute) != 0) != expected.computeWork ||
        (Count(frame, pkg::D3D12OperationCode::ExecuteCopy) != 0) != expected.copyWork ||
        (!HasPackageResourceFlag(view, pkg::ResourceFlags::FrameLocal) && expected.gpuWrittenFrameLocal) ||
        HasPackageResourceFlag(view, pkg::ResourceFlags::Temporal) != expected.temporal ||
        (Count(load, pkg::D3D12OperationCode::ActivateAlias) != 0) != expected.aliasing ||
        (view.ExternalSlots().empty() == expected.externalResource) ||
        (Count(frame, pkg::D3D12OperationCode::AcquireExternal) != 0) != expected.externalResource ||
        (Count(frame, pkg::D3D12OperationCode::WaitExternal) != 0) != expected.externalResource ||
        (Count(frame, pkg::D3D12OperationCode::ReleaseExternal) != 0) != expected.externalResource)
    {
        std::cerr << input.name << ": Frozen Package features do not match the Slice contract\n";
        return 1;
    }

    if ((Count(frame, pkg::D3D12OperationCode::WaitTemporal) != 0) != expected.temporal)
    {
        std::cerr << input.name << ": Temporal wait contract mismatch\n";
        return 2;
    }

    if (expected.dedicatedCopyQueue)
    {
        const auto copyQueue = input.targetProfile.directQueueCount +
                               input.targetProfile.computeQueueCount;
        if (!HasOperationOnQueue(frame, pkg::D3D12OperationCode::ExecuteCopy, copyQueue) ||
            Count(frame, pkg::D3D12OperationCode::WaitQueue) == 0)
        {
            std::cerr << input.name << ": dedicated Copy queue/handoff was not fixed into the Package\n";
            return 3;
        }
    }

    if (expected.dedicatedComputeQueue)
    {
        const auto computeQueue = input.targetProfile.directQueueCount;
        if (!HasOperationOnQueue(frame, pkg::D3D12OperationCode::ExecuteCompute, computeQueue) ||
            Count(frame, pkg::D3D12OperationCode::WaitQueue) < 2)
        {
            std::cerr << input.name << ": multiple-queue handoffs were not fixed into the Package\n";
            return 4;
        }
        if (!HasTransitionAfterState(frame, 0, pkg::ExplicitStateBits::PixelShaderRead) ||
            (expected.temporal &&
             !HasTransitionAfterState(frame, computeQueue, pkg::ExplicitStateBits::NonPixelShaderRead)))
        {
            std::cerr << input.name << ": shader-read stage scope was not fixed into the Package\n";
            return 6;
        }
    }

    if (expected.recoveryContract)
    {
        bool recreate = false;
        bool rebind = false;
        bool temporal = false;
        for (const auto& resource : view.Resources())
        {
            recreate = recreate || resource.rebuildPolicy == pkg::RebuildPolicy::RecreateFromPackage;
            rebind = rebind || resource.rebuildPolicy == pkg::RebuildPolicy::RequireExternalRebind;
            temporal = temporal || (resource.flags & static_cast<std::uint32_t>(pkg::ResourceFlags::Temporal)) != 0;
        }
        if (!recreate || !rebind || !temporal)
        {
            std::cerr << input.name << ": Device-recovery Package contract is incomplete\n";
            return 5;
        }
    }
    return 0;
}
}

int main()
{
    std::map<std::tuple<std::uint32_t, std::uint16_t>, std::vector<std::byte>> packages;

    for (const auto key : lvl::StageAInputs())
    {
        auto inputResult = lvl::BuildScenario(key);
        if (!inputResult)
        {
            std::cerr << lvl::SliceName(key.slice) << ": " << inputResult.Error() << '\n';
            return 1;
        }
        auto input = std::move(inputResult).Value();

        if (const auto validation = ValidateGraph(input); validation != 0)
            return 10 + validation;

        auto first = sge::compiler::d3d12::Compile(input.graph, input.targetProfile);
        auto second = sge::compiler::d3d12::Compile(input.graph, input.targetProfile);
        if (!first || !second)
        {
            const auto& error = !first ? first.Error() : second.Error();
            std::cerr << input.name << ": " << error.stage << ": " << error.message << '\n';
            return 20;
        }
        if (first.Value().packageBytes != second.Value().packageBytes ||
            first.Value().executionDigestHex != second.Value().executionDigestHex)
        {
            std::cerr << input.name << ": general Compiler output is not deterministic\n";
            return 21;
        }

        auto frozen = sge::package::PackageReader::Read(first.Value().packageBytes);
        if (!frozen)
        {
            std::cerr << input.name << ": " << frozen.Error().message << '\n';
            return 22;
        }
        auto decoded = pkg::D3D12PackageView::Decode(frozen.Value());
        if (!decoded)
        {
            std::cerr << input.name << ": " << decoded.Error().message << '\n';
            return 23;
        }
        if (const auto validation = ValidatePackage(input, decoded.Value()); validation != 0)
            return 30 + validation;

        packages.emplace(
            std::tuple{static_cast<std::uint32_t>(key.slice), static_cast<std::uint16_t>(key.frontend)},
            first.Value().packageBytes);
        std::cout << "  passed: " << input.name << '\n';
    }

    const auto package = [&](lvl::Slice slice, lvl::Frontend frontend) -> const std::vector<std::byte>& {
        return packages.at(std::tuple{static_cast<std::uint32_t>(slice), static_cast<std::uint16_t>(frontend)});
    };

    if (package(lvl::Slice::Slice14ClassicalSdf, lvl::Frontend::Classical) !=
        package(lvl::Slice::Slice14ClassicalSdf, lvl::Frontend::Sdf))
    {
        std::cerr << "Slice 14 Classical and SDF inputs did not converge to byte-identical Packages\n";
        return 40;
    }

    const auto& slice15Classical = package(lvl::Slice::Slice15PgaFrontend, lvl::Frontend::Classical);
    if (slice15Classical != package(lvl::Slice::Slice15PgaFrontend, lvl::Frontend::Sdf) ||
        slice15Classical != package(lvl::Slice::Slice15PgaFrontend, lvl::Frontend::Pga))
    {
        std::cerr << "Slice 15 Classical, SDF, and PGA inputs did not converge to byte-identical Packages\n";
        return 41;
    }

    std::cout << "Slice 1-15 Stage-A reconstruction passed: 18 inputs used one general Compiler path.\n";
    return 0;
}
