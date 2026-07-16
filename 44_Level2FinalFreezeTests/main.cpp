#include "../00_Base/BinaryIO.h"
#include "../00_Base/FileIO.h"
#include "../00_Base/Sha256.h"
#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../08_D3D12PackageSchema/D3D12Encoding.h"
#include "../24_Level1Scenarios/Level1Scenarios.h"
#include "../25_Level2Scenarios/Level2Scenarios.h"
#include "../26_Level2GeneratedGraphs/GeneratedGraphs.h"
#include "../27_Level2RuntimeScenarios/RuntimeScenarios.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

#ifdef interface
#undef interface
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <sstream>
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
namespace pkg = sge::package;
namespace d3d = sge::package::d3d12_v13;
namespace lvl1 = sge::level1;
namespace lvl2 = sge::level2;
namespace gen = sge::level2::generated;
namespace runtime_cases = sge::level2::runtime_scenarios;

constexpr std::string_view FreezeIdentity = "SGE2-Level2-D3D12-v1-FinalFreeze-Corpus1";
constexpr std::string_view BaselineCommit = "fc6b883b20d5428a4bf4f82b072fab15e8cb844a";
constexpr std::uint32_t FrozenSchemaVersion = 17;
constexpr std::uint32_t FrozenRuntimeVersion = 17;
constexpr std::size_t FrozenCorpusCount = 54;
constexpr std::string_view ExpectedSemanticCorpusDigest = "4b07e0725dc87f30f495794ea2ef5e245dd0d6415397761c441a9a1ca4f83c49";

struct CorpusCase final
{
    std::string name;
    sem::SemanticGraph graph;
    sge::target::D3D12TargetProfile profile;
};

void WriteString(base::BinaryWriter& writer, std::string_view value)
{
    writer.WriteU64(static_cast<std::uint64_t>(value.size()));
    writer.WriteBytes(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(value.data()), value.size()));
}

void WriteId(base::BinaryWriter& writer, std::uint32_t value)
{
    writer.WriteU32(value);
}

template<class T, class F>
std::vector<const T*> SortedPointers(const std::vector<T>& values, F&& less)
{
    std::vector<const T*> result;
    result.reserve(values.size());
    for (const auto& value : values) result.push_back(&value);
    std::sort(result.begin(), result.end(), std::forward<F>(less));
    return result;
}

