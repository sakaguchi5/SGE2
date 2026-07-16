#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../08_D3D12PackageSchema/D3D12Encoding.h"
#include "../26_Level2GeneratedGraphs/GeneratedGraphs.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
namespace base = sge::base;
namespace sem = sge::semantic;
namespace analysis = sge::analysis;
namespace compiler = sge::compiler::d3d12;
namespace gen = sge::level2::generated;
namespace pkg = sge::package::d3d12_v13;

struct CompiledPackage final
{
    std::vector<std::byte> bytes;
    std::string digest;
};

base::Result<CompiledPackage, std::string> CompileOnce(const gen::GeneratedCase& generated)
{
    auto compiled = compiler::Compile(generated.graph, generated.targetProfile);
    if (!compiled)
        return base::Result<CompiledPackage, std::string>::Failure(
            compiled.Error().stage + ": " + compiled.Error().message);
    auto value = std::move(compiled).Value();
    return base::Result<CompiledPackage, std::string>::Success(
        {std::move(value.packageBytes), std::move(value.executionDigestHex)});
}

std::set<gen::WorkEdge> DependencyPairs(const analysis::AnalyzedGraph& analyzed)
{
    std::set<gen::WorkEdge> result;
    for (const auto& edge : analyzed.dependencies)
        result.insert({edge.producer, edge.consumer});
    return result;
}

bool ValidateAnalysis(const gen::GeneratedCase& generated, std::string_view label)
{
    auto analyzed = analysis::Analyze(generated.graph);
    if (!analyzed)
    {
        std::cerr << label << ": SemanticAnalysis rejected generated graph";
        if (!analyzed.Error().empty()) std::cerr << ": " << analyzed.Error().front().message;
        std::cerr << '\n';
        return false;
    }
    if (analyzed.Value().canonicalWorkOrder != generated.oracle.canonicalWorkOrder)
    {
        std::cerr << label << ": canonical Work order differs from the independent oracle\n";
        return false;
    }
    const std::set<gen::WorkEdge> expected(
        generated.oracle.reducedEdges.begin(), generated.oracle.reducedEdges.end());
    if (DependencyPairs(analyzed.Value()) != expected)
    {
        std::cerr << label << ": transitive reduction differs from the independent oracle\n";
        return false;
    }
    return true;
}

std::vector<gen::ExpectedWait> WaitsFor(
    const gen::GeneratedCase& generated,
    std::uint32_t consumerPosition)
{
    std::vector<gen::ExpectedWait> result;
    for (const auto& wait : generated.oracle.waits)
        if (wait.consumerPosition == consumerPosition) result.push_back(wait);
    return result;
}

