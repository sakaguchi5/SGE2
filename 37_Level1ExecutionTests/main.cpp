#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../09_PackageRuntime/PackageRuntime.h"
#include "../10_D3D12Executor/D3D12Executor.h"
#include "../11_PlatformWin32/Win32Window.h"
#include "../24_Level1Scenarios/Level1Scenarios.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

// Win32/COM headers define `interface` as a macro for COM declarations.
// SemanticModel intentionally uses `Program::interface` as a C++ member name,
// so remove the legacy macro after all platform headers have been included.
#ifdef interface
#undef interface
#endif

namespace
{
namespace lvl = sge::level1;

constexpr std::array<float, 16> Identity = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};
constexpr std::array<float, 4> ExternalColor = {0.96f, 0.88f, 0.72f, 1.0f};

bool HasQueueCompletion(
    const sge::runtime::FrameSubmission& submission,
    std::uint32_t queue)
{
    return std::any_of(submission.queues.begin(), submission.queues.end(),
        [=](const auto& completion) {
            return completion.queue == queue && completion.value != 0;
        });
}

int ValidateSubmission(
    const lvl::ScenarioInput& input,
    const sge::runtime::FrameSubmission& submission,
    std::uint64_t frameNumber,
    std::uint64_t expectedEpoch)
{
    const auto framesInFlight = input.targetProfile.framesInFlight;
    if (submission.deviceEpoch != expectedEpoch ||
        submission.framesInFlight != framesInFlight ||
        submission.frameSlot != frameNumber % framesInFlight)
    {
        std::cerr << input.name << ": frame " << frameNumber
                  << " submission identity metadata mismatch\n";
        return 1;
    }

    const std::uint32_t directQueue = 0;
    const std::uint32_t computeQueue = input.targetProfile.directQueueCount;
    const std::uint32_t copyQueue = input.targetProfile.directQueueCount +
                                    input.targetProfile.computeQueueCount;
    if (!HasQueueCompletion(submission, directQueue))
    {
        std::cerr << input.name << ": Direct queue completion is missing\n";
        return 2;
    }
    const bool hasComputeCompletion =
        input.targetProfile.computeQueueCount != 0 &&
        HasQueueCompletion(submission, computeQueue);
    if (input.expectations.dedicatedComputeQueue != hasComputeCompletion)
    {
        std::cerr << input.name << ": Compute queue completion does not match the Package topology\n";
        return 3;
    }
    const bool hasCopyCompletion =
        input.targetProfile.copyQueueCount != 0 &&
        HasQueueCompletion(submission, copyQueue);
    if (input.expectations.dedicatedCopyQueue != hasCopyCompletion)
    {
        std::cerr << input.name << ": Copy queue completion does not match the Package topology\n";
        return 4;
    }

    if (frameNumber >= framesInFlight && submission.reusedSlotFenceValue == 0)
    {
        std::cerr << input.name << ": reused Direct frame slot did not carry a fence dependency\n";
        return 5;
    }

    if (input.expectations.temporal)
    {
        const auto current = static_cast<std::uint32_t>(frameNumber % framesInFlight);
        const auto previous = (current + framesInFlight - 1u) % framesInFlight;
        if (submission.temporalCurrentInstance != current ||
            submission.temporalPreviousInstance != previous ||
            (frameNumber == 0 && submission.temporalDependencyFenceValue != 0) ||
            (frameNumber != 0 && submission.temporalDependencyFenceValue == 0))
        {
            std::cerr << input.name << ": Temporal submission metadata mismatch at frame "
                      << frameNumber << '\n';
            return 6;
        }
    }
    else if (submission.temporalDependencyFenceValue != 0)
    {
        std::cerr << input.name << ": non-Temporal Package reported a Temporal dependency\n";
        return 7;
    }

    const std::size_t expectedReleases = input.expectations.externalResource ? 1u : 0u;
    if (submission.releasedExternalResources.size() != expectedReleases)
    {
        std::cerr << input.name << ": External release count mismatch\n";
        return 8;
    }
    if (expectedReleases != 0 &&
        (!submission.releasedExternalResources[0].safeAfter ||
         submission.releasedExternalResources[0].slot != 0))
    {
        std::cerr << input.name << ": External release token is incomplete\n";
        return 9;
    }
    return 0;
}