base::Result<base::Digest256, std::string> SemanticDigest(const CorpusCase& value)
{
    auto analyzed = analysis::Analyze(value.graph);
    if (!analyzed)
    {
        std::string message = "semantic corpus analysis failed";
        if (!analyzed.Error().empty()) message += ": " + analyzed.Error().front().message;
        return base::Result<base::Digest256, std::string>::Failure(std::move(message));
    }

    base::BinaryWriter writer;
    WriteString(writer, FreezeIdentity);
    WriteString(writer, value.name);

    const auto& profile = value.profile;
    writer.WriteU32(profile.minimumFeatureLevel);
    writer.WriteU16(profile.shaderModelMajor);
    writer.WriteU16(profile.shaderModelMinor);
    writer.WriteU16(profile.rootSignatureMajor);
    writer.WriteU16(profile.rootSignatureMinor);
    writer.WriteU32(static_cast<std::uint32_t>(profile.barrierModel));
    writer.WriteU16(static_cast<std::uint16_t>(profile.shaderBinaryFormat));
    writer.WriteU32(profile.framesInFlight);
    writer.WriteU32(profile.directQueueCount);
    writer.WriteU32(profile.computeQueueCount);
    writer.WriteU32(profile.copyQueueCount);
    writer.WriteU32(profile.surfaceImageCount);
    writer.WriteU32(profile.rtvDescriptorCount);
    writer.WriteU32(profile.dsvDescriptorCount);
    writer.WriteU32(profile.shaderDescriptorCount);
    writer.WriteU32(profile.samplerDescriptorCount);

    const auto resources = SortedPointers(value.graph.resources, [](const auto* left, const auto* right) {
        return left->id.value < right->id.value;
    });
    writer.WriteU32(static_cast<std::uint32_t>(resources.size()));
    for (const auto* resource : resources)
    {
        WriteId(writer, resource->id.value);
        writer.WriteU16(static_cast<std::uint16_t>(resource->kind));
        writer.WriteU16(static_cast<std::uint16_t>(resource->lifetime));
        writer.WriteU16(static_cast<std::uint16_t>(resource->update));
        writer.WriteU16(static_cast<std::uint16_t>(resource->visibility));
        writer.WriteU64(resource->buffer.sizeBytes);
        writer.WriteU32(resource->buffer.strideBytes);
        writer.WriteU16(static_cast<std::uint16_t>(resource->texture2D.extentMeaning));
        writer.WriteU32(resource->texture2D.width);
        writer.WriteU32(resource->texture2D.height);
        writer.WriteU16(static_cast<std::uint16_t>(resource->texture2D.formatMeaning));
        writer.WriteU32(resource->texture2D.rowBytes);
        writer.WriteU16(resource->texture2D.mipLevels);
        writer.WriteU64(resource->dynamicData.requiredBytes);
        writer.WriteU32(resource->dynamicData.requiredAlignment);
        writer.WriteU16(static_cast<std::uint16_t>(resource->surface.formatMeaning));
        writer.WriteU64(static_cast<std::uint64_t>(resource->initialContent.size()));
        writer.WriteBytes(resource->initialContent);
        WriteId(writer, resource->aliasPreparation.value);
    }

    const auto uses = SortedPointers(value.graph.resourceUses, [](const auto* left, const auto* right) {
        return left->id.value < right->id.value;
    });
    writer.WriteU32(static_cast<std::uint32_t>(uses.size()));
    for (const auto* use : uses)
    {
        WriteId(writer, use->id.value);
        WriteId(writer, use->resource.value);
        writer.WriteU16(static_cast<std::uint16_t>(use->effect));
        writer.WriteU16(static_cast<std::uint16_t>(use->role));
        writer.WriteU16(static_cast<std::uint16_t>(use->temporalRelation));
    }

    const auto programs = SortedPointers(value.graph.programs, [](const auto* left, const auto* right) {
        return left->id.value < right->id.value;
    });
    writer.WriteU32(static_cast<std::uint32_t>(programs.size()));
    for (const auto* program : programs)
    {
        WriteId(writer, program->id.value);
        writer.WriteU16(static_cast<std::uint16_t>(program->kind));
        writer.WriteU32(program->interface.vertexStrideBytes);

        const auto vertexInputs = SortedPointers(program->interface.vertexInputs,
            [](const auto* left, const auto* right) {
                return std::tuple{left->byteOffset, left->meaning, left->componentCount} <
                       std::tuple{right->byteOffset, right->meaning, right->componentCount};
            });
        writer.WriteU32(static_cast<std::uint32_t>(vertexInputs.size()));
        for (const auto* input : vertexInputs)
        {
            writer.WriteU16(static_cast<std::uint16_t>(input->meaning));
            writer.WriteU16(input->componentCount);
            writer.WriteU32(input->byteOffset);
        }

        const auto parameters = SortedPointers(program->interface.parameters,
            [](const auto* left, const auto* right) { return left->id.value < right->id.value; });
        writer.WriteU32(static_cast<std::uint32_t>(parameters.size()));
        for (const auto* parameter : parameters)
        {
            WriteId(writer, parameter->id.value);
            writer.WriteU16(static_cast<std::uint16_t>(parameter->kind));
            writer.WriteU16(static_cast<std::uint16_t>(parameter->stage));
            writer.WriteU32(parameter->shaderRegister);
            writer.WriteU64(parameter->requiredBytes);
            writer.WriteU32(parameter->requiredAlignment);
        }
        WriteString(writer, program->source.hlslSource);
        WriteString(writer, program->source.vertexEntry);
        WriteString(writer, program->source.pixelEntry);
        WriteString(writer, program->source.computeEntry);
    }

    const auto works = SortedPointers(value.graph.works, [](const auto* left, const auto* right) {
        return left->id.value < right->id.value;
    });
    writer.WriteU32(static_cast<std::uint32_t>(works.size()));
    for (const auto* work : works)
    {
        WriteId(writer, work->id.value);
        writer.WriteU16(static_cast<std::uint16_t>(work->kind));
        WriteId(writer, work->raster.program.value);
        writer.WriteU32(work->raster.vertexCount);
        writer.WriteU64(work->copy.bytes);
        WriteId(writer, work->compute.program.value);
        writer.WriteU32(work->compute.threadGroupCountX);
        writer.WriteU32(work->compute.threadGroupCountY);
        writer.WriteU32(work->compute.threadGroupCountZ);

        const auto operands = SortedPointers(work->operands, [](const auto* left, const auto* right) {
            return std::tuple{left->kind, left->parameter.value, left->use.value} <
                   std::tuple{right->kind, right->parameter.value, right->use.value};
        });
        writer.WriteU32(static_cast<std::uint32_t>(operands.size()));
        for (const auto* operand : operands)
        {
            writer.WriteU16(static_cast<std::uint16_t>(operand->kind));
            WriteId(writer, operand->use.value);
            WriteId(writer, operand->parameter.value);
        }
    }

    writer.WriteU32(static_cast<std::uint32_t>(analyzed.Value().canonicalWorkOrder.size()));
    for (const auto work : analyzed.Value().canonicalWorkOrder) WriteId(writer, work.value);

    auto dependencies = analyzed.Value().dependencies;
    std::sort(dependencies.begin(), dependencies.end(), [](const auto& left, const auto& right) {
        return std::tuple{left.producer.value, left.consumer.value,
                          static_cast<std::uint16_t>(left.kind), left.resource.value} <
               std::tuple{right.producer.value, right.consumer.value,
                          static_cast<std::uint16_t>(right.kind), right.resource.value};
    });
    writer.WriteU32(static_cast<std::uint32_t>(dependencies.size()));
    for (const auto& dependency : dependencies)
    {
        WriteId(writer, dependency.producer.value);
        WriteId(writer, dependency.consumer.value);
        WriteId(writer, dependency.resource.value);
        writer.WriteU16(static_cast<std::uint16_t>(dependency.kind));
    }

    return base::Result<base::Digest256, std::string>::Success(base::Sha256(writer.Bytes()));
}

