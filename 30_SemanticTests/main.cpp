#include "../03_SemanticBuilder/SemanticBuilder.h"
#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../20_ClassicalFrontend/ClassicalTriangle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>

namespace
{
namespace sem = sge::semantic;

const sem::ResourceUse& UseForOperand(
    const sem::SemanticGraph& graph,
    const sem::Work& work,
    sem::WorkOperandKind kind,
    sem::ProgramParameterId parameter = {})
{
    const auto found = std::find_if(work.operands.begin(), work.operands.end(), [&](const auto& operand) {
        return operand.kind == kind &&
               (kind != sem::WorkOperandKind::ProgramParameter || operand.parameter == parameter);
    });
    return graph.resourceUses[found->use.value];
}
}

int main()
{
    auto graphResult = sge::classical::BuildTriangleGraph();
    if (!graphResult) { std::cerr << graphResult.Error() << '\n'; return 1; }
    const auto& graph = graphResult.Value();
    auto analyzed = sge::analysis::Analyze(graph);
    if (!analyzed) { std::cerr << "valid Stage-D operand graph was rejected\n"; return 2; }

    if (graph.works.size() != 3 || graph.programs.size() != 2 || analyzed.Value().canonicalWorkOrder.size() != 3) return 3;
    if (graph.works[0].kind != sem::WorkKind::Raster || graph.works[1].kind != sem::WorkKind::Compute || graph.works[2].kind != sem::WorkKind::Copy ||
        analyzed.Value().canonicalWorkOrder[0] != graph.works[2].id || analyzed.Value().canonicalWorkOrder[1] != graph.works[1].id ||
        analyzed.Value().canonicalWorkOrder[2] != graph.works[0].id)
        return 4;

    const auto& rasterWork = graph.works[0];
    const auto& computeWork = graph.works[1];
    const auto& copyWork = graph.works[2];
    const auto& computePrevious = UseForOperand(graph, computeWork, sem::WorkOperandKind::ProgramParameter, {1});
    const auto& computeWrite = UseForOperand(graph, computeWork, sem::WorkOperandKind::ProgramParameter, {2});
    const auto& rasterComputedRead = UseForOperand(graph, rasterWork, sem::WorkOperandKind::ProgramParameter, {2});
    const auto& copySource = UseForOperand(graph, copyWork, sem::WorkOperandKind::CopySource);
    const auto& copyDestination = UseForOperand(graph, copyWork, sem::WorkOperandKind::CopyDestination);
    const auto& rasterCopiedRead = UseForOperand(graph, rasterWork, sem::WorkOperandKind::ProgramParameter, {3});
    const auto& aliasedRead = UseForOperand(graph, rasterWork, sem::WorkOperandKind::ProgramParameter, {4});
    const auto& externalRead = UseForOperand(graph, rasterWork, sem::WorkOperandKind::ProgramParameter, {5});
    if (computePrevious.role != sem::ViewRole::ShaderBuffer || computePrevious.temporalRelation != sem::TemporalRelation::Previous ||
        computePrevious.effect != sem::Effect::Read ||
        computeWrite.role != sem::ViewRole::StorageBuffer || computeWrite.effect != sem::Effect::Write || computePrevious.resource != computeWrite.resource ||
        rasterComputedRead.role != sem::ViewRole::ShaderBuffer || rasterComputedRead.effect != sem::Effect::Read ||
        copySource.role != sem::ViewRole::CopySource || copyDestination.role != sem::ViewRole::CopyDestination ||
        rasterCopiedRead.role != sem::ViewRole::ShaderBuffer || aliasedRead.role != sem::ViewRole::ShaderBuffer ||
        externalRead.role != sem::ViewRole::ShaderBuffer)
        return 5;

    const auto prepIt = std::find_if(graph.resources.begin(), graph.resources.end(), [](const auto& r) { return r.lifetime == sem::LifetimeIntent::Preparation; });
    if (prepIt == graph.resources.end()) return 6;
    const auto& aliasedResource = graph.resources[aliasedRead.resource.value];
    if (prepIt->kind != sem::ResourceKind::Buffer || prepIt->update != sem::UpdateIntent::Immutable || prepIt->initialContent.size() != 16 ||
        aliasedResource.kind != sem::ResourceKind::Buffer || aliasedResource.update != sem::UpdateIntent::Immutable ||
        aliasedResource.lifetime != sem::LifetimeIntent::Persistent || aliasedResource.initialContent.size() != 16 ||
        aliasedResource.aliasPreparation != prepIt->id ||
        prepIt->buffer.sizeBytes != aliasedResource.buffer.sizeBytes || prepIt->buffer.strideBytes != aliasedResource.buffer.strideBytes)
        return 7;
    for (const auto& use : graph.resourceUses)
        if (use.resource == prepIt->id) { std::cerr << "Preparation resource leaked into a Work operand\n"; return 8; }

    const auto& externalResource = graph.resources[externalRead.resource.value];
    if (externalResource.kind != sem::ResourceKind::Buffer || externalResource.lifetime != sem::LifetimeIntent::External ||
        externalResource.update != sem::UpdateIntent::External || externalResource.visibility != sem::Visibility::Published ||
        externalResource.buffer.sizeBytes != 16 || externalResource.buffer.strideBytes != 16 || !externalResource.initialContent.empty())
        return 9;

    const auto rasterProgram = std::find_if(graph.programs.begin(), graph.programs.end(), [](const auto& p) { return p.kind == sem::ProgramKind::Raster; });
    const auto computeProgram = std::find_if(graph.programs.begin(), graph.programs.end(), [](const auto& p) { return p.kind == sem::ProgramKind::Compute; });
    if (rasterProgram == graph.programs.end() || computeProgram == graph.programs.end() ||
        rasterProgram->interface.parameters.size() != 6 || computeProgram->interface.parameters.size() != 3 ||
        rasterProgram->interface.parameters[0].kind != sem::ProgramParameterKind::ConstantBuffer ||
        rasterProgram->interface.parameters[1].kind != sem::ProgramParameterKind::SampledTexture ||
        rasterProgram->interface.parameters[5].shaderRegister != 4 ||
        computeProgram->interface.parameters[1].kind != sem::ProgramParameterKind::ReadOnlyBuffer ||
        computeProgram->interface.parameters[2].kind != sem::ProgramParameterKind::UnorderedBuffer)
        return 10;

    auto wrongPreparationLifetime = graph;
    wrongPreparationLifetime.resources[prepIt->id.value].lifetime = sem::LifetimeIntent::Persistent;
    if (sge::analysis::Analyze(wrongPreparationLifetime)) { std::cerr << "missing Preparation lifetime was accepted\n"; return 11; }

    auto incompatibleAlias = graph;
    incompatibleAlias.resources[prepIt->id.value].buffer.sizeBytes = 32;
    if (sge::analysis::Analyze(incompatibleAlias)) { std::cerr << "incompatible alias resources were accepted\n"; return 12; }

    auto preparationReferenced = graph;
    preparationReferenced.resourceUses[aliasedRead.id.value].resource = prepIt->id;
    if (sge::analysis::Analyze(preparationReferenced)) { std::cerr << "Preparation resource referenced by raster was accepted\n"; return 13; }

    auto wrongTemporalLifetime = graph;
    wrongTemporalLifetime.resources[computeWrite.resource.value].lifetime = sem::LifetimeIntent::FrameLocal;
    if (sge::analysis::Analyze(wrongTemporalLifetime)) return 14;

    auto missingBinding = graph;
    missingBinding.works[0].operands.erase(std::remove_if(
        missingBinding.works[0].operands.begin(), missingBinding.works[0].operands.end(), [](const auto& operand) {
            return operand.kind == sem::WorkOperandKind::ProgramParameter && operand.parameter.value == 5;
        }), missingBinding.works[0].operands.end());
    if (sge::analysis::Analyze(missingBinding)) { std::cerr << "missing ProgramParameter binding was accepted\n"; return 15; }

    auto sharedUse = graph;
    sharedUse.works[1].operands.push_back(sharedUse.works[0].operands[1]);
    if (sge::analysis::Analyze(sharedUse)) { std::cerr << "ResourceUse shared by multiple Works was accepted\n"; return 16; }

    auto constantMismatch = graph;
    constantMismatch.programs[rasterProgram->id.value].interface.parameters[0].requiredBytes += 16;
    if (sge::analysis::Analyze(constantMismatch)) { std::cerr << "constant-buffer parameter size mismatch was accepted\n"; return 17; }

    auto orphanPreparation = graph;
    orphanPreparation.resources[aliasedResource.id.value].aliasPreparation = {};
    if (sge::analysis::Analyze(orphanPreparation)) { std::cerr << "orphan Preparation resource was accepted\n"; return 18; }

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
        return 19;
    }

    const auto dependencyExists = [&](sem::WorkId producer, sem::WorkId consumer) {
        return std::any_of(analyzed.Value().dependencies.begin(), analyzed.Value().dependencies.end(),
            [&](const auto& edge) { return edge.producer == producer && edge.consumer == consumer; });
    };
    if (!dependencyExists(copyWork.id, rasterWork.id) || !dependencyExists(computeWork.id, rasterWork.id))
    {
        std::cerr << "ResourceUse hazards did not produce Work dependencies\n";
        return 20;
    }
    const auto temporalLifetime = std::find_if(analyzed.Value().resourceLifetimes.begin(),
        analyzed.Value().resourceLifetimes.end(), [&](const auto& lifetime) {
            return lifetime.resource == computeWrite.resource;
        });
    if (temporalLifetime == analyzed.Value().resourceLifetimes.end() || !temporalLifetime->usedByWork ||
        temporalLifetime->firstUse >= temporalLifetime->lastUse)
        return 21;

    auto cyclic = graph;
    cyclic.works[computeWork.id.value].dependencies.push_back(rasterWork.id);
    cyclic.works[rasterWork.id.value].dependencies.push_back(computeWork.id);
    if (sge::analysis::Analyze(cyclic))
    {
        std::cerr << "dependency cycle was accepted\n";
        return 22;
    }

    sem::SemanticBuilder invalidBuilder;
    constexpr std::array<std::byte, 4> invalidDepthBytes{};
    if (invalidBuilder.AddImmutableTexture2D("ForbiddenDepthUpload", 1, 1, sem::FormatMeaning::Depth32Float, 4, invalidDepthBytes)) return 23;

    auto overlappingVertexLayout = graph;
    auto overlappingRasterProgram = std::find_if(overlappingVertexLayout.programs.begin(), overlappingVertexLayout.programs.end(), [](const auto& program) {
        return program.kind == sem::ProgramKind::Raster;
    });
    overlappingRasterProgram->interface.vertexInputs[0].componentCount = 4;
    if (sge::analysis::Analyze(overlappingVertexLayout))
    {
        std::cerr << "overlapping vertex layout was accepted\n";
        return 24;
    }

    auto unusedProgram = graph;
    auto orphan = *rasterProgram;
    orphan.id = {1000};
    orphan.debugName = "UnusedProgram";
    unusedProgram.programs.push_back(orphan);
    if (sge::analysis::Analyze(unusedProgram))
    {
        std::cerr << "unused Program was accepted\n";
        return 25;
    }

    auto missingTemporalPrevious = graph;
    missingTemporalPrevious.works[computeWork.id.value].operands.erase(std::remove_if(
        missingTemporalPrevious.works[computeWork.id.value].operands.begin(),
        missingTemporalPrevious.works[computeWork.id.value].operands.end(), [](const auto& operand) {
            return operand.kind == sem::WorkOperandKind::ProgramParameter && operand.parameter.value == 1;
        }), missingTemporalPrevious.works[computeWork.id.value].operands.end());
    missingTemporalPrevious.programs[computeProgram->id.value].interface.parameters.erase(
        missingTemporalPrevious.programs[computeProgram->id.value].interface.parameters.begin() + 1);
    missingTemporalPrevious.programs[computeProgram->id.value].interface.parameters[1].id = {1};
    missingTemporalPrevious.works[computeWork.id.value].operands[1].parameter = {1};
    if (sge::analysis::Analyze(missingTemporalPrevious))
    {
        std::cerr << "Temporal resource without a previous-generation reader was accepted\n";
        return 26;
    }

    auto invalidSourceEntries = graph;
    auto invalidRasterProgram = std::find_if(invalidSourceEntries.programs.begin(), invalidSourceEntries.programs.end(), [](const auto& program) {
        return program.kind == sem::ProgramKind::Raster;
    });
    invalidRasterProgram->source.computeEntry = "CSMain";
    if (sge::analysis::Analyze(invalidSourceEntries))
    {
        std::cerr << "raster Program with a compute entry was accepted\n";
        return 27;
    }

    sem::SemanticGraph empty;
    if (sge::analysis::Analyze(empty)) return 28;
    std::cout << "Stage-E static completeness plus generic operand, parameter identity, dependency, lifetime, alias, and resource-contract analysis tests passed.\n";
    return 0;
}