bool ValidateOperationModel(const gen::GeneratedCase& generated,
                            const pkg::D3D12PackageView& view,
                            std::string_view label)
{
    const auto expectedResources = generated.spec.kinds.size() +
        (generated.oracle.copyWorkCount != 0 ? 1u : 0u);
    const auto expectedViews = generated.oracle.computeWorkCount +
        generated.oracle.copyWorkCount * 2u;
    if (view.Resources().size() != expectedResources ||
        view.Views().size() != expectedViews ||
        view.Shaders().size() != (generated.oracle.computeWorkCount != 0 ? 1u : 0u) ||
        view.Programs().size() != generated.oracle.computeWorkCount ||
        view.BindingLayouts().size() != generated.oracle.computeWorkCount ||
        view.ComputeExecutables().size() != generated.oracle.computeWorkCount ||
        view.ComputeCommands().size() != generated.oracle.computeWorkCount ||
        !view.Executables().empty() || !view.RasterCommands().empty() ||
        !view.SurfaceSlots().empty())
    {
        std::cerr << label << ": Package artifact cardinality differs from the generated graph\n";
        return false;
    }

    if (view.Profile().computeQueueCount != generated.targetProfile.computeQueueCount ||
        view.Profile().copyQueueCount != generated.targetProfile.copyQueueCount ||
        view.Profile().surfaceImageCount != 0 ||
        view.Profile().shaderDescriptorCount != generated.oracle.computeWorkCount)
    {
        std::cerr << label << ": frozen Target profile differs from the generated profile\n";
        return false;
    }

    std::vector<gen::ExpectedWait> pendingWaits;
    std::vector<bool> signaled(generated.spec.kinds.size(), false);
    std::uint32_t executePosition = 0;
    std::uint32_t signalCount = 0;
    for (const auto& operation : view.FrameOperations())
    {
        if (operation.opcode == pkg::D3D12OperationCode::WaitQueue)
        {
            auto payload = pkg::DecodeWaitQueue(operation.payload);
            if (!payload || operation.operationVersion != 2 ||
                payload.Value().signalPoint.value >= generated.oracle.queueByPosition.size())
            {
                std::cerr << label << ": malformed WaitQueue operation\n";
                return false;
            }
            const auto signal = payload.Value().signalPoint.value;
            pendingWaits.push_back({executePosition, operation.queue.value,
                generated.oracle.queueByPosition[signal], signal});
            continue;
        }

        const bool compute = operation.opcode == pkg::D3D12OperationCode::ExecuteCompute;
        const bool copy = operation.opcode == pkg::D3D12OperationCode::ExecuteCopy;
        if (compute || copy)
        {
            if (executePosition >= generated.oracle.canonicalWorkOrder.size())
            {
                std::cerr << label << ": Package contains too many Execute operations\n";
                return false;
            }
            const auto workId = generated.oracle.canonicalWorkOrder[executePosition];
            const auto found = std::find(generated.workIdByNode.begin(), generated.workIdByNode.end(), workId);
            if (found == generated.workIdByNode.end()) return false;
            const auto node = static_cast<std::uint32_t>(std::distance(generated.workIdByNode.begin(), found));
            const bool expectedCompute = generated.spec.kinds[node] == gen::NodeKind::Compute;
            if (compute != expectedCompute ||
                operation.queue.value != generated.oracle.queueByPosition[executePosition])
            {
                std::cerr << label << ": Execute kind or Queue differs from the oracle at position "
                          << executePosition << '\n';
                return false;
            }
            if (compute && !pkg::DecodeExecuteCompute(operation.payload))
            {
                std::cerr << label << ": malformed ExecuteCompute payload\n";
                return false;
            }
            if (copy && !pkg::DecodeCopyBuffer(operation.payload))
            {
                std::cerr << label << ": malformed ExecuteCopy payload\n";
                return false;
            }
            auto expectedWaits = WaitsFor(generated, executePosition);
            if (pendingWaits != expectedWaits)
            {
                std::cerr << label << ": WaitQueue set differs from the latest-producer oracle at position "
                          << executePosition << '\n';
                return false;
            }
            pendingWaits.clear();
            ++executePosition;
            continue;
        }

        if (operation.opcode == pkg::D3D12OperationCode::SignalQueue)
        {
            auto payload = pkg::DecodeSignalQueue(operation.payload);
            if (!payload || operation.operationVersion != 2 ||
                payload.Value().signalPoint.value >= signaled.size())
            {
                std::cerr << label << ": malformed SignalQueue operation\n";
                return false;
            }
            const auto signal = payload.Value().signalPoint.value;
            if (signaled[signal] || signal != signalCount ||
                operation.queue.value != generated.oracle.queueByPosition[signal])
            {
                std::cerr << label << ": SignalPoint identities are not dense or queue-correct\n";
                return false;
            }
            signaled[signal] = true;
            ++signalCount;
        }
        else if (operation.opcode == pkg::D3D12OperationCode::WaitTemporal ||
                 operation.opcode == pkg::D3D12OperationCode::AcquireExternal ||
                 operation.opcode == pkg::D3D12OperationCode::WaitExternal ||
                 operation.opcode == pkg::D3D12OperationCode::ReleaseExternal ||
                 operation.opcode == pkg::D3D12OperationCode::PresentSurface ||
                 operation.opcode == pkg::D3D12OperationCode::AcquireSurfaceImage)
        {
            std::cerr << label << ": generated headless DAG acquired an unrelated runtime boundary\n";
            return false;
        }
    }

    if (executePosition != generated.spec.kinds.size() ||
        signalCount != generated.spec.kinds.size() ||
        !pendingWaits.empty() ||
        std::find(signaled.begin(), signaled.end(), false) != signaled.end())
    {
        std::cerr << label << ": generated Frame stream is incomplete\n";
        return false;
    }
    return true;
}