sem::SemanticGraph BuildStageGHeadlessGraph()
{
    sem::SemanticGraph graph;
    sem::Resource output;
    output.id = {0};
    output.kind = sem::ResourceKind::Buffer;
    output.lifetime = sem::LifetimeIntent::Persistent;
    output.update = sem::UpdateIntent::GpuWritten;
    output.visibility = sem::Visibility::Internal;
    output.buffer = {16, 16};
    graph.resources.push_back(output);

    graph.resourceUses.push_back({{0}, {0}, sem::Effect::Write,
        sem::ViewRole::StorageBuffer, sem::TemporalRelation::Current});

    sem::Program program;
    program.id = {0};
    program.kind = sem::ProgramKind::Compute;
    program.interface.parameters.push_back({{0}, "StageMHeadlessOutput",
        sem::ProgramParameterKind::UnorderedBuffer, sem::ShaderStage::Compute, 0, 0, 1});
    program.source.hlslSource = R"hlsl(
RWStructuredBuffer<float4> StageMHeadlessOutput : register(u0);
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    StageMHeadlessOutput[0] = float4(0.25f, 0.50f, 0.75f, 1.0f);
}
)hlsl";
    program.source.computeEntry = "CSMain";
    graph.programs.push_back(std::move(program));

    sem::Work work;
    work.id = {0};
    work.kind = sem::WorkKind::Compute;
    work.operands.push_back({sem::WorkOperandKind::ProgramParameter, {0}, {0}});
    work.compute.program = {0};
    graph.works.push_back(std::move(work));
    return graph;
}

gen::GeneratedSpec ExplicitSpec(std::string name,
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

std::vector<gen::GeneratedSpec> RepresentativeSpecs()
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
    return specs;
}

std::vector<gen::GeneratedSpec> LargeSeedSpecs()
{
    return {
        gen::MakeSeededSpec("StageJ.Seed.10.2611923443488327891", 10, 0x243F6A8885A308D3ull, 28, true),
        gen::MakeSeededSpec("StageJ.Seed.25.1376283091369227076", 25, 0x13198A2E03707344ull, 18, true),
        gen::MakeSeededSpec("StageJ.Seed.50.11820040416388919760", 50, 0xA4093822299F31D0ull, 12, true),
        gen::MakeSeededSpec("StageJ.Seed.100.589684135938649225", 100, 0x082EFA98EC4E6C89ull, 8, true)
    };
}

