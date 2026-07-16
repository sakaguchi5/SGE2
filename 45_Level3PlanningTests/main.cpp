#include "../00_Base/Sha256.h"
#include "../00_Base/FileIO.h"
#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../06_D3D12TargetCompiler/Level3Compiler.h"
#include "../13_Level3PlanVerifier/Level3PlanVerifier.h"
#include "../24_Level1Scenarios/Level1Scenarios.h"
#include "../25_Level2Scenarios/Level2Scenarios.h"
#include "../26_Level2GeneratedGraphs/GeneratedGraphs.h"
#include "../27_Level2RuntimeScenarios/RuntimeScenarios.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace compiler = sge::compiler::d3d12;
namespace l3c = sge::compiler::d3d12::level3_compiler;
namespace l3 = sge::level3;
namespace verify = sge::level3::verification;
namespace generated = sge::level2::generated;

sge::base::Digest256 ParseDigest(std::string_view text)
{
    sge::base::Digest256 result{};
    const auto nibble = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<std::uint8_t>(value - 'a' + 10);
        return static_cast<std::uint8_t>(value - 'A' + 10);
    };
    for (std::size_t index = 0; index < result.size() && index * 2 + 1 < text.size(); ++index)
        result[index] = static_cast<std::byte>((nibble(text[index * 2]) << 4) | nibble(text[index * 2 + 1]));
    return result;
}

struct Case final
{
    std::string name;
    sge::semantic::SemanticGraph graph;
    sge::target::D3D12TargetProfile profile;
};

sge::semantic::SemanticGraph BuildHeadlessGraph()
{
    namespace sem = sge::semantic;
    sem::SemanticGraph graph;
    sem::Resource output;
    output.id = {0}; output.kind = sem::ResourceKind::Buffer;
    output.lifetime = sem::LifetimeIntent::Persistent; output.update = sem::UpdateIntent::GpuWritten;
    output.buffer = {16, 16}; graph.resources.push_back(output);
    graph.resourceUses.push_back({{0}, {0}, sem::Effect::Write, sem::ViewRole::StorageBuffer, sem::TemporalRelation::Current});
    sem::Program program;
    program.id = {0}; program.kind = sem::ProgramKind::Compute;
    program.interface.parameters.push_back({{0}, "Level3HeadlessOutput",
        sem::ProgramParameterKind::UnorderedBuffer, sem::ShaderStage::Compute, 0, 0, 1});
    program.source.hlslSource = R"hlsl(
RWStructuredBuffer<float4> Level3HeadlessOutput : register(u0);
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) { Level3HeadlessOutput[0] = float4(0.25f, 0.50f, 0.75f, 1.0f); }
)hlsl";
    program.source.computeEntry = "CSMain"; graph.programs.push_back(std::move(program));
    sem::Work work;
    work.id = {0}; work.kind = sem::WorkKind::Compute;
    work.operands.push_back({sem::WorkOperandKind::ProgramParameter, {0}, {0}});
    work.compute.program = {0}; graph.works.push_back(std::move(work));
    return graph;
}

generated::GeneratedSpec ExplicitSpec(std::string name, std::vector<generated::NodeKind> kinds,
                                      std::initializer_list<generated::Edge> edges, std::uint64_t seed)
{
    generated::GeneratedSpec spec;
    spec.name = std::move(name); spec.kinds = std::move(kinds);
    spec.edges.assign(edges.begin(), edges.end()); spec.seed = seed;
    return spec;
}

