#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../06_D3D12TargetCompiler/Level3Compiler.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../09_PackageRuntime/PackageRuntime.h"
#include "../10_D3D12Executor/D3D12Executor.h"
#include "../13_Level3PlanVerifier/Level3PlanVerifier.h"
#include "../27_Level2RuntimeScenarios/RuntimeScenarios.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

namespace
{
namespace k = sge::level2::runtime_scenarios;
namespace compiler = sge::compiler::d3d12;
namespace l3c = sge::compiler::d3d12::level3_compiler;
namespace l3 = sge::level3;

std::span<const std::byte> Bytes(const k::Float4& value) { return std::as_bytes(std::span<const float>(value)); }

k::Float4 Decode(std::span<const std::byte> bytes)
{
    k::Float4 result{};
    if (bytes.size() >= sizeof(result)) std::memcpy(result.data(), bytes.data(), sizeof(result));
    return result;
}

bool Near(const k::Float4& left, const k::Float4& right)
{
    for (std::size_t index = 0; index < left.size(); ++index)
        if (std::abs(left[index] - right[index]) > 1.0e-5f) return false;
    return true;
}

std::shared_ptr<sge::runtime::ICompletionToken> ReleaseToken(
    const sge::runtime::FrameSubmission& submission, std::uint32_t slot)
{
    const auto found = std::find_if(submission.releasedExternalResources.begin(),
        submission.releasedExternalResources.end(), [slot](const auto& release) { return release.slot == slot; });
    return found == submission.releasedExternalResources.end() ? nullptr : found->safeAfter;
}

sge::base::Result<k::Float4, std::string> Execute(
    const k::Input& input, const l3::ExecutionPlanIR& plan, bool recover)
{
    auto compiled = compiler::CompileSelectedPlan(input.graph, input.targetProfile, plan);
    if (!compiled) return sge::base::Result<k::Float4, std::string>::Failure(
        compiled.Error().stage + ": " + compiled.Error().message);
    auto package = sge::package::PackageReader::Read(std::move(compiled).Value().packageBytes);
    if (!package) return sge::base::Result<k::Float4, std::string>::Failure(package.Error().message);
    sge::d3d12::D3D12Executor executor({true, true});
    auto loaded = sge::runtime::LoadPackage(std::move(package).Value(), executor);
    if (!loaded) return sge::base::Result<k::Float4, std::string>::Failure(
        loaded.Error().stage + ": " + loaded.Error().message);

    if (recover)
    {
        auto recovered = sge::runtime::RecoverDevice(loaded.Value(), executor,
            sge::runtime::DeviceRecoveryMode::ControlledRebuild);
        if (!recovered || !recovered.Value().packageObjectsRebuilt || !recovered.Value().externalRebindRequired)
            return sge::base::Result<k::Float4, std::string>::Failure("selected Package reconstruction failed");
    }

    const k::Float4 externalInput{0.5f, 1.0f, 1.5f, 2.0f};
    const k::Float4 dynamicA{2.0f, 4.0f, 6.0f, 8.0f};
    const k::Float4 dynamicB{1.0f, 1.5f, 2.0f, 2.5f};
    const k::Float4 zero{};
    auto inputBuffer = executor.CreateExternalBuffer(loaded.Value().Instance(), 0, Bytes(externalInput));
    auto outputA = executor.CreateExternalBuffer(loaded.Value().Instance(), 1, Bytes(zero));
    auto outputB = executor.CreateExternalBuffer(loaded.Value().Instance(), 2, Bytes(zero));
    if (!inputBuffer || !outputA || !outputB)
        return sge::base::Result<k::Float4, std::string>::Failure("external Buffer creation failed");
    const sge::runtime::DynamicDataBinding dynamic[2] = {{0, Bytes(dynamicA)}, {1, Bytes(dynamicB)}};
    const sge::runtime::ExternalResourceBinding external[3] = {
        {0, inputBuffer.Value().resource, inputBuffer.Value().availableAfter},
        {1, outputA.Value().resource, outputA.Value().availableAfter},
        {2, outputB.Value().resource, outputB.Value().availableAfter}};
    sge::runtime::FrameInvocation invocation{0, dynamic, external};
    auto submitted = sge::runtime::Submit(loaded.Value(), executor, invocation);
    if (!submitted) return sge::base::Result<k::Float4, std::string>::Failure(
        submitted.Error().stage + ": " + submitted.Error().message);
    auto readback = executor.ReadExternalBuffer(loaded.Value().Instance(), outputB.Value().resource,
        ReleaseToken(submitted.Value(), 2));
    if (!readback) return sge::base::Result<k::Float4, std::string>::Failure(
        readback.Error().stage + ": " + readback.Error().message);
    const auto actual = Decode(readback.Value().bytes);
    const auto expected = k::PipelineExpected(dynamicA, dynamicB, externalInput);
    if (!Near(actual, expected)) return sge::base::Result<k::Float4, std::string>::Failure(
        "GPU observation differs from the Semantic reference");
    return sge::base::Result<k::Float4, std::string>::Success(actual);
}
}

int main()
{
    auto built = k::Build(k::Scenario::DynamicExternalPipeline);
    if (!built) { std::cerr << built.Error() << '\n'; return 1; }
    auto input = std::move(built).Value();
    auto validated = compiler::ValidateSourceStage(input.graph, input.targetProfile);
    if (!validated) return 2;
    auto obligation = l3::BuildSemanticObligation(input.graph, validated.Value().analyzed);
    if (!obligation) return 2;
    const auto contract = l3::BuildPlanningContract(input.targetProfile);
    l3::CompilerPolicy policy;
    auto plans = l3c::GenerateCandidatePlans(obligation.Value(), contract, policy);

    std::vector<k::Float4> observations;
    bool scheduleAlternative = false;
    bool allDirect = false;
    bool dedicated = false;
    bool recovered = false;
    for (const auto& plan : plans)
    {
        const auto report = l3::verification::Verify(obligation.Value(), contract, plan);
        if (!report.verified) continue;
        scheduleAlternative |= plan.scheduleStrategy != l3::ScheduleStrategy::CanonicalMinimumId;
        allDirect |= plan.queueStrategy == l3::QueueStrategy::AllDirect;
        dedicated |= plan.queueStrategy == l3::QueueStrategy::CanonicalSafe ||
                     plan.queueStrategy == l3::QueueStrategy::KindPreferredDedicated;
        auto observed = Execute(input, plan, !recovered);
        if (!observed)
        {
            std::cerr << "verified Plan execution failed: " << observed.Error() << '\n';
            return 3;
        }
        recovered = true;
        observations.push_back(observed.Value());
    }
    if (observations.size() < 2 || !scheduleAlternative || !allDirect || !dedicated || !recovered)
    {
        std::cerr << "runtime candidate corpus did not cover schedule/Queue/reconstruction axes\n";
        return 4;
    }
    if (!std::all_of(observations.begin(), observations.end(), [&](const auto& value) {
        return Near(value, observations.front());
    }))
    {
        std::cerr << "verified Plans produced different GPU observations\n";
        return 5;
    }
    std::cout << "Level 3 verified schedule and Queue Packages produced identical WARP observations.\n";
    std::cout << "Selected Package reconstruction preserved the observation contract.\n";
    return 0;
}
