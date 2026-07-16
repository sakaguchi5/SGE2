#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../08_D3D12PackageSchema/D3D12Encoding.h"
#include "../09_PackageRuntime/PackageRuntime.h"
#include "../10_D3D12Executor/D3D12Executor.h"
#include "../27_Level2RuntimeScenarios/RuntimeScenarios.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace k = sge::level2::runtime_scenarios;
namespace pkg = sge::package::d3d12_v13;

std::span<const std::byte> Bytes(const k::Float4& value)
{
    return std::as_bytes(std::span<const float>(value));
}

bool Near(const k::Float4& actual, const k::Float4& expected)
{
    for (std::size_t index = 0; index < actual.size(); ++index)
        if (std::abs(actual[index] - expected[index]) > 1.0e-5f)
            return false;
    return true;
}

k::Float4 DecodeFloat4(std::span<const std::byte> bytes)
{
    k::Float4 result{};
    if (bytes.size() >= sizeof(result)) std::memcpy(result.data(), bytes.data(), sizeof(result));
    return result;
}

void Print(const char* label, const k::Float4& value)
{
    std::cerr << label << " = [" << value[0] << ", " << value[1] << ", "
              << value[2] << ", " << value[3] << "]\n";
}

std::shared_ptr<sge::runtime::ICompletionToken> ReleaseToken(
    const sge::runtime::FrameSubmission& submission,
    std::uint32_t slot)
{
    const auto found = std::find_if(submission.releasedExternalResources.begin(),
        submission.releasedExternalResources.end(), [slot](const auto& release) {
            return release.slot == slot;
        });
    return found == submission.releasedExternalResources.end() ? nullptr : found->safeAfter;
}


bool HasQueueCompletion(const sge::runtime::FrameSubmission& submission, std::uint32_t queue)
{
    return std::any_of(submission.queues.begin(), submission.queues.end(),
        [queue](const auto& completion) { return completion.queue == queue && completion.value != 0; });
}

std::uint32_t Count(std::span<const pkg::OperationView> operations,
                    pkg::D3D12OperationCode code)
{
    return static_cast<std::uint32_t>(std::count_if(operations.begin(), operations.end(),
        [code](const auto& operation) { return operation.opcode == code; }));
}

struct CompiledScenario final
{
    k::Input input;
    sge::package::FrozenExecutablePackage package;
};

sge::base::Result<CompiledScenario, std::string> Compile(k::Scenario scenario, bool directFallback = false)
{
    auto built = k::Build(scenario);
    if (!built) return sge::base::Result<CompiledScenario, std::string>::Failure(built.Error());
    auto input = std::move(built).Value();
    if (directFallback)
    {
        input.targetProfile.computeQueueCount = 0;
        input.targetProfile.copyQueueCount = 0;
    }
    auto compiled = sge::compiler::d3d12::Compile(input.graph, input.targetProfile);
    if (!compiled)
        return sge::base::Result<CompiledScenario, std::string>::Failure(
            compiled.Error().stage + ": " + compiled.Error().message);
    auto frozen = sge::package::PackageReader::Read(std::move(compiled).Value().packageBytes);
    if (!frozen)
        return sge::base::Result<CompiledScenario, std::string>::Failure(frozen.Error().message);
    CompiledScenario result{std::move(input), std::move(frozen).Value()};
    return sge::base::Result<CompiledScenario, std::string>::Success(std::move(result));
}

