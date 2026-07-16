#include "../00_Base/Sha256.h"
#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../06_D3D12TargetCompiler/Level3Compiler.h"
#include "../13_Level3PlanVerifier/Level3PlanVerifier.h"
#include "../27_Level2RuntimeScenarios/RuntimeScenarios.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace compiler = sge::compiler::d3d12;
namespace l3c = sge::compiler::d3d12::level3_compiler;
namespace l3 = sge::level3;
namespace verify = sge::level3::verification;
namespace scenarios = sge::level2::runtime_scenarios;

struct TestLog final
{
    std::uint32_t passed = 0;
    std::uint32_t failed = 0;

    void Pass(std::string_view name)
    {
        ++passed;
        std::cout << "[PASS] " << name << '\n';
    }

    void Fail(std::string_view name, std::string_view message)
    {
        ++failed;
        std::cerr << "[FAIL] " << name << ": " << message << '\n';
    }

    void Require(bool condition, std::string_view name, std::string_view message)
    {
        if (condition) Pass(name); else Fail(name, message);
    }
};

sge::base::Digest256 ParseDigest(std::string_view text)
{
    sge::base::Digest256 result{};
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
        if (value >= 'A' && value <= 'F') return static_cast<std::uint8_t>(value - 'A' + 10);
        return 0;
    };
    for (std::size_t index = 0; index < result.size() && index * 2 + 1 < text.size(); ++index)
        result[index] = static_cast<std::byte>((nibble(text[index * 2]) << 4) | nibble(text[index * 2 + 1]));
    return result;
}

struct Fixture final
{
    scenarios::Input input;
    l3::SemanticObligation obligation;
    l3::D3D12PlanningContract contract;
    l3::ExecutionPlanIR safePlan;
    compiler::CompileOutput safePackage;
};

sge::base::Result<Fixture, std::string> BuildFixture()
{
    auto built = scenarios::Build(scenarios::Scenario::DynamicExternalPipeline);
    if (!built) return sge::base::Result<Fixture, std::string>::Failure(built.Error());

    Fixture fixture;
    fixture.input = std::move(built).Value();
    auto validated = compiler::ValidateSourceStage(fixture.input.graph, fixture.input.targetProfile);
    if (!validated) return sge::base::Result<Fixture, std::string>::Failure(
        validated.Error().stage + ": " + validated.Error().message);

    auto obligation = l3::BuildSemanticObligation(fixture.input.graph, validated.Value().analyzed);
    if (!obligation) return sge::base::Result<Fixture, std::string>::Failure(obligation.Error());
    fixture.obligation = std::move(obligation).Value();
    fixture.contract = l3::BuildPlanningContract(fixture.input.targetProfile);
    fixture.safePlan = l3c::BuildCanonicalSafePlan(fixture.obligation, fixture.contract);

    const auto report = verify::Verify(fixture.obligation, fixture.contract, fixture.safePlan);
    if (!report.verified) return sge::base::Result<Fixture, std::string>::Failure(
        "CanonicalSafePlan was rejected by the independent verifier");

    auto package = compiler::CompileSelectedPlan(
        fixture.input.graph, fixture.input.targetProfile, fixture.safePlan);
    if (!package) return sge::base::Result<Fixture, std::string>::Failure(
        package.Error().stage + ": " + package.Error().message);
    fixture.safePackage = std::move(package).Value();
    return sge::base::Result<Fixture, std::string>::Success(std::move(fixture));
}

void RequireRejectedOrFrozen(TestLog& log, const Fixture& fixture, std::string_view name,
                             l3::ExecutionPlanIR mutated)
{
    mutated.identity = l3::ComputePlanIdentity(mutated);
    const auto report = verify::Verify(fixture.obligation, fixture.contract, mutated);
    if (!report.verified)
    {
        log.Pass(name);
        return;
    }

    auto package = compiler::CompileSelectedPlan(
        fixture.input.graph, fixture.input.targetProfile, mutated);
    if (!package)
    {
        log.Fail(name, "Verifier accepted the Plan, but selected-Plan lowering rejected it");
        return;
    }
    if (package.Value().packageBytes == fixture.safePackage.packageBytes)
    {
        log.Fail(name, "accepted Plan field changed PlanIdentity but was ignored by Package lowering");
        return;
    }
    log.Pass(name);
}