bool ValidateCompiledCase(gen::GeneratedCase generated,
                          bool verifyPermutation)
{
    const auto label = generated.spec.name;
    if (!ValidateAnalysis(generated, label)) return false;

    auto first = CompileOnce(generated);
    auto second = CompileOnce(generated);
    if (!first || !second)
    {
        std::cerr << label << ": Compile failed: "
                  << (!first ? first.Error() : second.Error()) << '\n';
        return false;
    }
    if (first.Value().bytes != second.Value().bytes ||
        first.Value().digest != second.Value().digest)
    {
        std::cerr << label << ": repeated Compile is not byte deterministic\n";
        return false;
    }

    if (verifyPermutation)
    {
        auto permutedCase = generated;
        permutedCase.graph = gen::PermuteStorage(
            std::move(permutedCase.graph), generated.spec.seed ^ 0xE7037ED1A0B428DBull);
        auto permuted = CompileOnce(permutedCase);
        if (!permuted || permuted.Value().bytes != first.Value().bytes ||
            permuted.Value().digest != first.Value().digest)
        {
            std::cerr << label << ": source storage permutation changed the Package\n";
            return false;
        }
    }

    auto frozen = sge::package::PackageReader::Read(first.Value().bytes);
    if (!frozen)
    {
        std::cerr << label << ": PackageReader rejected Compiler output: "
                  << frozen.Error().message << '\n';
        return false;
    }
    auto decoded = pkg::D3D12PackageView::Decode(frozen.Value());
    if (!decoded)
    {
        std::cerr << label << ": Package schema rejected Compiler output: "
                  << decoded.Error().message << '\n';
        return false;
    }
    return ValidateOperationModel(generated, decoded.Value(), label);
}

gen::GeneratedSpec ExplicitSpec(
    std::string name,
    std::vector<gen::NodeKind> kinds,
    std::initializer_list<gen::Edge> edges,
    std::uint64_t seed)
{
    gen::GeneratedSpec spec;
    spec.name = std::move(name);
    spec.kinds = std::move(kinds);
    spec.edges.assign(edges.begin(), edges.end());
    spec.seed = seed;
    return spec;
}

int RunExhaustiveSmallDagAnalysis()
{
    std::uint64_t graphCount = 0;
    for (std::uint32_t nodes = 1; nodes <= 6; ++nodes)
    {
        const auto edgeSlots = nodes * (nodes - 1u) / 2u;
        const auto maskCount = 1ull << edgeSlots;
        for (std::uint64_t mask = 0; mask < maskCount; ++mask)
        {
            auto spec = gen::MakeForwardMaskSpec(
                "StageJ.Exhaustive." + std::to_string(nodes) + "." + std::to_string(mask),
                nodes, mask, 0xA5A5A5A500000000ull ^ (mask * 0x9E3779B97F4A7C15ull) ^ nodes,
                false);
            auto generated = gen::BuildGeneratedCase(std::move(spec));
            if (!generated)
            {
                std::cerr << "exhaustive generator rejected mask " << mask
                          << " at node count " << nodes << ": " << generated.Error() << '\n';
                return 1;
            }
            if (!ValidateAnalysis(generated.Value(), generated.Value().spec.name)) return 2;
            ++graphCount;
        }
    }
    if (graphCount != 33867)
    {
        std::cerr << "unexpected exhaustive graph cardinality: " << graphCount << '\n';
        return 3;
    }
    std::cout << "  exhaustive: 33,867 forward-edge DAGs with 1-6 Works matched the independent oracle.\n";
    return 0;
}