int ValidatePackageContracts()
{
    for (const auto scenario : {k::Scenario::DynamicExternalPipeline, k::Scenario::CrossQueueTemporal})
    {
        auto compiled = Compile(scenario);
        if (!compiled)
        {
            std::cerr << "Stage-K Package Compile failed: " << compiled.Error() << '\n';
            return 1;
        }
        if (compiled.Value().package.Header().targetSchemaVersion != 17 ||
            compiled.Value().package.Header().minimumRuntimeVersion != 17)
        {
            std::cerr << "Stage-K Package did not use target schema/runtime 17\n";
            return 2;
        }
        auto view = pkg::D3D12PackageView::Decode(compiled.Value().package);
        if (!view)
        {
            std::cerr << "Stage-K Package decode failed: " << view.Error().message << '\n';
            return 2;
        }
        if (view.Value().DynamicSlots().size() != compiled.Value().input.dynamicSlotCount ||
            view.Value().ExternalSlots().size() != compiled.Value().input.externalSlotCount ||
            !view.Value().SurfaceSlots().empty())
        {
            std::cerr << compiled.Value().input.name << ": slot cardinality mismatch\n";
            return 3;
        }
        const auto& frame = view.Value().FrameOperations();
        bool temporalCopyUsesPreviousView = scenario != k::Scenario::CrossQueueTemporal;
        for (const auto& operation : frame)
        {
            if (operation.opcode != pkg::D3D12OperationCode::ExecuteCopy) continue;
            auto payload = pkg::DecodeCopyBuffer(operation.payload);
            if (!payload || operation.operationVersion != 2 ||
                payload.Value().sourceView.value >= view.Value().Views().size() ||
                payload.Value().destinationView.value >= view.Value().Views().size())
            {
                std::cerr << compiled.Value().input.name << ": ExecuteCopy v2 view contract is invalid\n";
                return 4;
            }
            const auto& sourceView = view.Value().Views()[payload.Value().sourceView.value];
            const auto& destinationView = view.Value().Views()[payload.Value().destinationView.value];
            if (sourceView.viewClass != pkg::ViewClass::CopySource ||
                destinationView.viewClass != pkg::ViewClass::CopyDestination)
                return 4;
            if ((sourceView.flags & static_cast<std::uint32_t>(pkg::ResourceViewFlags::TemporalPrevious)) != 0)
                temporalCopyUsesPreviousView = true;
        }
        if (!temporalCopyUsesPreviousView)
        {
            std::cerr << "Stage-K Temporal Copy did not freeze the Previous View identity\n";
            return 4;
        }
        if (Count(frame, pkg::D3D12OperationCode::AcquireExternal) != compiled.Value().input.externalSlotCount ||
            Count(frame, pkg::D3D12OperationCode::WaitExternal) != compiled.Value().input.externalSlotCount ||
            Count(frame, pkg::D3D12OperationCode::ReleaseExternal) != compiled.Value().input.externalSlotCount)
        {
            std::cerr << compiled.Value().input.name << ": external operation cardinality mismatch\n";
            return 4;
        }
        if (scenario == k::Scenario::DynamicExternalPipeline)
        {
            const auto uavViews = std::count_if(view.Value().Views().begin(), view.Value().Views().end(),
                [](const auto& artifact) { return artifact.viewClass == pkg::ViewClass::UnorderedAccess; });
            if (Count(frame, pkg::D3D12OperationCode::ExecuteCompute) != 2 ||
                Count(frame, pkg::D3D12OperationCode::ExecuteCopy) != 2 ||
                Count(frame, pkg::D3D12OperationCode::WaitQueue) != 3 || uavViews < 2)
            {
                std::cerr << "Stage-K interaction Package topology mismatch\n";
                return 5;
            }
        }
        else if (Count(frame, pkg::D3D12OperationCode::WaitTemporal) != 1 ||
                 Count(frame, pkg::D3D12OperationCode::ExecuteCopy) != 1 ||
                 Count(frame, pkg::D3D12OperationCode::ExecuteCompute) != 1)
        {
            std::cerr << "Stage-K Temporal Package topology mismatch\n";
            return 6;
        }
    }

    auto fallback = Compile(k::Scenario::DynamicExternalPipeline, true);
    if (!fallback)
    {
        std::cerr << "Stage-K Direct-fallback Package Compile failed: " << fallback.Error() << '\n';
        return 7;
    }
    auto fallbackView = pkg::D3D12PackageView::Decode(fallback.Value().package);
    if (!fallbackView || Count(fallbackView.Value().FrameOperations(),
            pkg::D3D12OperationCode::WaitQueue) != 0 ||
        fallbackView.Value().Profile().computeQueueCount != 0 ||
        fallbackView.Value().Profile().copyQueueCount != 0)
    {
        std::cerr << "Stage-K Direct-fallback Package retained dedicated-queue synchronization\n";
        return 8;
    }

    std::cout << "Stage-K Package interaction contracts passed.\n";
    return 0;
}