base::Result<std::vector<CorpusCase>, std::string> BuildCorpus()
{
    std::vector<CorpusCase> result;

    for (const auto key : lvl1::StageAInputs())
    {
        auto built = lvl1::BuildScenario(key);
        if (!built) return base::Result<std::vector<CorpusCase>, std::string>::Failure(built.Error());
        auto input = std::move(built).Value();
        result.push_back({"Level1/" + input.name, std::move(input.graph), input.targetProfile});
    }

    sge::target::D3D12TargetProfile headlessProfile;
    headlessProfile.computeQueueCount = 1;
    headlessProfile.copyQueueCount = 0;
    headlessProfile.surfaceImageCount = 0;
    headlessProfile.rtvDescriptorCount = 0;
    headlessProfile.dsvDescriptorCount = 0;
    headlessProfile.shaderDescriptorCount = 1;
    result.push_back({"StageG/HeadlessCompute", BuildStageGHeadlessGraph(), headlessProfile});

    for (const auto key : lvl2::StageHInputs())
    {
        auto built = lvl2::BuildStageHScenario(key);
        if (!built) return base::Result<std::vector<CorpusCase>, std::string>::Failure(built.Error());
        auto input = std::move(built).Value();
        result.push_back({"StageH/" + input.name, std::move(input.graph), input.targetProfile});
    }

    auto generatedSpecs = RepresentativeSpecs();
    auto largeSpecs = LargeSeedSpecs();
    generatedSpecs.insert(generatedSpecs.end(),
        std::make_move_iterator(largeSpecs.begin()), std::make_move_iterator(largeSpecs.end()));
    for (auto& spec : generatedSpecs)
    {
        auto built = gen::BuildGeneratedCase(std::move(spec));
        if (!built) return base::Result<std::vector<CorpusCase>, std::string>::Failure(built.Error());
        auto input = std::move(built).Value();
        result.push_back({"StageJ/" + input.spec.name, std::move(input.graph), input.targetProfile});
    }

    auto pipeline = runtime_cases::Build(runtime_cases::Scenario::DynamicExternalPipeline);
    if (!pipeline) return base::Result<std::vector<CorpusCase>, std::string>::Failure(pipeline.Error());
    auto pipelineInput = std::move(pipeline).Value();
    auto fallbackGraph = pipelineInput.graph;
    auto fallbackProfile = pipelineInput.targetProfile;
    fallbackProfile.computeQueueCount = 0;
    fallbackProfile.copyQueueCount = 0;
    result.push_back({"StageK/DynamicExternalDedicated", std::move(pipelineInput.graph), pipelineInput.targetProfile});
    result.push_back({"StageK/DynamicExternalDirectFallback", std::move(fallbackGraph), fallbackProfile});

    auto temporal = runtime_cases::Build(runtime_cases::Scenario::CrossQueueTemporal);
    if (!temporal) return base::Result<std::vector<CorpusCase>, std::string>::Failure(temporal.Error());
    auto temporalInput = std::move(temporal).Value();
    result.push_back({"StageK/CrossQueueTemporal", std::move(temporalInput.graph), temporalInput.targetProfile});

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    if (result.size() != FrozenCorpusCount)
        return base::Result<std::vector<CorpusCase>, std::string>::Failure(
            "final corpus cardinality changed: " + std::to_string(result.size()));
    const auto duplicate = std::adjacent_find(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.name == right.name;
    });
    if (duplicate != result.end())
        return base::Result<std::vector<CorpusCase>, std::string>::Failure(
            "final corpus contains duplicate case name: " + duplicate->name);
    return base::Result<std::vector<CorpusCase>, std::string>::Success(std::move(result));
}

struct DigestText final { std::string value; };

base::Result<DigestText, std::string> SemanticCorpusDigest(const std::vector<CorpusCase>& corpus)
{
    base::BinaryWriter writer;
    WriteString(writer, FreezeIdentity);
    WriteString(writer, BaselineCommit);
    writer.WriteU32(static_cast<std::uint32_t>(corpus.size()));
    for (const auto& item : corpus)
    {
        auto digest = SemanticDigest(item);
        if (!digest) return base::Result<DigestText, std::string>::Failure(digest.Error());
        WriteString(writer, item.name);
        writer.WriteBytes(digest.Value());
    }
    return base::Result<DigestText, std::string>::Success({base::ToHex(base::Sha256(writer.Bytes()))});
}

struct ManifestBuild final
{
    std::string text;
    std::string aggregateDigest;
    std::string semanticCorpusDigest;
};