std::vector<generated::GeneratedSpec> FrozenGeneratedSpecs()
{
    using K = generated::NodeKind;
    std::vector<generated::GeneratedSpec> specs;
    specs.push_back(ExplicitSpec("StageJ.SingleCompute", {K::Compute}, {}, 0x101));
    specs.push_back(ExplicitSpec("StageJ.IndependentMixed", {K::Compute,K::Copy,K::Compute,K::Copy}, {}, 0x102));
    specs.push_back(ExplicitSpec("StageJ.MixedChain", {K::Compute,K::Copy,K::Compute,K::Copy,K::Compute}, {{0,1},{1,2},{2,3},{3,4}}, 0x103));
    specs.push_back(ExplicitSpec("StageJ.FanOut", {K::Compute,K::Copy,K::Compute,K::Copy,K::Compute}, {{0,1},{0,2},{0,3},{0,4}}, 0x104));
    specs.push_back(ExplicitSpec("StageJ.FanInLatestProducer", {K::Compute,K::Compute,K::Compute,K::Copy,K::Compute}, {{0,3},{1,3},{2,3},{3,4}}, 0x105));
    specs.push_back(ExplicitSpec("StageJ.Diamond", {K::Copy,K::Compute,K::Copy,K::Compute}, {{0,1},{0,2},{1,3},{2,3}}, 0x106));
    specs.push_back(ExplicitSpec("StageJ.DisconnectedComponents", {K::Compute,K::Copy,K::Compute,K::Copy,K::Compute,K::Copy}, {{0,1},{1,2},{3,4},{4,5}}, 0x107));
    specs.push_back(ExplicitSpec("StageJ.Layered", {K::Compute,K::Copy,K::Compute,K::Copy,K::Compute,K::Copy}, {{0,2},{0,3},{1,2},{1,3},{2,4},{2,5},{3,4},{3,5}}, 0x108));
    specs.push_back(ExplicitSpec("StageJ.DenseTransitive", {K::Compute,K::Copy,K::Compute,K::Copy,K::Compute,K::Copy},
        {{0,1},{0,2},{0,3},{0,4},{0,5},{1,2},{1,3},{1,4},{1,5},{2,3},{2,4},{2,5},{3,4},{3,5},{4,5}}, 0x109));
    constexpr std::uint64_t masks[] = {0x0000,0x0001,0x0003,0x0015,0x002A,0x0055,0x00AA,0x0123,
        0x0249,0x0492,0x0924,0x1249,0x1555,0x2AAA,0x5A5A,0x7FFF};
    for (std::size_t index = 0; index < std::size(masks); ++index)
        specs.push_back(generated::MakeForwardMaskSpec("StageJ.SampleMask." + std::to_string(index), 6,
            masks[index], 0xC6BC279692B5CC83ull ^ masks[index], true));
    specs.push_back(generated::MakeSeededSpec("StageJ.Seed.10.2611923443488327891", 10, 0x243F6A8885A308D3ull, 28, true));
    specs.push_back(generated::MakeSeededSpec("StageJ.Seed.25.1376283091369227076", 25, 0x13198A2E03707344ull, 18, true));
    specs.push_back(generated::MakeSeededSpec("StageJ.Seed.50.11820040416388919760", 50, 0xA4093822299F31D0ull, 12, true));
    specs.push_back(generated::MakeSeededSpec("StageJ.Seed.100.589684135938649225", 100, 0x082EFA98EC4E6C89ull, 8, true));
    return specs;
}

std::vector<Case> BuildCompatibilityCorpus()
{
    std::vector<Case> cases;
    for (const auto key : sge::level1::StageAInputs())
    {
        auto built = sge::level1::BuildScenario(key);
        if (built) cases.push_back({"Level1/" + built.Value().name,
            std::move(built.Value().graph), built.Value().targetProfile});
    }
    for (const auto key : sge::level2::StageHInputs())
    {
        auto built = sge::level2::BuildStageHScenario(key);
        if (built) cases.push_back({"StageH/" + built.Value().name,
            std::move(built.Value().graph), built.Value().targetProfile});
    }
    sge::target::D3D12TargetProfile headless;
    headless.computeQueueCount = 1; headless.copyQueueCount = 0; headless.surfaceImageCount = 0;
    headless.rtvDescriptorCount = 0; headless.dsvDescriptorCount = 0; headless.shaderDescriptorCount = 1;
    cases.push_back({"StageG/HeadlessCompute", BuildHeadlessGraph(), headless});
    for (const auto scenario : {sge::level2::runtime_scenarios::Scenario::DynamicExternalPipeline,
                                sge::level2::runtime_scenarios::Scenario::CrossQueueTemporal})
    {
        auto built = sge::level2::runtime_scenarios::Build(scenario);
        if (built) cases.push_back({"StageK/" + built.Value().name,
            std::move(built.Value().graph), built.Value().targetProfile});
    }
    auto fallback = sge::level2::runtime_scenarios::Build(
        sge::level2::runtime_scenarios::Scenario::DynamicExternalPipeline);
    if (fallback)
    {
        fallback.Value().targetProfile.computeQueueCount = 0;
        fallback.Value().targetProfile.copyQueueCount = 0;
        cases.push_back({"StageK/DynamicExternalDirectFallback", std::move(fallback.Value().graph), fallback.Value().targetProfile});
    }
    for (auto spec : FrozenGeneratedSpecs())
    {
        auto built = generated::BuildGeneratedCase(std::move(spec));
        if (built) cases.push_back({"Generated/" + built.Value().spec.name,
            std::move(built.Value().graph), built.Value().targetProfile});
    }
    return cases;
}