int ExpectInvocationFailure(sge::runtime::LoadedPackage& loaded,
                            sge::d3d12::D3D12Executor& executor,
                            const sge::runtime::FrameInvocation& invocation,
                            std::string_view label)
{
    auto submitted = sge::runtime::Submit(loaded, executor, invocation);
    if (submitted || submitted.Error().stage != "invocation")
    {
        std::cerr << "Stage-K invocation boundary did not reject " << label;
        if (!submitted) std::cerr << " at " << submitted.Error().stage << ": " << submitted.Error().message;
        std::cerr << '\n';
        return 1;
    }
    return 0;
}

int ExecuteDynamicExternalPipeline()
{
    auto compiled = Compile(k::Scenario::DynamicExternalPipeline);
    if (!compiled) { std::cerr << compiled.Error() << '\n'; return 1; }
    sge::d3d12::D3D12Executor executor({true, true});
    auto loaded = sge::runtime::LoadPackage(std::move(compiled).Value().package, executor);
    if (!loaded)
    {
        std::cerr << "Stage-K pipeline load failed at " << loaded.Error().stage
                  << ": " << loaded.Error().message << '\n';
        return 2;
    }

    const k::Float4 externalInput{0.5f, 1.0f, 1.5f, 2.0f};
    const k::Float4 zero{};
    auto input = executor.CreateExternalBuffer(loaded.Value().Instance(), 0, Bytes(externalInput));
    auto outputA = executor.CreateExternalBuffer(loaded.Value().Instance(), 1, Bytes(zero));
    auto outputB = executor.CreateExternalBuffer(loaded.Value().Instance(), 2, Bytes(zero));
    if (!input || !outputA || !outputB)
    {
        const auto& error = !input ? input.Error() : !outputA ? outputA.Error() : outputB.Error();
        std::cerr << "Stage-K external Buffer creation failed at " << error.stage << ": " << error.message << '\n';
        return 3;
    }

    const k::Float4 boundaryA{1.0f, 2.0f, 3.0f, 4.0f};
    const k::Float4 boundaryB{5.0f, 6.0f, 7.0f, 8.0f};
    const sge::runtime::DynamicDataBinding validDynamic[2] = {{0, Bytes(boundaryA)}, {1, Bytes(boundaryB)}};
    const sge::runtime::ExternalResourceBinding validExternal[3] = {
        {0, input.Value().resource, input.Value().availableAfter},
        {1, outputA.Value().resource, outputA.Value().availableAfter},
        {2, outputB.Value().resource, outputB.Value().availableAfter}};

    // Runtime boundary: each invalid invocation must fail before any Package operation executes.
    {
        sge::runtime::FrameInvocation invocation{0, std::span(validDynamic, 1), validExternal};
        if (ExpectInvocationFailure(loaded.Value(), executor, invocation, "missing dynamic slot")) return 4;
    }
    {
        const sge::runtime::DynamicDataBinding duplicate[2] = {{0, Bytes(boundaryA)}, {0, Bytes(boundaryB)}};
        sge::runtime::FrameInvocation invocation{0, duplicate, validExternal};
        if (ExpectInvocationFailure(loaded.Value(), executor, invocation, "duplicate dynamic slot")) return 5;
    }
    {
        const sge::runtime::DynamicDataBinding unknown[2] = {{0, Bytes(boundaryA)}, {99, Bytes(boundaryB)}};
        sge::runtime::FrameInvocation invocation{0, unknown, validExternal};
        if (ExpectInvocationFailure(loaded.Value(), executor, invocation, "unknown dynamic slot")) return 6;
    }
    {
        const auto shortBytes = std::as_bytes(std::span<const float>(boundaryA.data(), 1));
        const sge::runtime::DynamicDataBinding wrongSize[2] = {{0, shortBytes}, {1, Bytes(boundaryB)}};
        sge::runtime::FrameInvocation invocation{0, wrongSize, validExternal};
        if (ExpectInvocationFailure(loaded.Value(), executor, invocation, "dynamic byte-size mismatch")) return 7;
    }
    {
        sge::runtime::FrameInvocation invocation{0, validDynamic, std::span(validExternal, 2)};
        if (ExpectInvocationFailure(loaded.Value(), executor, invocation, "missing external slot")) return 8;
    }
    {
        const sge::runtime::ExternalResourceBinding duplicate[3] = {
            validExternal[0], {0, outputA.Value().resource, outputA.Value().availableAfter}, validExternal[2]};
        sge::runtime::FrameInvocation invocation{0, validDynamic, duplicate};
        if (ExpectInvocationFailure(loaded.Value(), executor, invocation, "duplicate external slot")) return 9;
    }
    {
        const sge::runtime::ExternalResourceBinding unknown[3] = {
            validExternal[0], validExternal[1], {99, outputB.Value().resource, outputB.Value().availableAfter}};
        sge::runtime::FrameInvocation invocation{0, validDynamic, unknown};
        if (ExpectInvocationFailure(loaded.Value(), executor, invocation, "unknown external slot")) return 10;
    }
    {
        const sge::runtime::ExternalResourceBinding wrongSlot[3] = {
            validExternal[0], {1, outputB.Value().resource, outputB.Value().availableAfter},
            {2, outputA.Value().resource, outputA.Value().availableAfter}};
        sge::runtime::FrameInvocation invocation{0, validDynamic, wrongSlot};
        if (ExpectInvocationFailure(loaded.Value(), executor, invocation, "slot-specific external resource mismatch")) return 11;
    }
    {
        const sge::runtime::ExternalResourceBinding wrongToken[3] = {
            validExternal[0],
            {1, outputA.Value().resource, outputB.Value().availableAfter},
            {2, outputB.Value().resource, outputA.Value().availableAfter}};
        sge::runtime::FrameInvocation invocation{0, validDynamic, wrongToken};
        if (ExpectInvocationFailure(loaded.Value(), executor, invocation, "slot-specific completion-token mismatch")) return 12;
    }

    std::array<k::Float4, 3> dynamicA = {
        k::Float4{1.0f, 2.0f, 3.0f, 4.0f},
        k::Float4{2.0f, 4.0f, 6.0f, 8.0f},
        k::Float4{3.0f, 6.0f, 9.0f, 12.0f}};
    std::array<k::Float4, 3> dynamicB = {
        k::Float4{0.25f, 0.5f, 0.75f, 1.0f},
        k::Float4{1.0f, 1.5f, 2.0f, 2.5f},
        k::Float4{2.0f, 2.5f, 3.0f, 3.5f}};

    auto inputBinding = input.Value();
    auto outputABinding = outputA.Value();
    auto outputBBinding = outputB.Value();
    for (std::uint64_t frame = 0; frame < 3; ++frame)
    {
        const sge::runtime::DynamicDataBinding dynamic[2] = {
            {0, Bytes(dynamicA[frame])}, {1, Bytes(dynamicB[frame])}};
        const sge::runtime::ExternalResourceBinding external[3] = {
            {0, inputBinding.resource, inputBinding.availableAfter},
            {1, outputABinding.resource, outputABinding.availableAfter},
            {2, outputBBinding.resource, outputBBinding.availableAfter}};
        sge::runtime::FrameInvocation invocation{frame, dynamic, external};
        auto submitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
        if (!submitted)
        {
            std::cerr << "Stage-K pipeline frame " << frame << " failed at " << submitted.Error().stage
                      << ": " << submitted.Error().message << '\n';
            return 13;
        }
        if (submitted.Value().releasedExternalResources.size() != 3 ||
            submitted.Value().queues.size() != 2 ||
            HasQueueCompletion(submitted.Value(), 0) ||
            !HasQueueCompletion(submitted.Value(), 1) ||
            !HasQueueCompletion(submitted.Value(), 2) ||
            (frame == 2 && submitted.Value().reusedSlotFenceValue == 0))
        {
            std::cerr << "Stage-K pipeline submission metadata mismatch\n";
            return 14;
        }
        inputBinding.availableAfter = ReleaseToken(submitted.Value(), 0);
        auto readA = executor.ReadExternalBuffer(loaded.Value().Instance(), outputABinding.resource,
                                                  ReleaseToken(submitted.Value(), 1));
        auto readB = executor.ReadExternalBuffer(loaded.Value().Instance(), outputBBinding.resource,
                                                  ReleaseToken(submitted.Value(), 2));
        if (!readA || !readB)
        {
            const auto& error = !readA ? readA.Error() : readB.Error();
            std::cerr << "Stage-K pipeline readback failed at " << error.stage << ": " << error.message << '\n';
            return 15;
        }
        const auto expected = k::PipelineExpected(dynamicA[frame], dynamicB[frame], externalInput);
        const auto actualA = DecodeFloat4(readA.Value().bytes);
        const auto actualB = DecodeFloat4(readB.Value().bytes);
        if (!Near(actualA, expected) || !Near(actualB, expected))
        {
            Print("expected", expected); Print("outputA", actualA); Print("outputB", actualB);
            return 16;
        }
        outputABinding.availableAfter = readA.Value().availableAfter;
        outputBBinding.availableAfter = readB.Value().availableAfter;
    }

    auto recovered = sge::runtime::RecoverDevice(loaded.Value(), executor,
        sge::runtime::DeviceRecoveryMode::ControlledRebuild);
    if (!recovered || recovered.Value().newDeviceEpoch != 2 ||
        !recovered.Value().packageObjectsRebuilt || !recovered.Value().externalRebindRequired)
    {
        if (!recovered) std::cerr << recovered.Error().stage << ": " << recovered.Error().message << '\n';
        return 17;
    }
    const sge::runtime::DynamicDataBinding staleDynamic[2] = {
        {0, Bytes(dynamicA[0])}, {1, Bytes(dynamicB[0])}};
    const sge::runtime::ExternalResourceBinding staleExternal[3] = {
        {0, inputBinding.resource, inputBinding.availableAfter},
        {1, outputABinding.resource, outputABinding.availableAfter},
        {2, outputBBinding.resource, outputBBinding.availableAfter}};
    sge::runtime::FrameInvocation stale{0, staleDynamic, staleExternal};
    if (ExpectInvocationFailure(loaded.Value(), executor, stale, "stale external epoch after reconstruction")) return 18;

    auto reboundInput = executor.CreateExternalBuffer(loaded.Value().Instance(), 0, Bytes(externalInput));
    auto reboundA = executor.CreateExternalBuffer(loaded.Value().Instance(), 1, Bytes(zero));
    auto reboundB = executor.CreateExternalBuffer(loaded.Value().Instance(), 2, Bytes(zero));
    if (!reboundInput || !reboundA || !reboundB) return 19;
    const sge::runtime::ExternalResourceBinding reboundExternal[3] = {
        {0, reboundInput.Value().resource, reboundInput.Value().availableAfter},
        {1, reboundA.Value().resource, reboundA.Value().availableAfter},
        {2, reboundB.Value().resource, reboundB.Value().availableAfter}};
    sge::runtime::FrameInvocation reboundInvocation{0, staleDynamic, reboundExternal};
    auto reboundSubmitted = sge::runtime::Submit(loaded.Value(), executor, reboundInvocation);
    if (!reboundSubmitted) return 20;
    auto reboundRead = executor.ReadExternalBuffer(loaded.Value().Instance(), reboundB.Value().resource,
        ReleaseToken(reboundSubmitted.Value(), 2));
    if (!reboundRead || !Near(DecodeFloat4(reboundRead.Value().bytes),
        k::PipelineExpected(dynamicA[0], dynamicB[0], externalInput)))
        return 21;

    std::cout << "Stage-K dynamic/external Compute-Copy values, slot boundaries, frame reuse, and reconstruction passed.\n";
    return 0;
}