sge::semantic::SemanticGraph BuildHeadlessComputeGraph()
{
    namespace sem = sge::semantic;
    sem::SemanticGraph graph;

    sem::Resource output;
    output.id = sem::ResourceId{0};
    output.debugName = "StageGHeadlessOutput";
    output.kind = sem::ResourceKind::Buffer;
    output.lifetime = sem::LifetimeIntent::Persistent;
    output.update = sem::UpdateIntent::GpuWritten;
    output.visibility = sem::Visibility::Internal;
    output.buffer = {16, 16};
    graph.resources.push_back(std::move(output));

    sem::ResourceUse outputUse;
    outputUse.id = sem::ResourceUseId{0};
    outputUse.resource = sem::ResourceId{0};
    outputUse.effect = sem::Effect::Write;
    outputUse.role = sem::ViewRole::StorageBuffer;
    outputUse.temporalRelation = sem::TemporalRelation::Current;
    graph.resourceUses.push_back(outputUse);

    sem::Program program;
    program.id = sem::ProgramId{0};
    program.debugName = "StageGHeadlessComputeProgram";
    program.kind = sem::ProgramKind::Compute;
    program.interface.parameters.push_back({
        sem::ProgramParameterId{0},
        "HeadlessOutput",
        sem::ProgramParameterKind::UnorderedBuffer,
        sem::ShaderStage::Compute,
        0,
        0,
        1});
    program.source.hlslSource = R"hlsl(
RWStructuredBuffer<float4> HeadlessOutput : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    HeadlessOutput[0] = float4(0.25f, 0.50f, 0.75f, 1.0f);
}
)hlsl";
    program.source.computeEntry = "CSMain";
    graph.programs.push_back(std::move(program));

    sem::Work work;
    work.id = sem::WorkId{0};
    work.debugName = "StageGHeadlessComputeWork";
    work.kind = sem::WorkKind::Compute;
    work.operands.push_back({
        sem::WorkOperandKind::ProgramParameter,
        sem::ResourceUseId{0},
        sem::ProgramParameterId{0}});
    work.compute.program = sem::ProgramId{0};
    work.compute.threadGroupCountX = 1;
    work.compute.threadGroupCountY = 1;
    work.compute.threadGroupCountZ = 1;
    graph.works.push_back(std::move(work));
    return graph;
}

int ExecuteHeadlessCompute()
{
    auto profile = sge::target::D3D12TargetProfile{};
    profile.computeQueueCount = 1;
    profile.copyQueueCount = 0;
    profile.surfaceImageCount = 0;
    profile.rtvDescriptorCount = 0;
    profile.dsvDescriptorCount = 0;
    profile.shaderDescriptorCount = 1;

    auto compiled = sge::compiler::d3d12::Compile(BuildHeadlessComputeGraph(), profile);
    if (!compiled)
    {
        std::cerr << "Stage-G headless compute Compile failed at "
                  << compiled.Error().stage << ": " << compiled.Error().message << '\n';
        return 1;
    }
    auto package = sge::package::PackageReader::Read(
        std::move(compiled).Value().packageBytes);
    if (!package)
    {
        std::cerr << "Stage-G headless PackageReader failed: "
                  << package.Error().message << '\n';
        return 2;
    }

    auto frozen = std::move(package).Value();
    auto view = sge::package::d3d12_v13::D3D12PackageView::Decode(frozen);
    if (!view || !view.Value().SurfaceSlots().empty() ||
        view.Value().Profile().surfaceImageCount != 0)
    {
        std::cerr << "Stage-G headless Package retained a Surface contract\n";
        return 3;
    }

    sge::d3d12::D3D12Executor executor({true, true});
    auto loaded = sge::runtime::LoadPackage(std::move(frozen), executor);
    if (!loaded)
    {
        std::cerr << "Stage-G headless Load failed at " << loaded.Error().stage
                  << ": " << loaded.Error().message << '\n';
        return 4;
    }

    sge::runtime::FrameInvocation invocation;
    for (std::uint64_t frameNumber = 0; frameNumber < 3; ++frameNumber)
    {
        invocation.frameNumber = frameNumber;
        auto submitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
        if (!submitted)
        {
            std::cerr << "Stage-G headless frame " << frameNumber
                      << " Submit failed at " << submitted.Error().stage
                      << ": " << submitted.Error().message << '\n';
            return 5;
        }
        if (submitted.Value().queues.size() != 1 ||
            submitted.Value().queues[0].queue != 1 ||
            submitted.Value().queues[0].value == 0 ||
            HasQueueCompletion(submitted.Value(), 0))
        {
            std::cerr << "Stage-G headless Package did not report only its Compute queue completion\n";
            return 6;
        }
        if (frameNumber >= profile.framesInFlight &&
            submitted.Value().reusedSlotFenceValue == 0)
        {
            std::cerr << "Stage-G headless Compute frame slot was reused without its Package queue fence\n";
            return 7;
        }
    }

    auto rebuilt = sge::runtime::RecoverDevice(
        loaded.Value(), executor,
        sge::runtime::DeviceRecoveryMode::ControlledRebuild);
    if (!rebuilt)
    {
        std::cerr << "Stage-G headless reconstruction failed at "
                  << rebuilt.Error().stage << ": " << rebuilt.Error().message << '\n';
        return 8;
    }
    if (!rebuilt.Value().packageObjectsRebuilt ||
        !rebuilt.Value().adapterReacquired ||
        rebuilt.Value().externalRebindRequired)
    {
        std::cerr << "Stage-G headless reconstruction report mismatch\n";
        return 9;
    }

    invocation.frameNumber = 0;
    auto resubmitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
    if (!resubmitted || !HasQueueCompletion(resubmitted.Value(), 1))
    {
        if (!resubmitted)
            std::cerr << "Stage-G post-recovery headless Submit failed at "
                      << resubmitted.Error().stage << ": " << resubmitted.Error().message << '\n';
        else
            std::cerr << "Stage-G post-recovery headless Compute completion is missing\n";
        return 10;
    }

    std::cout << "Stage-G headless Compute Package executed and reconstructed without a Surface host.\n";
    return 0;
}