int CheckCanonicalCompatibility(const std::vector<Case>& corpus)
{
    for (const auto& item : corpus)
    {
        auto oldPackage = compiler::Compile(item.graph, item.profile);
        auto validated = compiler::ValidateSourceStage(item.graph, item.profile);
        if (!oldPackage || !validated)
        {
            std::cerr << item.name << ": Level 2 compile/validation failed\n";
            return 1;
        }
        auto obligation = l3::BuildSemanticObligation(item.graph, validated.Value().analyzed);
        if (!obligation)
        {
            std::cerr << item.name << ": obligation failed: " << obligation.Error() << '\n';
            return 2;
        }
        const auto contract = l3::BuildPlanningContract(item.profile);
        const auto safe = l3c::BuildCanonicalSafePlan(obligation.Value(), contract);
        const auto report = verify::Verify(obligation.Value(), contract, safe);
        if (!report.verified)
        {
            std::cerr << item.name << ": CanonicalSafe verifier rejected with "
                      << report.violations.size() << " diagnostics\n";
            if (!report.violations.empty()) std::cerr << report.violations.front().message << '\n';
            return 3;
        }
        auto newPackage = compiler::CompileSelectedPlan(item.graph, item.profile, safe);
        if (!newPackage)
        {
            std::cerr << item.name << ": selected Plan lowering failed at "
                      << newPackage.Error().stage << ": " << newPackage.Error().message << '\n';
            return 4;
        }
        if (oldPackage.Value().packageBytes != newPackage.Value().packageBytes ||
            oldPackage.Value().executionDigestHex != newPackage.Value().executionDigestHex)
        {
            std::cerr << item.name << ": CanonicalSafe Package bytes differ from Level 2\n";
            return 5;
        }
    }
    return 0;
}

int CheckAdversarialVerifier(const Case& item)
{
    auto validated = compiler::ValidateSourceStage(item.graph, item.profile);
    if (!validated) return 10;
    auto obligation = l3::BuildSemanticObligation(item.graph, validated.Value().analyzed);
    if (!obligation) return 10;
    const auto contract = l3::BuildPlanningContract(item.profile);
    const auto safe = l3c::BuildCanonicalSafePlan(obligation.Value(), contract);
    std::vector<l3::ExecutionPlanIR> invalid;
    const auto mutate = [&](auto function) {
        auto plan = safe; function(plan); plan.identity = l3::ComputePlanIdentity(plan); invalid.push_back(std::move(plan));
    };
    mutate([](auto& plan) { plan.workSchedule.pop_back(); });
    if (!obligation.Value().dependencies.empty())
    {
        auto plan = safe;
        const auto& edge = obligation.Value().dependencies.front();
        const auto producer = std::find(plan.workSchedule.begin(), plan.workSchedule.end(), edge.producer);
        const auto consumer = std::find(plan.workSchedule.begin(), plan.workSchedule.end(), edge.consumer);
        if (producer != plan.workSchedule.end() && consumer != plan.workSchedule.end()) std::iter_swap(producer, consumer);
        plan.identity = l3::ComputePlanIdentity(plan);
        invalid.push_back(std::move(plan));
    }
    mutate([](auto& plan) { plan.queueAssignments.pop_back(); });
    mutate([](auto& plan) { plan.queueAssignments.front().queueIndex = 99; });
    mutate([](auto& plan) { plan.synchronization.clear(); });
    mutate([](auto& plan) { plan.resourceInstances.front().physicalInstanceCount += 1; });
    mutate([](auto& plan) { plan.resourceInstances.front().firstUse = 99; });
    mutate([](auto& plan) { plan.allocations.front().resources.clear(); });
    mutate([](auto& plan) { plan.useStates.pop_back(); });
    if (!safe.bindings.empty()) mutate([](auto& plan) { plan.bindings.pop_back(); });
    if (!safe.externalBoundaries.empty()) mutate([](auto& plan) { plan.externalBoundaries.clear(); });
    for (std::size_t index = 0; index < invalid.size(); ++index)
    {
        const auto report = verify::Verify(obligation.Value(), contract, invalid[index]);
        if (report.verified)
        {
            std::cerr << "invalid Plan mutation " << index << " passed verification\n";
            return 11;
        }
    }
    auto badIdentity = safe;
    badIdentity.identity[0] = std::byte{0x7f};
    if (verify::Verify(obligation.Value(), contract, badIdentity).verified)
    {
        std::cerr << "invalid PlanIdentity passed verification\n";
        return 12;
    }
    return 0;
}