int ExecuteDirectFallbackPipeline()
{
    auto compiled = Compile(k::Scenario::DynamicExternalPipeline, true);
    if (!compiled) { std::cerr << compiled.Error() << '\n'; return 1; }
    sge::d3d12::D3D12Executor executor({true, true});
    auto loaded = sge::runtime::LoadPackage(std::move(compiled).Value().package, executor);
    if (!loaded) return 2;

    const k::Float4 externalInput{0.75f, 1.25f, 1.75f, 2.25f};
    const k::Float4 dynamicA{2.0f, 4.0f, 6.0f, 8.0f};
    const k::Float4 dynamicB{1.0f, 1.5f, 2.0f, 2.5f};
    const k::Float4 zero{};
    auto input = executor.CreateExternalBuffer(loaded.Value().Instance(), 0, Bytes(externalInput));
    auto outputA = executor.CreateExternalBuffer(loaded.Value().Instance(), 1, Bytes(zero));
    auto outputB = executor.CreateExternalBuffer(loaded.Value().Instance(), 2, Bytes(zero));
    if (!input || !outputA || !outputB) return 3;

    const sge::runtime::DynamicDataBinding dynamic[2] = {
        {0, Bytes(dynamicA)}, {1, Bytes(dynamicB)}};
    const sge::runtime::ExternalResourceBinding external[3] = {
        {0, input.Value().resource, input.Value().availableAfter},
        {1, outputA.Value().resource, outputA.Value().availableAfter},
        {2, outputB.Value().resource, outputB.Value().availableAfter}};
    sge::runtime::FrameInvocation invocation{0, dynamic, external};
    auto submitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
    if (!submitted || submitted.Value().queues.size() != 1 ||
        !HasQueueCompletion(submitted.Value(), 0))
        return 4;
    auto readback = executor.ReadExternalBuffer(loaded.Value().Instance(), outputB.Value().resource,
        ReleaseToken(submitted.Value(), 2));
    if (!readback || !Near(DecodeFloat4(readback.Value().bytes),
        k::PipelineExpected(dynamicA, dynamicB, externalInput)))
        return 5;

    std::cout << "Stage-K Compute/Copy Direct-queue fallback produced the same semantic value.\n";
    return 0;
}

