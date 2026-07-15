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

    std::cout << "Slice 1-15 Stage-B execution passed: 18 general-Compiler Packages executed through one Package Runtime and D3D12 WARP Executor.\n";
    return 0;
}