void CheckVerifierGate(TestLog& log, const Fixture& fixture)
{
    auto corrupted = fixture.safePlan;
    corrupted.identity[0] ^= std::byte{0x01};
    const auto report = verify::Verify(fixture.obligation, fixture.contract, corrupted);
    if (report.verified)
    {
        log.Fail("raw-plan-lowering-requires-verifier", "identity-corrupted Plan unexpectedly passed verification");
        return;
    }

    const auto lowered = compiler::CompileSelectedPlan(
        fixture.input.graph, fixture.input.targetProfile, corrupted);
    log.Require(!lowered, "raw-plan-lowering-requires-verifier",
        "CompileSelectedPlan accepted a Plan that the independent verifier rejected");
}

void CheckBindingAuthority(TestLog& log, const Fixture& fixture)
{
    if (fixture.safePlan.bindings.empty())
    {
        log.Fail("binding-root-index-authority", "qualification fixture has no BindingPlan");
        log.Fail("binding-descriptor-index-authority", "qualification fixture has no BindingPlan");
        return;
    }

    auto root = fixture.safePlan;
    root.bindings.front().rootParameterIndex += 17;
    RequireRejectedOrFrozen(log, fixture, "binding-root-index-authority", std::move(root));

    const auto descriptor = std::find_if(fixture.safePlan.bindings.begin(), fixture.safePlan.bindings.end(),
        [](const auto& binding) { return binding.descriptorIndex != sge::base::InvalidIndex; });
    if (descriptor == fixture.safePlan.bindings.end())
    {
        log.Fail("binding-descriptor-index-authority", "qualification fixture has no descriptor-backed BindingPlan");
        return;
    }
    auto changed = fixture.safePlan;
    const auto index = static_cast<std::size_t>(std::distance(fixture.safePlan.bindings.begin(), descriptor));
    changed.bindings[index].descriptorIndex += 11;
    RequireRejectedOrFrozen(log, fixture, "binding-descriptor-index-authority", std::move(changed));
}

void CheckSynchronizationAuthority(TestLog& log, const Fixture& fixture)
{
    if (fixture.safePlan.synchronization.empty())
    {
        log.Fail("signal-point-authority", "qualification fixture has no cross-Queue synchronization");
        return;
    }
    auto changed = fixture.safePlan;
    changed.synchronization.front().signalPoint += 1000;
    RequireRejectedOrFrozen(log, fixture, "signal-point-authority", std::move(changed));
}

void CheckAllocationReferenceAuthority(TestLog& log, const Fixture& fixture)
{
    const auto resource = std::find_if(fixture.safePlan.resourceInstances.begin(),
        fixture.safePlan.resourceInstances.end(), [](const auto& item) {
            return item.allocation != sge::base::InvalidIndex;
        });
    if (resource == fixture.safePlan.resourceInstances.end())
    {
        log.Fail("resource-allocation-reference-authority", "qualification fixture has no Package-owned allocation reference");
        return;
    }
    auto changed = fixture.safePlan;
    const auto index = static_cast<std::size_t>(std::distance(fixture.safePlan.resourceInstances.begin(), resource));
    changed.resourceInstances[index].allocation += 1000;
    RequireRejectedOrFrozen(log, fixture, "resource-allocation-reference-authority", std::move(changed));
}