int CheckCandidatesAndPolicies(const Case& item)
{
    l3::CompilerPolicy safePolicy;
    safePolicy.kind = l3::CompilerPolicyKind::CanonicalSafe;
    auto first = l3c::CompileLevel3(item.graph, item.profile, safePolicy);
    auto repeated = l3c::CompileLevel3(item.graph, item.profile, safePolicy);
    if (!first || !repeated)
    {
        const auto* error = !first ? &first.Error() : &repeated.Error();
        std::cerr << "Level 3 compile failed at " << error->stage << ": " << error->message << '\n';
        return 20;
    }
    if (first.Value().manifest.canonicalBytes != repeated.Value().manifest.canonicalBytes)
    {
        std::cerr << "Candidate Manifest is not deterministic\n";
        return 21;
    }
    std::set<std::string> plans;
    std::set<std::string> packages;
    for (const auto& candidate : first.Value().manifest.candidates)
    {
        if (!candidate.verification.verified) continue;
        plans.insert(sge::base::ToHex(candidate.plan.identity));
        packages.insert(candidate.packageExecutionDigestHex);
    }
    if (plans.size() < 2 || packages.size() < 2)
    {
        std::cerr << "candidate axes did not produce distinct verified Plans and Packages\n";
        return 22;
    }
    const auto frontier = l3c::ParetoFrontier(first.Value().manifest.candidates);
    if (frontier.empty() || !std::is_sorted(frontier.begin(), frontier.end(), [&](auto left, auto right) {
        return first.Value().manifest.candidates[left].plan.identity <
               first.Value().manifest.candidates[right].plan.identity;
    }))
    {
        std::cerr << "deterministic Pareto frontier construction failed\n";
        return 22;
    }

    l3::CompilerPolicy directPolicy;
    directPolicy.kind = l3::CompilerPolicyKind::MinimizeQueueHandoffs;
    l3::CompilerPolicy dedicatedPolicy;
    dedicatedPolicy.kind = l3::CompilerPolicyKind::PreferDedicatedQueues;
    auto direct = l3c::CompileLevel3(item.graph, item.profile, directPolicy);
    auto dedicated = l3c::CompileLevel3(item.graph, item.profile, dedicatedPolicy);
    if (!direct || !dedicated || direct.Value().selectedPlan.identity == dedicated.Value().selectedPlan.identity)
    {
        std::cerr << "Queue policies did not select different Plans\n";
        return 23;
    }

    l3::ProfileRecord profile;
    profile.planIdentity = dedicated.Value().selectedPlan.identity;
    profile.packageExecutionDigest = ParseDigest(dedicated.Value().selectedPackage.executionDigestHex);
    profile.targetProfileDigest = dedicated.Value().planningContract.digest;
    const std::string adapter = "WARP-Level3-Qualification";
    const std::string scenario = "DynamicExternalPipeline-OneFrame";
    profile.adapterDriverFingerprint = sge::base::Sha256(std::as_bytes(std::span(adapter)));
    profile.measurementScenarioDigest = sge::base::Sha256(std::as_bytes(std::span(scenario)));
    profile.sampleCount = 8;
    profile.measuredNanoseconds = 1000;
    l3::CompilerPolicy profilePolicy;
    profilePolicy.kind = l3::CompilerPolicyKind::ProfileGuided;
    l3::ProfileSelectionContext context{profile.adapterDriverFingerprint, profile.measurementScenarioDigest, 4};
    auto reselected = l3c::CompileLevel3(item.graph, item.profile, profilePolicy, &profile, &context);
    if (!reselected || reselected.Value().selectedPlan.identity != profile.planIdentity)
    {
        std::cerr << "fixed Profile record did not deterministically reselect its Plan\n";
        return 24;
    }
    auto stale = profile;
    stale.targetProfileDigest[0] = std::byte{0xff};
    if (l3c::CompileLevel3(item.graph, item.profile, profilePolicy, &stale, &context))
    {
        std::cerr << "stale Profile provenance was accepted\n";
        return 25;
    }
    stale = profile;
    stale.packageExecutionDigest[0] ^= std::byte{0x01};
    if (l3c::CompileLevel3(item.graph, item.profile, profilePolicy, &stale, &context))
    {
        std::cerr << "stale Package execution digest was accepted\n";
        return 26;
    }
    return 0;
}

