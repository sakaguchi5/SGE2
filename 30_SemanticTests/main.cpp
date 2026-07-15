#include "../03_SemanticBuilder/SemanticBuilder.h"
#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../20_ClassicalFrontend/ClassicalTriangle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>

int main()
{
    namespace sem = sge::semantic;
    auto graphResult = sge::classical::BuildTriangleGraph();
    if (!graphResult) { std::cerr << graphResult.Error() << '\n'; return 1; }
    const auto& graph = graphResult.Value();
    auto analyzed = sge::analysis::Analyze(graph);
    if (!analyzed) { std::cerr << "valid Slice-15 retained graph was rejected\n"; return 2; }

    if (graph.works.size() != 3 || graph.programs.size() != 2 || analyzed.Value().canonicalWorkOrder.size() != 3) return 3;
    if (graph.works[0].kind != sem::WorkKind::Raster || graph.works[1].kind != sem::WorkKind::Compute || graph.works[2].kind != sem::WorkKind::Copy ||
        analyzed.Value().canonicalWorkOrder[0] != graph.works[2].id || analyzed.Value().canonicalWorkOrder[1] != graph.works[1].id ||
        analyzed.Value().canonicalWorkOrder[2] != graph.works[0].id)
        return 4;

    const auto& rasterWork = graph.works[0];
    const auto& computeWork = graph.works[1];
    const auto& copyWork = graph.works[2];
    const auto& computePrevious = graph.resourceUses[computeWork.compute.previous.value];
    const auto& computeWrite = graph.resourceUses[computeWork.compute.output.value];
    const auto& rasterComputedRead = graph.resourceUses[rasterWork.raster.computedData.value];
    const auto& copySource = graph.resourceUses[copyWork.copy.source.value];
    const auto& copyDestination = graph.resourceUses[copyWork.copy.destination.value];
    const auto& rasterCopiedRead = graph.resourceUses[rasterWork.raster.copiedData.value];
    const auto& aliasedRead = graph.resourceUses[rasterWork.raster.aliasedData.value];
    const auto& externalRead = graph.resourceUses[rasterWork.raster.externalData.value];
    if (computePrevious.role != sem::ViewRole::TemporalPreviousBuffer || computePrevious.effect != sem::Effect::Read ||
        computeWrite.role != sem::ViewRole::StorageBuffer || computeWrite.effect != sem::Effect::Write || computePrevious.resource != computeWrite.resource ||
        rasterComputedRead.role != sem::ViewRole::ComputedBuffer || rasterComputedRead.effect != sem::Effect::Read ||
        copySource.role != sem::ViewRole::CopySource || copyDestination.role != sem::ViewRole::CopyDestination ||
        rasterCopiedRead.role != sem::ViewRole::CopiedBuffer || aliasedRead.role != sem::ViewRole::AliasedBuffer || aliasedRead.effect != sem::Effect::Read ||
        externalRead.role != sem::ViewRole::ExternalBuffer || externalRead.effect != sem::Effect::Read)
        return 5;

    const auto prepIt = std::find_if(graph.resources.begin(), graph.resources.end(), [](const auto& r) { return r.lifetime == sem::LifetimeIntent::Preparation; });
    if (prepIt == graph.resources.end()) return 6;
    const auto& aliasedResource = graph.resources[aliasedRead.resource.value];
    if (prepIt->kind != sem::ResourceKind::Buffer || prepIt->update != sem::UpdateIntent::Immutable || prepIt->initialContent.size() != 16 ||
        aliasedResource.kind != sem::ResourceKind::Buffer || aliasedResource.update != sem::UpdateIntent::Immutable ||
        aliasedResource.lifetime != sem::LifetimeIntent::Persistent || aliasedResource.initialContent.size() != 16 ||
        prepIt->buffer.sizeBytes != aliasedResource.buffer.sizeBytes || prepIt->buffer.strideBytes != aliasedResource.buffer.strideBytes)
        return 7;
    for (const auto& use : graph.resourceUses)
        if (use.resource == prepIt->id) { std::cerr << "Preparation resource leaked into a Work use\n"; return 8; }
    const auto& externalResource = graph.resources[externalRead.resource.value];
    if (externalResource.kind != sem::ResourceKind::Buffer || externalResource.lifetime != sem::LifetimeIntent::External ||
        externalResource.update != sem::UpdateIntent::External || externalResource.visibility != sem::Visibility::Published ||
        externalResource.buffer.sizeBytes != 16 || externalResource.buffer.strideBytes != 16 || !externalResource.initialContent.empty())
        return 9;

    const auto rasterProgram = std::find_if(graph.programs.begin(), graph.programs.end(), [](const auto& p) { return p.kind == sem::ProgramKind::Raster; });
    const auto computeProgram = std::find_if(graph.programs.begin(), graph.programs.end(), [](const auto& p) { return p.kind == sem::ProgramKind::Compute; });
    if (rasterProgram == graph.programs.end() || computeProgram == graph.programs.end() ||
        rasterProgram->interface.sampledTextureCount != 1 || rasterProgram->interface.sampledBufferCount != 4 ||
        computeProgram->interface.sampledBufferCount != 1 || computeProgram->interface.unorderedBufferCount != 1)
        return 9;

    auto wrongPreparationLifetime = graph;
    wrongPreparationLifetime.resources[prepIt->id.value].lifetime = sem::LifetimeIntent::Persistent;
    if (sge::analysis::Analyze(wrongPreparationLifetime)) { std::cerr << "missing Preparation lifetime was accepted\n"; return 10; }

    auto incompatibleAlias = graph;
    incompatibleAlias.resources[prepIt->id.value].buffer.sizeBytes = 32;
    if (sge::analysis::Analyze(incompatibleAlias)) { std::cerr << "incompatible alias resources were accepted\n"; return 11; }

    auto preparationReferenced = graph;
    preparationReferenced.resourceUses[rasterWork.raster.aliasedData.value].resource = prepIt->id;
    if (sge::analysis::Analyze(preparationReferenced)) { std::cerr << "Preparation resource referenced by raster was accepted\n"; return 12; }

    auto wrongTemporalLifetime = graph;
    wrongTemporalLifetime.resources[computeWrite.resource.value].lifetime = sem::LifetimeIntent::FrameLocal;
    if (sge::analysis::Analyze(wrongTemporalLifetime)) return 13;

    auto reordered = graph;
    std::reverse(reordered.resources.begin(), reordered.resources.end());
    std::reverse(reordered.resourceUses.begin(), reordered.resourceUses.end());
    std::reverse(reordered.programs.begin(), reordered.programs.end());
    std::reverse(reordered.works.begin(), reordered.works.end());
    auto reorderedAnalysis = sge::analysis::Analyze(reordered);
    if (!reorderedAnalysis || reorderedAnalysis.Value().canonicalWorkOrder != analyzed.Value().canonicalWorkOrder ||
        reorderedAnalysis.Value().canonicalResourceOrder != analyzed.Value().canonicalResourceOrder ||
        reorderedAnalysis.Value().canonicalResourceUseOrder != analyzed.Value().canonicalResourceUseOrder)
    {
        std::cerr << "canonical analysis depends on vector order\n";
        return 14;
    }

    const auto dependencyExists = [&](sem::WorkId producer, sem::WorkId consumer) {
        return std::any_of(analyzed.Value().dependencies.begin(), analyzed.Value().dependencies.end(),
            [&](const auto& edge) { return edge.producer == producer && edge.consumer == consumer; });
    };
    if (!dependencyExists(copyWork.id, rasterWork.id) || !dependencyExists(computeWork.id, rasterWork.id))
    {
        std::cerr << "ResourceUse hazards did not produce Work dependencies\n";
        return 15;
    }
    const auto temporalLifetime = std::find_if(analyzed.Value().resourceLifetimes.begin(),
        analyzed.Value().resourceLifetimes.end(), [&](const auto& lifetime) {
            return lifetime.resource == computeWrite.resource;
        });
    if (temporalLifetime == analyzed.Value().resourceLifetimes.end() || !temporalLifetime->usedByWork ||
        temporalLifetime->firstUse >= temporalLifetime->lastUse)
        return 16;

    auto cyclic = graph;
    cyclic.works[computeWork.id.value].dependencies.push_back(rasterWork.id);
    cyclic.works[rasterWork.id.value].dependencies.push_back(computeWork.id);
    if (sge::analysis::Analyze(cyclic))
    {
        std::cerr << "dependency cycle was accepted\n";
        return 17;
    }

    sem::SemanticBuilder invalidBuilder;
    constexpr std::array<std::byte, 4> invalidDepthBytes{};
    if (invalidBuilder.AddImmutableTexture2D("ForbiddenDepthUpload", 1, 1, sem::FormatMeaning::Depth32Float, 4, invalidDepthBytes)) return 18;

    sem::SemanticGraph empty;
    if (sge::analysis::Analyze(empty)) return 19;
    std::cout << "Generic ID, dependency, cycle, lifetime, and resource-contract analysis tests passed.\n";
    return 0;
}