void CheckNegativeAndPositiveControls(TestLog& log, const Fixture& fixture)
{
    auto badState = fixture.safePlan;
    if (badState.useStates.empty())
    {
        log.Fail("invalid-state-negative-control", "qualification fixture has no state plan");
    }
    else
    {
        badState.useStates.front().required = badState.useStates.front().required == l3::AbstractState::Common ?
            l3::AbstractState::UnorderedWrite : l3::AbstractState::Common;
        badState.identity = l3::ComputePlanIdentity(badState);
        log.Require(!verify::Verify(fixture.obligation, fixture.contract, badState).verified,
            "invalid-state-negative-control", "Verifier accepted an invalid required state");
    }

    auto largerAllocation = fixture.safePlan;
    const auto allocation = std::find_if(largerAllocation.allocations.begin(), largerAllocation.allocations.end(),
        [](const auto& item) { return item.kind == l3::PlanAllocationKind::Committed && item.sizeBytes != 0; });
    if (allocation == largerAllocation.allocations.end())
    {
        log.Fail("allocation-field-positive-control", "qualification fixture has no non-empty Committed allocation");
    }
    else
    {
        allocation->sizeBytes += 65536;
        RequireRejectedOrFrozen(log, fixture, "allocation-field-positive-control", std::move(largerAllocation));
    }

    auto queuePlan = fixture.safePlan;
    const auto dedicated = std::find_if(queuePlan.queueAssignments.begin(), queuePlan.queueAssignments.end(),
        [](const auto& assignment) { return assignment.queueClass != l3::QueueClass::Direct; });
    if (dedicated == queuePlan.queueAssignments.end())
    {
        log.Fail("queue-field-positive-control", "qualification fixture has no dedicated Queue assignment");
    }
    else
    {
        dedicated->queueClass = l3::QueueClass::Direct;
        dedicated->queueIndex = 0;
        queuePlan.queueStrategy = l3::QueueStrategy::BoundedAlternative;
        // Synchronization is deliberately rebuilt here so this is a legal Queue alternative,
        // not merely an invalid missing-wait mutation.
        queuePlan.synchronization.clear();
        for (const auto& dependency : fixture.obligation.dependencies)
        {
            const auto producer = std::find_if(queuePlan.queueAssignments.begin(), queuePlan.queueAssignments.end(),
                [&](const auto& item) { return item.work == dependency.producer; });
            const auto consumer = std::find_if(queuePlan.queueAssignments.begin(), queuePlan.queueAssignments.end(),
                [&](const auto& item) { return item.work == dependency.consumer; });
            if (producer == queuePlan.queueAssignments.end() || consumer == queuePlan.queueAssignments.end()) continue;
            if (producer->queueClass == consumer->queueClass && producer->queueIndex == consumer->queueIndex) continue;
            const auto position = std::find(queuePlan.workSchedule.begin(), queuePlan.workSchedule.end(), dependency.producer);
            queuePlan.synchronization.push_back({dependency.producer, dependency.consumer,
                static_cast<std::uint32_t>(std::distance(queuePlan.workSchedule.begin(), position))});
        }
        RequireRejectedOrFrozen(log, fixture, "queue-field-positive-control", std::move(queuePlan));
    }
}