int RunRepresentativeCompileCorpus()
{
    std::vector<gen::GeneratedSpec> specs;
    specs.push_back(ExplicitSpec("StageJ.SingleCompute", {gen::NodeKind::Compute}, {}, 0x101));
    specs.push_back(ExplicitSpec("StageJ.IndependentMixed",
        {gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute, gen::NodeKind::Copy}, {}, 0x102));
    specs.push_back(ExplicitSpec("StageJ.MixedChain",
        {gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute},
        {{0,1},{1,2},{2,3},{3,4}}, 0x103));
    specs.push_back(ExplicitSpec("StageJ.FanOut",
        {gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute},
        {{0,1},{0,2},{0,3},{0,4}}, 0x104));
    specs.push_back(ExplicitSpec("StageJ.FanInLatestProducer",
        {gen::NodeKind::Compute, gen::NodeKind::Compute, gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute},
        {{0,3},{1,3},{2,3},{3,4}}, 0x105));
    specs.push_back(ExplicitSpec("StageJ.Diamond",
        {gen::NodeKind::Copy, gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute},
        {{0,1},{0,2},{1,3},{2,3}}, 0x106));
    specs.push_back(ExplicitSpec("StageJ.DisconnectedComponents",
        {gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute,
         gen::NodeKind::Copy, gen::NodeKind::Compute, gen::NodeKind::Copy},
        {{0,1},{1,2},{3,4},{4,5}}, 0x107));
    specs.push_back(ExplicitSpec("StageJ.Layered",
        {gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute,
         gen::NodeKind::Copy, gen::NodeKind::Compute, gen::NodeKind::Copy},
        {{0,2},{0,3},{1,2},{1,3},{2,4},{2,5},{3,4},{3,5}}, 0x108));
    specs.push_back(ExplicitSpec("StageJ.DenseTransitive",
        {gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute,
         gen::NodeKind::Copy, gen::NodeKind::Compute, gen::NodeKind::Copy},
        {{0,1},{0,2},{0,3},{0,4},{0,5},{1,2},{1,3},{1,4},{1,5},
         {2,3},{2,4},{2,5},{3,4},{3,5},{4,5}}, 0x109));

    constexpr std::array<std::uint64_t, 16> masks = {
        0x0000,0x0001,0x0003,0x0015,0x002A,0x0055,0x00AA,0x0123,
        0x0249,0x0492,0x0924,0x1249,0x1555,0x2AAA,0x5A5A,0x7FFF};
    for (std::size_t index = 0; index < masks.size(); ++index)
        specs.push_back(gen::MakeForwardMaskSpec(
            "StageJ.SampleMask." + std::to_string(index), 6, masks[index],
            0xC6BC279692B5CC83ull ^ masks[index], true));

    std::uint32_t compiledCount = 0;
    for (auto& spec : specs)
    {
        auto generated = gen::BuildGeneratedCase(std::move(spec));
        if (!generated)
        {
            std::cerr << "representative generator failed: " << generated.Error() << '\n';
            return 1;
        }
        if (!ValidateCompiledCase(std::move(generated).Value(), true)) return 2;
        ++compiledCount;
    }
    std::cout << "  compiled: " << compiledCount
              << " representative chain/fan/diamond/layered/dense/mixed-queue Packages passed.\n";
    return 0;
}

int RunLargeSeedCorpus()
{
    const std::array<std::tuple<std::uint32_t, std::uint64_t, std::uint32_t>, 4> cases = {{
        {10u,  0x243F6A8885A308D3ull, 28u},
        {25u,  0x13198A2E03707344ull, 18u},
        {50u,  0xA4093822299F31D0ull, 12u},
        {100u, 0x082EFA98EC4E6C89ull,  8u}}};
    for (const auto& [nodes, seed, density] : cases)
    {
        auto spec = gen::MakeSeededSpec(
            "StageJ.Seed." + std::to_string(nodes) + "." + std::to_string(seed),
            nodes, seed, density, true);
        auto generated = gen::BuildGeneratedCase(std::move(spec));
        if (!generated)
        {
            std::cerr << "large seed generator failed for seed " << seed
                      << ": " << generated.Error() << '\n';
            return 1;
        }
        if (!ValidateCompiledCase(std::move(generated).Value(), true))
        {
            std::cerr << "large seed qualification failed; seed=" << seed
                      << ", nodes=" << nodes << ", density=" << density << '\n';
            return 2;
        }
        std::cout << "    seed=" << seed << ", Works=" << nodes
                  << ", edgeDensity=" << density << "%\n";
    }
    std::cout << "  large: deterministic 10/25/50/100-Work seed corpus passed.\n";
    return 0;
}