int CheckAllocationCandidates(const Case& item)
{
    auto validated = compiler::ValidateSourceStage(item.graph, item.profile);
    if (!validated) return 27;
    auto obligation = l3::BuildSemanticObligation(item.graph, validated.Value().analyzed);
    if (!obligation) return 27;
    const auto contract = l3::BuildPlanningContract(item.profile);
    l3::CompilerPolicy policy;
    const auto plans = l3c::GenerateCandidatePlans(obligation.Value(), contract, policy);
    const auto findStrategy = [&](l3::AllocationStrategy strategy) {
        return std::find_if(plans.begin(), plans.end(), [&](const auto& plan) {
            return plan.scheduleStrategy == l3::ScheduleStrategy::CanonicalMinimumId &&
                   plan.queueStrategy == l3::QueueStrategy::CanonicalSafe &&
                   plan.allocationStrategy == strategy &&
                   verify::Verify(obligation.Value(), contract, plan).verified;
        });
    };
    const auto alias = findStrategy(l3::AllocationStrategy::CanonicalSafe);
    const auto committed = findStrategy(l3::AllocationStrategy::ConservativeCommitted);
    if (alias == plans.end() || committed == plans.end())
    {
        std::cerr << "Schema-17-compatible allocation candidates are missing\n";
        return 27;
    }
    auto aliasPackage = compiler::CompileSelectedPlan(item.graph, item.profile, *alias);
    auto committedPackage = compiler::CompileSelectedPlan(item.graph, item.profile, *committed);
    const auto aliasCost = l3c::CalculateCost(obligation.Value(), *alias);
    const auto committedCost = l3c::CalculateCost(obligation.Value(), *committed);
    if (!aliasPackage || !committedPackage ||
        aliasPackage.Value().packageBytes == committedPackage.Value().packageBytes ||
        aliasCost.aliasGroupCount == 0 || committedCost.aliasGroupCount != 0 ||
        aliasCost.totalReservedBytes >= committedCost.totalReservedBytes)
    {
        std::cerr << "alias and conservative allocation Plans did not preserve their distinct frozen contracts\n";
        return 28;
    }
    return 0;
}

struct ManifestText final { std::string value; };