void CheckProfileProvenance(TestLog& log, const Fixture& fixture)
{
    l3::CompilerPolicy directPolicy;
    directPolicy.kind = l3::CompilerPolicyKind::MinimizeQueueHandoffs;
    auto direct = l3c::CompileLevel3(fixture.input.graph, fixture.input.targetProfile, directPolicy);
    l3::CompilerPolicy dedicatedPolicy;
    dedicatedPolicy.kind = l3::CompilerPolicyKind::PreferDedicatedQueues;
    auto dedicated = l3c::CompileLevel3(fixture.input.graph, fixture.input.targetProfile, dedicatedPolicy);
    if (!direct || !dedicated)
    {
        const auto* error = !direct ? &direct.Error() : &dedicated.Error();
        log.Fail("profile-fixture", error->stage + ": " + error->message);
        return;
    }

    l3::ProfileRecord profile;
    profile.planIdentity = dedicated.Value().selectedPlan.identity;
    profile.packageExecutionDigest = ParseDigest(dedicated.Value().selectedPackage.executionDigestHex);
    profile.targetProfileDigest = dedicated.Value().planningContract.digest;
    const std::string adapter = "WARP-Level3-Authority";
    const std::string scenario = "DynamicExternalPipeline-Authority";
    profile.adapterDriverFingerprint = sge::base::Sha256(std::as_bytes(std::span(adapter)));
    profile.measurementScenarioDigest = sge::base::Sha256(std::as_bytes(std::span(scenario)));
    profile.sampleCount = 8;
    profile.measuredNanoseconds = 1000;

    l3::ProfileSelectionContext context{profile.adapterDriverFingerprint, profile.measurementScenarioDigest, 4};
    l3::CompilerPolicy profilePolicy;
    profilePolicy.kind = l3::CompilerPolicyKind::ProfileGuided;

    auto selected = l3c::CompileLevel3(fixture.input.graph, fixture.input.targetProfile,
        profilePolicy, &profile, &context);
    log.Require(selected && selected.Value().selectedPlan.identity == profile.planIdentity,
        "profile-valid-reselection", "valid fixed Profile did not deterministically reselect its Plan");
    if (selected)
    {
        log.Require(selected.Value().selectedPackage.packageBytes != direct.Value().selectedPackage.packageBytes,
            "profile-creates-distinct-package", "Profile reselection did not replace the direct-policy Package");
    }

    const auto mustReject = [&](std::string_view name, l3::ProfileRecord candidate,
                                l3::ProfileSelectionContext candidateContext) {
        const auto result = l3c::CompileLevel3(fixture.input.graph, fixture.input.targetProfile,
            profilePolicy, &candidate, &candidateContext);
        log.Require(!result, name, "stale or mismatched Profile provenance was accepted");
    };

    auto stale = profile;
    stale.targetProfileDigest[0] ^= std::byte{0x01};
    mustReject("profile-rejects-contract-digest", stale, context);

    stale = profile;
    stale.planIdentity[0] ^= std::byte{0x01};
    mustReject("profile-rejects-plan-identity", stale, context);

    stale = profile;
    stale.packageExecutionDigest[0] ^= std::byte{0x01};
    mustReject("profile-rejects-package-digest", stale, context);

    auto wrongAdapter = context;
    wrongAdapter.adapterDriverFingerprint[0] ^= std::byte{0x01};
    mustReject("profile-rejects-adapter-fingerprint", profile, wrongAdapter);

    auto wrongScenario = context;
    wrongScenario.measurementScenarioDigest[0] ^= std::byte{0x01};
    mustReject("profile-rejects-scenario-digest", profile, wrongScenario);

    stale = profile;
    stale.sampleCount = context.minimumSampleCount - 1;
    mustReject("profile-rejects-insufficient-samples", stale, context);
}
}

int main()
{
    auto fixtureResult = BuildFixture();
    if (!fixtureResult)
    {
        std::cerr << "Failed to construct Level 3 authority fixture: " << fixtureResult.Error() << '\n';
        return 2;
    }
    const auto fixture = std::move(fixtureResult).Value();
    TestLog log;

    CheckVerifierGate(log, fixture);
    CheckBindingAuthority(log, fixture);
    CheckSynchronizationAuthority(log, fixture);
    CheckAllocationReferenceAuthority(log, fixture);
    CheckNegativeAndPositiveControls(log, fixture);
    CheckProfileProvenance(log, fixture);

    std::cout << "\nAuthority tests passed: " << log.passed << ", failed: " << log.failed << '\n';
    if (log.failed != 0)
    {
        std::cerr << "SGE2 LEVEL 3 DECISIVE AUTHORITY TESTS FAILED\n";
        std::cerr << "A true Level 3 freeze requires every accepted Plan field to be authoritative,\n"
                     "and Package lowering must be unreachable for verifier-rejected Plans.\n";
        return 1;
    }

    std::cout << "SGE2 LEVEL 3 DECISIVE AUTHORITY TESTS PASSED\n";
    std::cout << "The verified ExecutionPlanIR is authoritative for all currently represented decisions.\n";
    return 0;
}