int ValidateSurfaceHostRequirement()
{
    auto input = lvl::BuildScenario({lvl::Slice::Slice01FixedTriangle, lvl::Frontend::Neutral});
    if (!input)
    {
        std::cerr << "Stage-G Surface requirement scenario construction failed: "
                  << input.Error() << '\n';
        return 1;
    }
    auto compiled = sge::compiler::d3d12::Compile(
        input.Value().graph, input.Value().targetProfile);
    if (!compiled)
    {
        std::cerr << "Stage-G Surface requirement Compile failed at "
                  << compiled.Error().stage << ": " << compiled.Error().message << '\n';
        return 2;
    }
    auto package = sge::package::PackageReader::Read(
        std::move(compiled).Value().packageBytes);
    if (!package)
    {
        std::cerr << "Stage-G Surface requirement PackageReader failed: "
                  << package.Error().message << '\n';
        return 3;
    }

    sge::d3d12::D3D12Executor executor({true, true});
    auto loaded = sge::runtime::LoadPackage(std::move(package).Value(), executor);
    if (loaded || loaded.Error().stage != "surface")
    {
        std::cerr << "Stage-G Surface Package was not rejected without ISurfaceHost\n";
        return 4;
    }
    std::cout << "Stage-G Surface host requirement is derived from the Package Surface slot.\n";
    return 0;
}