int ExecuteCrossQueueTemporal()
{
    auto compiled = Compile(k::Scenario::CrossQueueTemporal);
    if (!compiled) { std::cerr << compiled.Error() << '\n'; return 1; }
    sge::d3d12::D3D12Executor executor({true, true});
    auto loaded = sge::runtime::LoadPackage(std::move(compiled).Value().package, executor);
    if (!loaded)
    {
        std::cerr << "Stage-K Temporal load failed at " << loaded.Error().stage
                  << ": " << loaded.Error().message << '\n';
        return 2;
    }
    const k::Float4 zero{};
    auto output = executor.CreateExternalBuffer(loaded.Value().Instance(), 0, Bytes(zero));
    if (!output) return 3;
    auto outputBinding = output.Value();
    const std::array<k::Float4, 3> values = {
        k::Float4{1.0f, 2.0f, 3.0f, 4.0f},
        k::Float4{5.0f, 6.0f, 7.0f, 8.0f},
        k::Float4{9.0f, 10.0f, 11.0f, 12.0f}};

    for (std::uint64_t frame = 0; frame < values.size(); ++frame)
    {
        const sge::runtime::DynamicDataBinding dynamic{0, Bytes(values[frame])};
        const sge::runtime::ExternalResourceBinding external{
            0, outputBinding.resource, outputBinding.availableAfter};
        sge::runtime::FrameInvocation invocation;
        invocation.frameNumber = frame;
        invocation.dynamicData = std::span(&dynamic, 1);
        invocation.externalResources = std::span(&external, 1);
        auto submitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
        if (!submitted)
        {
            std::cerr << "Stage-K Temporal frame " << frame << " failed at " << submitted.Error().stage
                      << ": " << submitted.Error().message << '\n';
            return 4;
        }
        const auto expectedPrevious = frame == 0 ? k::TemporalSeed() : values[frame - 1];
        const auto current = static_cast<std::uint32_t>(frame % 2);
        const auto previous = (current + 1u) % 2u;
        if (submitted.Value().queues.size() != 2 ||
            HasQueueCompletion(submitted.Value(), 0) ||
            !HasQueueCompletion(submitted.Value(), 1) ||
            !HasQueueCompletion(submitted.Value(), 2) ||
            submitted.Value().temporalCurrentInstance != current ||
            submitted.Value().temporalPreviousInstance != previous ||
            (frame == 0 && submitted.Value().temporalDependencyFenceValue != 0) ||
            (frame != 0 && submitted.Value().temporalDependencyFenceValue == 0))
        {
            std::cerr << "Stage-K Temporal submission metadata mismatch\n";
            return 5;
        }
        auto readback = executor.ReadExternalBuffer(loaded.Value().Instance(), outputBinding.resource,
            ReleaseToken(submitted.Value(), 0));
        if (!readback)
        {
            std::cerr << "Stage-K Temporal readback failed at " << readback.Error().stage
                      << ": " << readback.Error().message << '\n';
            return 6;
        }
        const auto actual = DecodeFloat4(readback.Value().bytes);
        if (!Near(actual, expectedPrevious))
        {
            Print("expected previous", expectedPrevious); Print("actual previous", actual);
            return 7;
        }
        outputBinding.availableAfter = readback.Value().availableAfter;
    }

    // Temporal frame numbers are part of the Runtime contract.
    const sge::runtime::DynamicDataBinding skippedDynamic{0, Bytes(values[0])};
    const sge::runtime::ExternalResourceBinding skippedExternal{
        0, outputBinding.resource, outputBinding.availableAfter};
    sge::runtime::FrameInvocation skipped;
    skipped.frameNumber = 4;
    skipped.dynamicData = std::span(&skippedDynamic, 1);
    skipped.externalResources = std::span(&skippedExternal, 1);
    if (ExpectInvocationFailure(loaded.Value(), executor, skipped, "non-consecutive Temporal frame number")) return 8;

    auto recovered = sge::runtime::RecoverDevice(loaded.Value(), executor,
        sge::runtime::DeviceRecoveryMode::ControlledRebuild);
    if (!recovered || !recovered.Value().temporalHistoryReset ||
        !recovered.Value().externalRebindRequired || recovered.Value().newDeviceEpoch != 2)
        return 9;
    auto rebound = executor.CreateExternalBuffer(loaded.Value().Instance(), 0, Bytes(zero));
    if (!rebound) return 10;
    const k::Float4 recoveryValue{100.0f, 200.0f, 300.0f, 400.0f};
    const sge::runtime::DynamicDataBinding dynamic{0, Bytes(recoveryValue)};
    const sge::runtime::ExternalResourceBinding external{
        0, rebound.Value().resource, rebound.Value().availableAfter};
    sge::runtime::FrameInvocation invocation;
    invocation.frameNumber = 0;
    invocation.dynamicData = std::span(&dynamic, 1);
    invocation.externalResources = std::span(&external, 1);
    auto submitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
    if (!submitted || submitted.Value().temporalDependencyFenceValue != 0) return 11;
    auto readback = executor.ReadExternalBuffer(loaded.Value().Instance(), rebound.Value().resource,
        ReleaseToken(submitted.Value(), 0));
    if (!readback || !Near(DecodeFloat4(readback.Value().bytes), k::TemporalSeed())) return 12;

    std::cout << "Stage-K cross-queue Temporal values, previous/current instances, waits, and history reset passed.\n";
    return 0;
}
}

int main()
{
    std::cout << "Stage-K Runtime semantic correctness / interaction matrix...\n";
    if (const int result = ValidatePackageContracts(); result != 0) return 10 + result;
    if (const int result = ExecuteDynamicExternalPipeline(); result != 0) return 100 + result;
    if (const int result = ExecuteDirectFallbackPipeline(); result != 0) return 200 + result;
    if (const int result = ExecuteCrossQueueTemporal(); result != 0) return 300 + result;
    std::cout << "Stage-K Runtime semantic correctness and interaction matrix passed.\n";
    return 0;
}
