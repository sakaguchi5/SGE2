#include "Level3PlanModel.h"

#include "../00_Base/BinaryIO.h"

#include <algorithm>
#include <map>
#include <set>
#include <span>
#include <tuple>

namespace sge::level3
{
namespace
{
void WriteDigest(base::BinaryWriter& writer, const base::Digest256& digest) { writer.WriteBytes(digest); }
void WriteId(base::BinaryWriter& writer, std::uint32_t id) { writer.WriteU32(id); }
std::uint64_t ResourceSize(const semantic::Resource& resource)
{
    if (resource.kind == semantic::ResourceKind::Buffer) return resource.buffer.sizeBytes;
    // Schema 17 delegates native texture allocation size to D3D12 materialization.
    // Keep the planning value zero so the Plan and frozen AllocationArtifact
    // describe the same contract instead of inventing a linear texture size.
    return 0;
}
}

AbstractState RequiredState(semantic::ViewRole role, semantic::WorkKind workKind)
{
    switch (role)
    {
    case semantic::ViewRole::VertexData: return AbstractState::VertexBuffer;
    case semantic::ViewRole::ConstantData: return AbstractState::ConstantBuffer;
    case semantic::ViewRole::SampledTexture:
    case semantic::ViewRole::ShaderBuffer: return AbstractState::ShaderRead;
    case semantic::ViewRole::StorageBuffer: return AbstractState::UnorderedWrite;
    case semantic::ViewRole::ColorAttachment: return AbstractState::RenderTarget;
    case semantic::ViewRole::PresentSource: return AbstractState::Present;
    case semantic::ViewRole::DepthAttachment: return AbstractState::DepthWrite;
    case semantic::ViewRole::CopySource: return AbstractState::CopySource;
    case semantic::ViewRole::CopyDestination: return AbstractState::CopyDestination;
    }
    return workKind == semantic::WorkKind::Copy ? AbstractState::CopySource : AbstractState::Common;
}

base::Result<SemanticObligation, std::string> BuildSemanticObligation(
    const semantic::SemanticGraph& graph, const analysis::AnalyzedGraph& analyzed)
{
    if (analyzed.source != &graph)
        return base::Result<SemanticObligation, std::string>::Failure("AnalyzedGraph is detached from its SemanticGraph");

    std::map<std::uint32_t, const semantic::Resource*> resources;
    std::map<std::uint32_t, const semantic::ResourceUse*> uses;
    std::map<std::uint32_t, const semantic::Work*> works;
    std::map<std::uint32_t, analysis::ResourceLifetime> lifetimes;
    for (const auto& value : graph.resources) resources[value.id.value] = &value;
    for (const auto& value : graph.resourceUses) uses[value.id.value] = &value;
    for (const auto& value : graph.works) works[value.id.value] = &value;
    for (const auto& value : analyzed.resourceLifetimes) lifetimes[value.resource.value] = value;

    std::map<std::uint32_t, semantic::WorkId> useOwners;
    for (const auto& work : graph.works)
        for (const auto& operand : work.operands) useOwners[operand.use.value] = work.id;

    SemanticObligation output;
    for (const auto id : analyzed.canonicalResourceOrder)
    {
        const auto& source = *resources.at(id.value);
        const auto lifetime = lifetimes.at(id.value);
        output.resources.push_back({source.id, source.kind, source.lifetime, source.update,
            source.visibility, ResourceSize(source),
            source.update == semantic::UpdateIntent::DynamicPerFrame ? 256u : 65536u,
            lifetime.firstUse, lifetime.lastUse, lifetime.usedByWork, source.aliasPreparation});
    }
    for (const auto id : analyzed.canonicalResourceUseOrder)
    {
        const auto& source = *uses.at(id.value);
        const auto owner = useOwners.find(id.value);
        if (owner == useOwners.end())
            return base::Result<SemanticObligation, std::string>::Failure("ResourceUse has no owning Work");
        output.uses.push_back({source.id, source.resource, owner->second, source.effect,
            source.role, source.temporalRelation});
    }
    for (const auto id : analyzed.canonicalWorkOrder)
    {
        const auto& source = *works.at(id.value);
        ObligationWork work{source.id, source.kind, {}};
        for (const auto& operand : source.operands) work.uses.push_back(operand.use);
        std::sort(work.uses.begin(), work.uses.end(), [](auto left, auto right) { return left.value < right.value; });
        output.works.push_back(std::move(work));
    }
    for (const auto& program : graph.programs)
    {
        for (const auto& parameter : program.interface.parameters)
            output.parameters.push_back({program.id, parameter.id, parameter.kind, parameter.stage,
                parameter.shaderRegister, parameter.requiredBytes});
    }
    std::sort(output.parameters.begin(), output.parameters.end(), [](const auto& left, const auto& right) {
        return std::tuple{left.program.value, left.id.value} < std::tuple{right.program.value, right.id.value};
    });
    output.dependencies = analyzed.dependencies;
    std::sort(output.dependencies.begin(), output.dependencies.end(), [](const auto& left, const auto& right) {
        return std::tuple{left.producer.value, left.consumer.value, left.resource.value,
            static_cast<std::uint16_t>(left.kind)} <
            std::tuple{right.producer.value, right.consumer.value, right.resource.value,
            static_cast<std::uint16_t>(right.kind)};
    });
    output.digest = base::Sha256(EncodeCanonical(output));
    return base::Result<SemanticObligation, std::string>::Success(std::move(output));
}

D3D12PlanningContract BuildPlanningContract(const target::D3D12TargetProfile& profile)
{
    D3D12PlanningContract output;
    output.framesInFlight = profile.framesInFlight;
    output.directQueueCount = profile.directQueueCount;
    output.computeQueueCount = profile.computeQueueCount;
    output.copyQueueCount = profile.copyQueueCount;
    output.digest = base::Sha256(EncodeCanonical(output));
    return output;
}

std::vector<std::byte> EncodeCanonical(const SemanticObligation& value)
{
    base::BinaryWriter writer;
    writer.WriteU32(value.version);
    writer.WriteU32(static_cast<std::uint32_t>(value.resources.size()));
    for (const auto& item : value.resources)
    {
        WriteId(writer, item.id.value); writer.WriteU16(static_cast<std::uint16_t>(item.kind));
        writer.WriteU16(static_cast<std::uint16_t>(item.lifetime)); writer.WriteU16(static_cast<std::uint16_t>(item.update));
        writer.WriteU16(static_cast<std::uint16_t>(item.visibility)); writer.WriteU64(item.sizeBytes);
        writer.WriteU32(item.alignment); writer.WriteU32(item.firstUse); writer.WriteU32(item.lastUse);
        writer.WriteU8(item.usedByWork ? 1u : 0u); WriteId(writer, item.aliasCompatibleWith.value);
    }
    writer.WriteU32(static_cast<std::uint32_t>(value.uses.size()));
    for (const auto& item : value.uses)
    {
        WriteId(writer, item.id.value); WriteId(writer, item.resource.value); WriteId(writer, item.owner.value);
        writer.WriteU16(static_cast<std::uint16_t>(item.effect)); writer.WriteU16(static_cast<std::uint16_t>(item.role));
        writer.WriteU16(static_cast<std::uint16_t>(item.temporalRelation));
    }
    writer.WriteU32(static_cast<std::uint32_t>(value.works.size()));
    for (const auto& item : value.works)
    {
        WriteId(writer, item.id.value); writer.WriteU16(static_cast<std::uint16_t>(item.kind));
        writer.WriteU32(static_cast<std::uint32_t>(item.uses.size()));
        for (const auto id : item.uses) WriteId(writer, id.value);
    }
    writer.WriteU32(static_cast<std::uint32_t>(value.parameters.size()));
    for (const auto& item : value.parameters)
    {
        WriteId(writer, item.program.value); WriteId(writer, item.id.value);
        writer.WriteU16(static_cast<std::uint16_t>(item.kind)); writer.WriteU16(static_cast<std::uint16_t>(item.stage));
        writer.WriteU32(item.shaderRegister); writer.WriteU64(item.requiredBytes);
    }
    writer.WriteU32(static_cast<std::uint32_t>(value.dependencies.size()));
    for (const auto& item : value.dependencies)
    {
        WriteId(writer, item.producer.value); WriteId(writer, item.consumer.value); WriteId(writer, item.resource.value);
        writer.WriteU16(static_cast<std::uint16_t>(item.kind));
    }
    return std::move(writer).Take();
}

std::vector<std::byte> EncodeCanonical(const D3D12PlanningContract& value)
{
    base::BinaryWriter writer;
    writer.WriteU32(value.version); writer.WriteU32(value.framesInFlight);
    writer.WriteU32(value.directQueueCount); writer.WriteU32(value.computeQueueCount); writer.WriteU32(value.copyQueueCount);
    writer.WriteU32(value.maximumResources); writer.WriteU32(value.maximumAllocations);
    writer.WriteU32(value.maximumViews); writer.WriteU32(value.maximumOperations);
    writer.WriteU8(value.standalonePlacedAllocation ? 1u : 0u);
    return std::move(writer).Take();
}

std::vector<std::byte> EncodeCanonical(const ExecutionPlanIR& value, bool includeIdentity)
{
    base::BinaryWriter writer;
    writer.WriteU32(value.version); writer.WriteU16(static_cast<std::uint16_t>(value.scheduleStrategy));
    writer.WriteU16(static_cast<std::uint16_t>(value.queueStrategy)); writer.WriteU16(static_cast<std::uint16_t>(value.allocationStrategy));
    writer.WriteU32(static_cast<std::uint32_t>(value.workSchedule.size()));
    for (const auto id : value.workSchedule) WriteId(writer, id.value);
    writer.WriteU32(static_cast<std::uint32_t>(value.queueAssignments.size()));
    for (const auto& item : value.queueAssignments) { WriteId(writer, item.work.value); writer.WriteU16(static_cast<std::uint16_t>(item.queueClass)); writer.WriteU32(item.queueIndex); }
    writer.WriteU32(static_cast<std::uint32_t>(value.synchronization.size()));
    for (const auto& item : value.synchronization) { WriteId(writer, item.producer.value); WriteId(writer, item.consumer.value); writer.WriteU32(item.signalPoint); }
    writer.WriteU32(static_cast<std::uint32_t>(value.resourceInstances.size()));
    for (const auto& item : value.resourceInstances) { WriteId(writer, item.resource.value); writer.WriteU32(item.physicalInstanceCount); writer.WriteU32(item.firstUse); writer.WriteU32(item.lastUse); writer.WriteU32(item.allocation); }
    writer.WriteU32(static_cast<std::uint32_t>(value.allocations.size()));
    for (const auto& item : value.allocations)
    {
        writer.WriteU32(item.id); writer.WriteU16(static_cast<std::uint16_t>(item.kind)); writer.WriteU64(item.sizeBytes);
        writer.WriteU64(item.alignment); writer.WriteU32(item.physicalInstanceCount); writer.WriteU32(item.aliasGroup);
        writer.WriteU32(static_cast<std::uint32_t>(item.resources.size())); for (const auto id : item.resources) WriteId(writer, id.value);
    }
    writer.WriteU32(static_cast<std::uint32_t>(value.useStates.size()));
    for (const auto& item : value.useStates) { WriteId(writer, item.use.value); writer.WriteU16(static_cast<std::uint16_t>(item.required)); }
    writer.WriteU32(static_cast<std::uint32_t>(value.bindings.size()));
    for (const auto& item : value.bindings) { WriteId(writer, item.program.value); WriteId(writer, item.parameter.value); writer.WriteU32(item.rootParameterIndex); writer.WriteU32(item.descriptorIndex); }
    writer.WriteU32(static_cast<std::uint32_t>(value.externalBoundaries.size())); for (const auto id : value.externalBoundaries) WriteId(writer, id.value);
    writer.WriteU32(static_cast<std::uint32_t>(value.presentBoundaries.size())); for (const auto id : value.presentBoundaries) WriteId(writer, id.value);
    if (includeIdentity) WriteDigest(writer, value.identity);
    return std::move(writer).Take();
}

std::vector<std::byte> EncodeCanonical(const ProfileRecord& value)
{
    base::BinaryWriter writer;
    writer.WriteU32(value.version); WriteDigest(writer, value.planIdentity); WriteDigest(writer, value.packageExecutionDigest);
    WriteDigest(writer, value.targetProfileDigest); WriteDigest(writer, value.adapterDriverFingerprint);
    WriteDigest(writer, value.measurementScenarioDigest); writer.WriteU32(value.sampleCount);
    writer.WriteU64(value.measuredNanoseconds); writer.WriteU64(value.observedPeakBytes);
    return std::move(writer).Take();
}

base::Digest256 ComputePlanIdentity(const ExecutionPlanIR& value)
{
    return base::Sha256(EncodeCanonical(value, false));
}
}