int ExecuteScenario(
    lvl::ScenarioKey key,
    sge::runtime::ISurfaceHost& surface)
{
    auto inputResult = lvl::BuildScenario(key);
    if (!inputResult)
    {
        std::cerr << lvl::SliceName(key.slice) << ": " << inputResult.Error() << '\n';
        return 1;
    }
    auto input = std::move(inputResult).Value();

    auto compiled = sge::compiler::d3d12::Compile(input.graph, input.targetProfile);
    if (!compiled)
    {
        std::cerr << input.name << ": Compile failed at " << compiled.Error().stage
                  << ": " << compiled.Error().message << '\n';
        return 2;
    }
    auto compileOutput = std::move(compiled).Value();
    auto package = sge::package::PackageReader::Read(
        std::move(compileOutput.packageBytes));
    if (!package)
    {
        std::cerr << input.name << ": PackageReader rejected output: "
                  << package.Error().message << '\n';
        return 3;
    }

    sge::d3d12::D3D12Executor executor({true, true});
    auto loaded = sge::runtime::LoadPackage(
        std::move(package).Value(), executor, surface);
    if (!loaded)
    {
        std::cerr << input.name << ": Load failed at " << loaded.Error().stage
                  << ": " << loaded.Error().message << '\n';
        return 4;
    }

    const auto constantBytes = std::as_bytes(std::span<const float>(Identity));
    std::vector<sge::runtime::DynamicDataBinding> dynamicBindings;
    if (input.expectations.dynamicData)
        dynamicBindings.push_back({0, constantBytes});

    std::vector<sge::runtime::ExternalResourceBinding> externalBindings;
    if (input.expectations.externalResource)
    {
        auto external = executor.CreateExternalColorBuffer(
            loaded.Value().Instance(), ExternalColor);
        if (!external)
        {
            std::cerr << input.name << ": External creation failed at "
                      << external.Error().stage << ": " << external.Error().message << '\n';
            return 5;
        }
        externalBindings.push_back(
            {0, external.Value().resource, external.Value().availableAfter});
    }

    for (std::uint64_t frameNumber = 0; frameNumber < 3; ++frameNumber)
    {
        sge::runtime::FrameInvocation invocation;
        invocation.frameNumber = frameNumber;
        invocation.dynamicData = std::span<const sge::runtime::DynamicDataBinding>(dynamicBindings);
        invocation.externalResources = std::span<const sge::runtime::ExternalResourceBinding>(externalBindings);
        auto submitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
        if (!submitted)
        {
            std::cerr << input.name << ": frame " << frameNumber
                      << " Submit failed at " << submitted.Error().stage
                      << ": " << submitted.Error().message << '\n';
            return 6;
        }
        if (const auto validation = ValidateSubmission(
                input, submitted.Value(), frameNumber, 1);
            validation != 0)
            return 10 + validation;

        if (!externalBindings.empty())
            externalBindings[0].availableAfter =
                submitted.Value().releasedExternalResources[0].safeAfter;
    }

    if (input.expectations.recoveryContract)
    {
        auto controlled = sge::runtime::RecoverDevice(
            loaded.Value(), executor,
            sge::runtime::DeviceRecoveryMode::ControlledRebuild);
        if (!controlled)
        {
            std::cerr << input.name << ": Controlled reconstruction failed at "
                      << controlled.Error().stage << ": "
                      << controlled.Error().message << '\n';
            return 30;
        }
        const auto& report = controlled.Value();
        if (report.previousDeviceEpoch != 1 || report.newDeviceEpoch != 2 ||
            report.stateBefore != sge::runtime::DeviceRuntimeState::Active ||
            report.stateAfter != sge::runtime::DeviceRuntimeState::Active ||
            !report.adapterReacquired || !report.packageObjectsRebuilt ||
            !report.temporalHistoryReset || !report.externalRebindRequired)
        {
            std::cerr << input.name << ": Controlled reconstruction report mismatch\n";
            return 31;
        }

        sge::runtime::FrameInvocation stale;
        stale.frameNumber = 0;
        stale.dynamicData = std::span<const sge::runtime::DynamicDataBinding>(dynamicBindings);
        stale.externalResources = std::span<const sge::runtime::ExternalResourceBinding>(externalBindings);
        auto staleSubmit = sge::runtime::Submit(loaded.Value(), executor, stale);
        if (staleSubmit || staleSubmit.Error().stage != "invocation")
        {
            std::cerr << input.name << ": stale External binding was not rejected after recovery\n";
            return 32;
        }

        auto rebound = executor.CreateExternalColorBuffer(
            loaded.Value().Instance(), ExternalColor);
        if (!rebound)
        {
            std::cerr << input.name << ": External rebind creation failed at "
                      << rebound.Error().stage << ": " << rebound.Error().message << '\n';
            return 33;
        }
        externalBindings[0] =
            {0, rebound.Value().resource, rebound.Value().availableAfter};
        sge::runtime::FrameInvocation reboundInvocation;
        reboundInvocation.frameNumber = 0;
        reboundInvocation.dynamicData = std::span<const sge::runtime::DynamicDataBinding>(dynamicBindings);
        reboundInvocation.externalResources = std::span<const sge::runtime::ExternalResourceBinding>(externalBindings);
        auto reboundFrame = sge::runtime::Submit(
            loaded.Value(), executor, reboundInvocation);
        if (!reboundFrame)
        {
            std::cerr << input.name << ": post-recovery Submit failed at "
                      << reboundFrame.Error().stage << ": "
                      << reboundFrame.Error().message << '\n';
            return 34;
        }
        if (const auto validation = ValidateSubmission(
                input, reboundFrame.Value(), 0, 2);
            validation != 0)
            return 40 + validation;
    }

    std::cout << "  executed: " << input.name << '\n';
    return 0;
}
}

int main()
{
    if (const auto headless = ExecuteHeadlessCompute(); headless != 0)
        return 10 + headless;
    if (const auto surfaceContract = ValidateSurfaceHostRequirement(); surfaceContract != 0)
        return 30 + surfaceContract;

    auto window = sge::platform::Win32Window::Create(
        L"SGE2 Level-1 Stage-B WARP Qualification", 96, 96);
    if (!window)
    {
        std::cerr << "Stage-B window creation failed: " << window.Error() << '\n';
        return 1;
    }

    for (const auto key : lvl::StageAInputs())
    {
        const auto result = ExecuteScenario(key, *window.Value());
        if (result != 0) return 100 + result;
    }

    std::cout << "Slice 1-15 Stage-B execution and Stage-G headless Runtime qualification passed.\n";
    return 0;
}