base::Result<ManifestBuild, std::string> BuildManifest(bool verifyRepeatedCompile)
{
    auto corpusResult = BuildCorpus();
    if (!corpusResult) return base::Result<ManifestBuild, std::string>::Failure(corpusResult.Error());
    auto corpus = std::move(corpusResult).Value();
    auto corpusDigest = SemanticCorpusDigest(corpus);
    if (!corpusDigest) return base::Result<ManifestBuild, std::string>::Failure(corpusDigest.Error());
    if (corpusDigest.Value().value != ExpectedSemanticCorpusDigest)
        return base::Result<ManifestBuild, std::string>::Failure(
            "semantic corpus digest changed: expected " + std::string(ExpectedSemanticCorpusDigest) +
            ", actual " + corpusDigest.Value().value);

    std::ostringstream output;
    output << "identity=" << FreezeIdentity << '\n';
    output << "baseline-commit=" << BaselineCommit << '\n';
    output << "target-schema=" << FrozenSchemaVersion << '\n';
    output << "minimum-runtime=" << FrozenRuntimeVersion << '\n';
    output << "corpus-count=" << corpus.size() << '\n';
    output << "semantic-corpus-digest=" << corpusDigest.Value().value << '\n';

    for (const auto& item : corpus)
    {
        auto semantic = SemanticDigest(item);
        if (!semantic) return base::Result<ManifestBuild, std::string>::Failure(semantic.Error());
        auto first = compiler::Compile(item.graph, item.profile);
        if (!first)
            return base::Result<ManifestBuild, std::string>::Failure(
                item.name + " compile failed at " + first.Error().stage + ": " + first.Error().message);
        if (verifyRepeatedCompile)
        {
            auto second = compiler::Compile(item.graph, item.profile);
            if (!second)
                return base::Result<ManifestBuild, std::string>::Failure(
                    item.name + " repeated compile failed at " + second.Error().stage + ": " + second.Error().message);
            if (first.Value().packageBytes != second.Value().packageBytes ||
                first.Value().executionDigestHex != second.Value().executionDigestHex)
                return base::Result<ManifestBuild, std::string>::Failure(
                    item.name + " is not byte deterministic inside one process");
        }

        auto frozenResult = pkg::PackageReader::Read(first.Value().packageBytes);
        if (!frozenResult)
            return base::Result<ManifestBuild, std::string>::Failure(
                item.name + " PackageReader failed: " + frozenResult.Error().message);
        auto frozen = std::move(frozenResult).Value();
        if (frozen.Header().targetSchemaVersion != FrozenSchemaVersion ||
            frozen.Header().minimumRuntimeVersion != FrozenRuntimeVersion)
            return base::Result<ManifestBuild, std::string>::Failure(
                item.name + " escaped the frozen schema/runtime version");

        auto viewResult = d3d::D3D12PackageView::Decode(frozen);
        if (!viewResult)
            return base::Result<ManifestBuild, std::string>::Failure(
                item.name + " D3D12 schema failed: " + viewResult.Error().message);
        const auto& view = viewResult.Value();

        output << "case=" << item.name
               << "|semantic=" << base::ToHex(semantic.Value())
               << "|bytes=" << frozen.FileBytes().size()
               << "|file=" << base::ToHex(frozen.Header().fileDigest)
               << "|execution=" << base::ToHex(frozen.Header().executionDigest)
               << "|profile=" << base::ToHex(frozen.Header().targetProfileDigest)
               << "|sections=" << frozen.Sections().size()
               << "|resources=" << view.Resources().size()
               << "|allocations=" << view.Allocations().size()
               << "|views=" << view.Views().size()
               << "|shaders=" << view.Shaders().size()
               << "|programs=" << view.Programs().size()
               << "|layouts=" << view.BindingLayouts().size()
               << "|raster=" << view.RasterCommands().size()
               << "|compute=" << view.ComputeCommands().size()
               << "|dynamic=" << view.DynamicSlots().size()
               << "|external=" << view.ExternalSlots().size()
               << "|surface=" << view.SurfaceSlots().size()
               << "|load-ops=" << view.LoadOperations().size()
               << "|frame-ops=" << view.FrameOperations().size()
               << '\n';
    }

    const auto body = output.str();
    const auto bodyBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(body.data()), body.size());
    const auto aggregate = base::ToHex(base::Sha256(bodyBytes));
    output << "qualification-aggregate=" << aggregate << '\n';
    return base::Result<ManifestBuild, std::string>::Success(
        {output.str(), aggregate, corpusDigest.Value().value});
}