sge::base::Result<ManifestText, std::string> BuildFreezeManifest(const std::vector<Case>& corpus)
{
    std::ostringstream output;
    output << "identity=SGE2-Level3-D3D12-v1-FinalFreeze-Corpus1\n";
    output << "level2-baseline=fc6b883b20d5428a4bf4f82b072fab15e8cb844a\n";
    output << "level3-docs-baseline=0df58f81b69d7d1ca333250c74b157391c6cbe90\n";
    output << "target-schema=17\nminimum-runtime=17\n";
    output << "level2-regression-count=" << corpus.size() << '\n';
    for (const auto& item : corpus)
    {
        auto validated = compiler::ValidateSourceStage(item.graph, item.profile);
        if (!validated) return sge::base::Result<ManifestText, std::string>::Failure(item.name + ": validation failed");
        auto obligation = l3::BuildSemanticObligation(item.graph, validated.Value().analyzed);
        if (!obligation) return sge::base::Result<ManifestText, std::string>::Failure(item.name + ": obligation failed");
        const auto contract = l3::BuildPlanningContract(item.profile);
        const auto safe = l3c::BuildCanonicalSafePlan(obligation.Value(), contract);
        const auto report = verify::Verify(obligation.Value(), contract, safe);
        if (!report.verified) return sge::base::Result<ManifestText, std::string>::Failure(item.name + ": safe Plan rejected");
        auto oldPackage = compiler::Compile(item.graph, item.profile);
        auto newPackage = compiler::CompileSelectedPlan(item.graph, item.profile, safe);
        if (!oldPackage || !newPackage || oldPackage.Value().packageBytes != newPackage.Value().packageBytes)
            return sge::base::Result<ManifestText, std::string>::Failure(item.name + ": Level 2 bytes changed");
        output << "case=" << item.name
               << "|obligation=" << sge::base::ToHex(obligation.Value().digest)
               << "|contract=" << sge::base::ToHex(contract.digest)
               << "|plan=" << sge::base::ToHex(safe.identity)
               << "|file=" << sge::base::ToHex(sge::base::Sha256(newPackage.Value().packageBytes))
               << "|execution=" << newPackage.Value().executionDigestHex << '\n';
    }
    const auto candidate = std::find_if(corpus.begin(), corpus.end(), [](const auto& item) {
        return item.name.find("Independent") != std::string::npos;
    });
    if (candidate == corpus.end()) return sge::base::Result<ManifestText, std::string>::Failure("candidate case missing");
    for (const auto policyKind : {l3::CompilerPolicyKind::CanonicalSafe,
                                  l3::CompilerPolicyKind::MinimizePeakMemory,
                                  l3::CompilerPolicyKind::MinimizeQueueHandoffs,
                                  l3::CompilerPolicyKind::PreferDedicatedQueues,
                                  l3::CompilerPolicyKind::MinimizeOperationCount})
    {
        l3::CompilerPolicy policy; policy.kind = policyKind;
        auto compiled = l3c::CompileLevel3(candidate->graph, candidate->profile, policy);
        if (!compiled) return sge::base::Result<ManifestText, std::string>::Failure("policy compile failed");
        output << "policy=" << static_cast<std::uint16_t>(policyKind)
               << "|selected=" << sge::base::ToHex(compiled.Value().selectedPlan.identity)
               << "|execution=" << compiled.Value().selectedPackage.executionDigestHex
               << "|manifest=" << sge::base::ToHex(sge::base::Sha256(compiled.Value().manifest.canonicalBytes))
               << "|candidates=" << compiled.Value().manifest.candidates.size() << '\n';
    }
    const auto body = output.str();
    output << "qualification-aggregate=" << sge::base::ToHex(sge::base::Sha256(
        std::as_bytes(std::span(body)))) << '\n';
    return sge::base::Result<ManifestText, std::string>::Success({output.str()});
}
}

int main(int argc, char** argv)
{
    const auto corpus = BuildCompatibilityCorpus();
    if (corpus.size() != 54)
    {
        std::cerr << "Level 3 compatibility corpus construction failed: " << corpus.size() << " cases\n";
        return 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--emit-manifest")
    {
        auto manifest = BuildFreezeManifest(corpus);
        if (!manifest) { std::cerr << manifest.Error() << '\n'; return 30; }
        const auto bytes = std::as_bytes(std::span(manifest.Value().value));
        auto written = sge::base::WriteAllBytes(argv[2], bytes);
        if (!written) { std::cerr << written.Error() << '\n'; return 31; }
        return 0;
    }
    if (const auto result = CheckCanonicalCompatibility(corpus); result != 0) return result;
    const auto adversarial = std::find_if(corpus.begin(), corpus.end(), [](const auto& item) {
        return std::any_of(item.graph.resources.begin(), item.graph.resources.end(), [](const auto& resource) {
            return resource.lifetime == sge::semantic::LifetimeIntent::External;
        }) && item.graph.works.size() >= 4;
    });
    if (adversarial == corpus.end()) { std::cerr << "external adversarial case is missing\n"; return 2; }
    if (const auto result = CheckAdversarialVerifier(*adversarial); result != 0) return result;
    const auto candidate = std::find_if(corpus.begin(), corpus.end(), [](const auto& item) {
        return item.name.find("Independent") != std::string::npos;
    });
    if (candidate == corpus.end()) { std::cerr << "independent candidate case is missing\n"; return 2; }
    if (const auto result = CheckCandidatesAndPolicies(*candidate); result != 0) return result;
    const auto alias = std::find_if(corpus.begin(), corpus.end(), [](const auto& item) {
        return item.name.find("Slice 11 Aliasing") != std::string::npos;
    });
    if (alias == corpus.end()) { std::cerr << "allocation candidate case is missing\n"; return 2; }
    if (const auto result = CheckAllocationCandidates(*alias); result != 0) return result;
    std::cout << "Level 3 obligation, Plan IR, independent verifier, candidates, policy, and profile tests passed.\n";
    std::cout << "CanonicalSafe reproduced Level 2 Package bytes for the fixed " << corpus.size() << "-Package corpus.\n";
    return 0;
}