sem::Work* FindWork(sem::SemanticGraph& graph, sem::WorkId id)
{
    const auto found = std::find_if(graph.works.begin(), graph.works.end(),
        [id](const sem::Work& work) { return work.id == id; });
    return found == graph.works.end() ? nullptr : &*found;
}

bool ExpectCompileRejection(std::string_view label,
                            const sem::SemanticGraph& graph,
                            const sge::target::D3D12TargetProfile& profile,
                            std::string_view stage)
{
    auto compiled = compiler::Compile(graph, profile);
    if (compiled)
    {
        std::cerr << label << " was accepted\n";
        return false;
    }
    if (compiled.Error().stage != stage)
    {
        std::cerr << label << " failed at " << compiled.Error().stage
                  << " instead of " << stage << '\n';
        return false;
    }
    return true;
}

int RunGeneratedInvalidCorpus()
{
    auto baseSpec = ExplicitSpec("StageJ.InvalidBase",
        {gen::NodeKind::Compute, gen::NodeKind::Copy, gen::NodeKind::Compute},
        {{0,1},{1,2}}, 0xBAD5EED);
    auto generatedResult = gen::BuildGeneratedCase(baseSpec);
    if (!generatedResult) return 1;
    auto generated = std::move(generatedResult).Value();

    auto cycle = generated.graph;
    auto* first = FindWork(cycle, generated.workIdByNode[0]);
    if (first == nullptr) return 2;
    first->dependencies.push_back(generated.workIdByNode[2]);
    if (!ExpectCompileRejection("generated cycle", cycle, generated.targetProfile, "semantic-analysis")) return 3;

    auto dangling = generated.graph;
    auto* danglingWork = FindWork(dangling, generated.workIdByNode[2]);
    if (danglingWork == nullptr) return 4;
    danglingWork->dependencies.push_back(sem::WorkId{9999});
    if (!ExpectCompileRejection("dangling dependency", dangling, generated.targetProfile, "semantic-analysis")) return 5;

    auto duplicateOwner = generated.graph;
    auto computeWorks = std::vector<sem::Work*>();
    for (auto& work : duplicateOwner.works)
        if (work.kind == sem::WorkKind::Compute) computeWorks.push_back(&work);
    if (computeWorks.size() < 2 || computeWorks[0]->operands.empty() || computeWorks[1]->operands.empty()) return 6;
    computeWorks[1]->operands[0].use = computeWorks[0]->operands[0].use;
    if (!ExpectCompileRejection("ResourceUse with two owners", duplicateOwner,
                                generated.targetProfile, "semantic-analysis")) return 7;

    auto missingBinding = generated.graph;
    auto missingWork = std::find_if(missingBinding.works.begin(), missingBinding.works.end(),
        [](const sem::Work& work) { return work.kind == sem::WorkKind::Compute; });
    if (missingWork == missingBinding.works.end()) return 8;
    missingWork->operands.clear();
    if (!ExpectCompileRejection("missing ProgramParameter binding", missingBinding,
                                generated.targetProfile, "semantic-analysis")) return 9;

    auto copyOverflow = generated.graph;
    auto copy = std::find_if(copyOverflow.works.begin(), copyOverflow.works.end(),
        [](const sem::Work& work) { return work.kind == sem::WorkKind::Copy; });
    if (copy == copyOverflow.works.end()) return 10;
    copy->copy.bytes = 64;
    if (!ExpectCompileRejection("generated Copy overflow", copyOverflow,
                                generated.targetProfile, "semantic-analysis")) return 11;

    auto previousPersistent = generated.graph;
    if (previousPersistent.resourceUses.empty()) return 12;
    previousPersistent.resourceUses.front().temporalRelation = sem::TemporalRelation::Previous;
    if (!ExpectCompileRejection("Previous on non-Temporal Resource", previousPersistent,
                                generated.targetProfile, "semantic-analysis")) return 13;

    auto unsupportedTexture = generated.graph;
    sem::Resource texture;
    texture.id = {9000};
    texture.debugName = "StageJ.Invalid.FixedDepthTexture";
    texture.kind = sem::ResourceKind::Texture2D;
    texture.lifetime = sem::LifetimeIntent::Persistent;
    texture.update = sem::UpdateIntent::Immutable;
    texture.visibility = sem::Visibility::Internal;
    texture.texture2D = {sem::TextureExtentMeaning::Fixed, 1, 1,
                         sem::FormatMeaning::Depth32Float, 4, 1};
    texture.initialContent.resize(4);
    unsupportedTexture.resources.push_back(texture);
    if (!ExpectCompileRejection("unsupported generated Texture format", unsupportedTexture,
                                generated.targetProfile, "target-feasibility")) return 14;

    auto multipleSurfaces = generated.graph;
    for (std::uint32_t index = 0; index < 2; ++index)
    {
        sem::Resource surface;
        surface.id = {9100u + index};
        surface.debugName = "StageJ.Invalid.Surface." + std::to_string(index);
        surface.kind = sem::ResourceKind::SurfaceImage;
        surface.lifetime = sem::LifetimeIntent::External;
        surface.update = sem::UpdateIntent::External;
        surface.visibility = sem::Visibility::Published;
        surface.surface.formatMeaning = sem::FormatMeaning::Bgra8Unorm;
        multipleSurfaces.resources.push_back(surface);
    }
    auto surfaceProfile = generated.targetProfile;
    surfaceProfile.surfaceImageCount = 2;
    if (!ExpectCompileRejection("multiple generated Surfaces", multipleSurfaces,
                                surfaceProfile, "target-feasibility")) return 15;

    auto duplicateWorkId = generated.graph;
    if (duplicateWorkId.works.size() < 2) return 16;
    duplicateWorkId.works[1].id = duplicateWorkId.works[0].id;
    if (!ExpectCompileRejection("duplicate generated Work ID", duplicateWorkId,
                                generated.targetProfile, "semantic-analysis")) return 17;

    auto invalidBackEdge = baseSpec;
    invalidBackEdge.edges.push_back({2, 1});
    auto rejectedBackEdge = gen::BuildGeneratedCase(std::move(invalidBackEdge));
    if (rejectedBackEdge)
    {
        std::cerr << "generator accepted a backward logical edge\n";
        return 18;
    }
    auto duplicateEdge = baseSpec;
    duplicateEdge.edges.push_back({0, 1});
    auto rejectedDuplicate = gen::BuildGeneratedCase(std::move(duplicateEdge));
    if (rejectedDuplicate)
    {
        std::cerr << "generator accepted a duplicate logical edge\n";
        return 19;
    }

    std::cout << "  invalid: 9 Compiler-boundary mutations and 2 generator-contract mutations were rejected.\n";
    return 0;
}
}

int main()
{
    std::cout << "Stage-J exhaustive small-DAG model qualification...\n";
    if (const auto result = RunExhaustiveSmallDagAnalysis(); result != 0) return 10 + result;

    std::cout << "Stage-J representative Package qualification...\n";
    if (const auto result = RunRepresentativeCompileCorpus(); result != 0) return 40 + result;

    std::cout << "Stage-J deterministic large-seed qualification...\n";
    if (const auto result = RunLargeSeedCorpus(); result != 0) return 70 + result;

    std::cout << "Stage-J generated invalid-boundary qualification...\n";
    if (const auto result = RunGeneratedInvalidCorpus(); result != 0) return 100 + result;

    std::cout << "Stage-J generated DAG model qualification passed.\n";
    return 0;
}