base::Result<void, std::string> WriteText(const std::filesystem::path& path, std::string_view text)
{
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
    return base::WriteAllBytes(path, bytes);
}

#if defined(_WIN32)
base::Result<void, std::string> VerifyFreshProcess(std::string_view parentManifest)
{
    std::array<wchar_t, 32768> executableBuffer{};
    const auto length = GetModuleFileNameW(nullptr, executableBuffer.data(),
        static_cast<DWORD>(executableBuffer.size()));
    if (length == 0 || length >= executableBuffer.size())
        return base::Result<void, std::string>::Failure("GetModuleFileNameW failed");
    const std::filesystem::path executable(std::wstring(executableBuffer.data(), length));
    const auto manifestPath = std::filesystem::temp_directory_path() /
        (L"sge2_level2_freeze_child_" + std::to_wstring(GetCurrentProcessId()) + L".txt");

    std::wstring command = L"\"" + executable.wstring() + L"\" --emit-manifest \"" +
                           manifestPath.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, executable.parent_path().c_str(), &startup, &process))
        return base::Result<void, std::string>::Failure("fresh-process CreateProcessW failed");

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0)
    {
        std::error_code ignored;
        std::filesystem::remove(manifestPath, ignored);
        return base::Result<void, std::string>::Failure(
            "fresh-process manifest child failed with code " + std::to_string(exitCode));
    }

    auto childBytes = base::ReadAllBytes(manifestPath);
    std::error_code ignored;
    std::filesystem::remove(manifestPath, ignored);
    if (!childBytes) return base::Result<void, std::string>::Failure(childBytes.Error());
    const auto parentBytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(parentManifest.data()), parentManifest.size());
    if (!std::equal(parentBytes.begin(), parentBytes.end(),
                    childBytes.Value().begin(), childBytes.Value().end()))
        return base::Result<void, std::string>::Failure(
            "fresh-process Compiler manifest differs from the parent process");
    return base::Result<void, std::string>::Success();
}
#endif
}

int main(int argc, char** argv)
{
    const std::string_view command = argc >= 2 ? std::string_view(argv[1]) : std::string_view{};
    if (command == "--print-corpus-digest")
    {
        auto corpus = BuildCorpus();
        if (!corpus) { std::cerr << corpus.Error() << '\n'; return 1; }
        auto digest = SemanticCorpusDigest(corpus.Value());
        if (!digest) { std::cerr << digest.Error() << '\n'; return 2; }
        std::cout << digest.Value().value << '\n';
        return 0;
    }

    const bool emitManifest = command == "--emit-manifest";
    if (emitManifest && argc < 3)
    {
        std::cerr << "--emit-manifest requires an output path\n";
        return 1;
    }

    auto built = BuildManifest(!emitManifest);
    if (!built)
    {
        std::cerr << "Stage-M final freeze failed: " << built.Error() << '\n';
        return 2;
    }

    if (emitManifest)
    {
        auto written = WriteText(std::filesystem::path(argv[2]), built.Value().text);
        if (!written) { std::cerr << written.Error() << '\n'; return 3; }
        return 0;
    }

#if defined(_WIN32)
    auto fresh = VerifyFreshProcess(built.Value().text);
    if (!fresh)
    {
        std::cerr << "Stage-M fresh-process qualification failed: " << fresh.Error() << '\n';
        return 4;
    }
#else
    std::cout << "  fresh-process comparison skipped on non-Windows host.\n";
#endif

    std::cout << "Stage-M Level 2 final freeze...\n";
    std::cout << "  fixed accepted corpus: " << FrozenCorpusCount
              << " Packages, schema/runtime v" << FrozenSchemaVersion << ".\n";
    std::cout << "  semantic corpus digest: " << built.Value().semanticCorpusDigest << '\n';
    std::cout << "  qualification aggregate: " << built.Value().aggregateDigest << '\n';
#if defined(_WIN32)
    std::cout << "  same-process and fresh-process Compiler manifests are byte-identical.\n";
#endif
    std::cout << "Stage-M per-configuration final-freeze qualification passed.\n";
    return 0;
}
