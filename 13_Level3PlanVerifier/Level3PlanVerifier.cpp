#include "Level3PlanVerifier.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace sge::level3::verification
{
namespace
{
void Add(VerificationReport& report, DiagnosticCode code, std::string stage,
         std::string message, std::uint32_t work = base::InvalidIndex,
         std::uint32_t resource = base::InvalidIndex, std::uint32_t use = base::InvalidIndex)
{
    report.violations.push_back({code, std::move(stage), std::move(message), work, resource, use});
}

bool QueueAvailable(const D3D12PlanningContract& contract, QueueClass queueClass, std::uint32_t index)
{
    if (queueClass == QueueClass::Direct) return index < contract.directQueueCount;
    if (queueClass == QueueClass::Compute) return index < contract.computeQueueCount;
    return index < contract.copyQueueCount;
}

bool QueueCanExecute(semantic::WorkKind kind, QueueClass queueClass)
{
    if (kind == semantic::WorkKind::Raster || kind == semantic::WorkKind::Present)
        return queueClass == QueueClass::Direct;
    if (kind == semantic::WorkKind::Compute)
        return queueClass == QueueClass::Direct || queueClass == QueueClass::Compute;
    return queueClass == QueueClass::Direct || queueClass == QueueClass::Copy;
}

bool SameQueue(const WorkQueueAssignment& left, const WorkQueueAssignment& right)
{
    return left.queueClass == right.queueClass && left.queueIndex == right.queueIndex;
}

// Deliberately duplicated from neither Planner nor Package lowering. This is
// the verifier's independent D3D12 v1 state oracle.
AbstractState ExpectedState(semantic::ViewRole role, semantic::WorkKind workKind)
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
}

VerificationReport Verify(const SemanticObligation& obligation,
                          const D3D12PlanningContract& contract,
                          const ExecutionPlanIR& plan)
{
    VerificationReport report;
    if (plan.identity != ComputePlanIdentity(plan))
        Add(report, DiagnosticCode::PlanIdentityMismatch, "identity", "PlanIdentity does not match canonical Plan bytes");

    std::map<std::uint32_t, const ObligationWork*> works;
    for (const auto& work : obligation.works) works[work.id.value] = &work;
    std::map<std::uint32_t, std::uint32_t> positions;
    for (std::uint32_t position = 0; position < plan.workSchedule.size(); ++position)
    {
        const auto id = plan.workSchedule[position].value;
        if (!works.contains(id)) Add(report, DiagnosticCode::UnknownWork, "schedule", "schedule contains an unknown Work", id);
        if (!positions.emplace(id, position).second) Add(report, DiagnosticCode::DuplicateWork, "schedule", "Work is scheduled more than once", id);
    }
    for (const auto& [id, work] : works)
        if (!positions.contains(id)) Add(report, DiagnosticCode::MissingWork, "schedule", "Work is not scheduled", id);
    for (const auto& dependency : obligation.dependencies)
    {
        const auto producer = positions.find(dependency.producer.value);
        const auto consumer = positions.find(dependency.consumer.value);
        if (producer != positions.end() && consumer != positions.end() && producer->second >= consumer->second)
            Add(report, DiagnosticCode::DependencyOrderViolation, "schedule", "dependency is reversed by the schedule",
                dependency.consumer.value, dependency.resource.value);
    }

    std::map<std::uint32_t, WorkQueueAssignment> assignments;
    for (const auto& assignment : plan.queueAssignments)
    {
        if (!works.contains(assignment.work.value))
        {
            Add(report, DiagnosticCode::UnknownWork, "queue", "queue assignment references an unknown Work", assignment.work.value);
            continue;
        }
        if (!assignments.emplace(assignment.work.value, assignment).second)
            Add(report, DiagnosticCode::DuplicateQueueAssignment, "queue", "Work has multiple queue assignments", assignment.work.value);
        if (!QueueAvailable(contract, assignment.queueClass, assignment.queueIndex) ||
            !QueueCanExecute(works.at(assignment.work.value)->kind, assignment.queueClass))
            Add(report, DiagnosticCode::QueueCapabilityViolation, "queue", "Queue cannot execute the assigned Work",
                assignment.work.value);
    }
    for (const auto& [id, work] : works)
        if (!assignments.contains(id)) Add(report, DiagnosticCode::MissingQueueAssignment, "queue", "Work has no queue assignment", id);

    std::map<std::uint32_t, std::uint32_t> producerSignals;
    std::set<std::pair<std::uint32_t, std::uint32_t>> synchronized;
    for (const auto& edge : plan.synchronization)
    {
        const auto producer = positions.find(edge.producer.value);
        const auto consumer = positions.find(edge.consumer.value);
        if (producer == positions.end() || consumer == positions.end() || producer->second >= consumer->second ||
            edge.signalPoint == base::InvalidIndex)
        {
            Add(report, DiagnosticCode::InvalidSynchronization, "synchronization", "invalid Signal/Wait edge",
                edge.consumer.value);
            continue;
        }
        if (edge.signalPoint != producer->second)
            Add(report, DiagnosticCode::InvalidSynchronization, "synchronization",
                "SignalPoint is not the canonical producer schedule position", edge.producer.value);
        const auto producerQueue = assignments.find(edge.producer.value);
        const auto consumerQueue = assignments.find(edge.consumer.value);
        if (producerQueue != assignments.end() && consumerQueue != assignments.end() &&
            SameQueue(producerQueue->second, consumerQueue->second))
            Add(report, DiagnosticCode::InvalidSynchronization, "synchronization",
                "same-Queue Works must not carry an explicit Signal/Wait edge", edge.consumer.value);
        const auto existing = producerSignals.find(edge.producer.value);
        if (existing != producerSignals.end() && existing->second != edge.signalPoint)
            Add(report, DiagnosticCode::InvalidSynchronization, "synchronization", "one producer uses multiple SignalPoints",
                edge.producer.value);
        producerSignals[edge.producer.value] = edge.signalPoint;
        if (!synchronized.emplace(edge.producer.value, edge.consumer.value).second)
            Add(report, DiagnosticCode::InvalidSynchronization, "synchronization", "duplicate Signal/Wait edge",
                edge.consumer.value);
    }
    for (const auto& dependency : obligation.dependencies)
    {
        const auto producerQueue = assignments.find(dependency.producer.value);
        const auto consumerQueue = assignments.find(dependency.consumer.value);
        if (producerQueue == assignments.end() || consumerQueue == assignments.end()) continue;
        const bool sameQueue = SameQueue(producerQueue->second, consumerQueue->second);
        if (!sameQueue && !synchronized.contains({dependency.producer.value, dependency.consumer.value}))
            Add(report, DiagnosticCode::MissingSynchronization, "synchronization", "cross-Queue dependency has no Signal/Wait edge",
                dependency.consumer.value, dependency.resource.value);
    }

    std::map<std::uint32_t, const ObligationUse*> uses;
    for (const auto& use : obligation.uses) uses[use.id.value] = &use;
    std::map<std::uint32_t, const ObligationResource*> resources;
    for (const auto& resource : obligation.resources) resources[resource.id.value] = &resource;
    std::map<std::uint32_t, ResourceInstancePlan> resourcePlans;
    for (const auto& resourcePlan : plan.resourceInstances)
    {
        if (!resources.contains(resourcePlan.resource.value) ||
            !resourcePlans.emplace(resourcePlan.resource.value, resourcePlan).second)
        {
            Add(report, DiagnosticCode::DuplicateResourcePlan, "resource", "unknown or duplicate Resource instance plan",
                base::InvalidIndex, resourcePlan.resource.value);
            continue;
        }
        const auto& resource = *resources.at(resourcePlan.resource.value);
        const std::uint32_t requiredInstances = resource.kind == semantic::ResourceKind::SurfaceImage ? 0u :
            resource.lifetime == semantic::LifetimeIntent::External ? 1u :
            (resource.lifetime == semantic::LifetimeIntent::FrameLocal || resource.lifetime == semantic::LifetimeIntent::Temporal) ?
                contract.framesInFlight : 1u;
        if (resourcePlan.physicalInstanceCount != requiredInstances)
            Add(report, DiagnosticCode::ResourceInstanceViolation, "resource", "physical instance law is violated",
                base::InvalidIndex, resource.id.value);

        std::uint32_t first = base::InvalidIndex;
        std::uint32_t last = base::InvalidIndex;
        for (const auto& use : obligation.uses)
        {
            if (use.resource != resource.id) continue;
            const auto position = positions.find(use.owner.value);
            if (position == positions.end()) continue;
            first = first == base::InvalidIndex ? position->second : std::min(first, position->second);
            last = last == base::InvalidIndex ? position->second : std::max(last, position->second);
        }
        if (resourcePlan.firstUse != first || resourcePlan.lastUse != last)
            Add(report, DiagnosticCode::LifetimeViolation, "lifetime", "Resource lifetime interval does not match the schedule",
                base::InvalidIndex, resource.id.value);
    }
    for (const auto& [id, resource] : resources)
        if (!resourcePlans.contains(id)) Add(report, DiagnosticCode::DuplicateResourcePlan, "resource", "Resource has no instance plan", base::InvalidIndex, id);

    std::map<std::uint32_t, const AllocationPlan*> allocations;
    std::map<std::uint32_t, std::uint32_t> allocationCoverage;
    std::map<std::uint32_t, std::uint32_t> allocationForResource;
    for (const auto& allocation : plan.allocations)
    {
        if (allocation.id == base::InvalidIndex || !allocations.emplace(allocation.id, &allocation).second)
            Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation", "invalid or duplicate Allocation identity");
        if (allocation.resources.empty())
            Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation", "Allocation covers no Resource");
        if (allocation.kind == PlanAllocationKind::Committed && allocation.resources.size() != 1)
            Add(report, DiagnosticCode::AllocationAliasViolation, "allocation", "Committed Allocation covers multiple Resources");
        if (allocation.kind == PlanAllocationKind::Placed && allocation.resources.size() == 1 &&
            !contract.standalonePlacedAllocation)
            Add(report, DiagnosticCode::AllocationAliasViolation, "allocation",
                "planning contract cannot represent a standalone Placed Allocation");
        std::uint64_t requiredSize = 0;
        std::uint32_t requiredInstances = 0;
        for (const auto resourceId : allocation.resources)
        {
            if (!resources.contains(resourceId.value) || ++allocationCoverage[resourceId.value] != 1)
            {
                Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation", "Resource has invalid Allocation coverage",
                    base::InvalidIndex, resourceId.value);
                continue;
            }
            allocationForResource[resourceId.value] = allocation.id;
            const auto& resource = *resources.at(resourceId.value);
            if (resource.lifetime == semantic::LifetimeIntent::External || resource.kind == semantic::ResourceKind::SurfaceImage)
                Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation", "External or Surface Resource is Package allocated",
                    base::InvalidIndex, resourceId.value);
            requiredSize = std::max(requiredSize, resource.sizeBytes);
            const auto resourcePlan = resourcePlans.find(resourceId.value);
            if (resourcePlan != resourcePlans.end())
            {
                requiredInstances = std::max(requiredInstances, resourcePlan->second.physicalInstanceCount);
                if (resourcePlan->second.allocation != allocation.id)
                    Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation",
                        "Resource instance Allocation reference does not name the covering Allocation",
                        base::InvalidIndex, resourceId.value);
            }
        }
        if (allocation.sizeBytes < requiredSize || allocation.physicalInstanceCount != requiredInstances)
            Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation", "Allocation size or instance coverage is insufficient");
        for (std::size_t left = 0; left < allocation.resources.size(); ++left)
            for (std::size_t right = left + 1; right < allocation.resources.size(); ++right)
            {
                const auto a = resourcePlans.find(allocation.resources[left].value);
                const auto b = resourcePlans.find(allocation.resources[right].value);
                if (a == resourcePlans.end() || b == resourcePlans.end()) continue;
                const bool overlap = a->second.firstUse != base::InvalidIndex && b->second.firstUse != base::InvalidIndex &&
                    !(a->second.lastUse < b->second.firstUse || b->second.lastUse < a->second.firstUse);
                const auto& ar = *resources.at(allocation.resources[left].value);
                const auto& br = *resources.at(allocation.resources[right].value);
                const bool compatible = ar.aliasCompatibleWith == br.id || br.aliasCompatibleWith == ar.id;
                if (overlap || !compatible || ar.kind != br.kind || allocation.aliasGroup == base::InvalidIndex)
                    Add(report, DiagnosticCode::AllocationAliasViolation, "alias", "Aliased Resources are overlapping or incompatible");
            }
    }
    for (const auto& [id, resource] : resources)
    {
        const bool packageOwned = resource->lifetime != semantic::LifetimeIntent::External && resource->kind != semantic::ResourceKind::SurfaceImage;
        if (packageOwned && allocationCoverage[id] != 1)
            Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation", "Package Resource is not covered exactly once", base::InvalidIndex, id);
        if (!packageOwned && allocationCoverage[id] != 0)
            Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation", "non-Package Resource has Allocation coverage", base::InvalidIndex, id);
        const auto planFound = resourcePlans.find(id);
        if (planFound != resourcePlans.end())
        {
            if (packageOwned)
            {
                const auto covering = allocationForResource.find(id);
                if (covering == allocationForResource.end() || planFound->second.allocation != covering->second)
                    Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation",
                        "Resource Allocation reference and Allocation coverage are not bidirectionally identical",
                        base::InvalidIndex, id);
            }
            else if (planFound->second.allocation != base::InvalidIndex)
            {
                Add(report, DiagnosticCode::AllocationCoverageViolation, "allocation",
                    "non-Package Resource carries an Allocation reference", base::InvalidIndex, id);
            }
        }
    }

    std::map<std::uint32_t, AbstractState> states;
    for (const auto& state : plan.useStates)
    {
        if (!uses.contains(state.use.value) || !states.emplace(state.use.value, state.required).second)
            Add(report, DiagnosticCode::StatePlanViolation, "state", "unknown or duplicate ResourceUse state", base::InvalidIndex, base::InvalidIndex, state.use.value);
        else if (state.required != ExpectedState(uses.at(state.use.value)->role, works.at(uses.at(state.use.value)->owner.value)->kind))
            Add(report, DiagnosticCode::StatePlanViolation, "state", "ResourceUse required state is incorrect", base::InvalidIndex, uses.at(state.use.value)->resource.value, state.use.value);
    }
    for (const auto& [id, use] : uses)
        if (!states.contains(id)) Add(report, DiagnosticCode::StatePlanViolation, "state", "ResourceUse has no state plan", use->owner.value, use->resource.value, id);

    using BindingKey = std::pair<std::uint32_t, std::uint32_t>;
    std::map<BindingKey, const BindingPlan*> actualBindings;
    for (const auto& binding : plan.bindings)
    {
        const BindingKey key{binding.program.value, binding.parameter.value};
        if (!actualBindings.emplace(key, &binding).second)
            Add(report, DiagnosticCode::BindingPlanViolation, "binding", "duplicate binding plan");
    }
    std::set<BindingKey> requiredBindings;
    std::uint32_t expectedRoot = 0;
    std::uint32_t expectedDescriptor = 0;
    std::uint32_t previousProgram = base::InvalidIndex;
    for (const auto& parameter : obligation.parameters)
    {
        const BindingKey key{parameter.program.value, parameter.id.value};
        requiredBindings.insert(key);
        if (parameter.program.value != previousProgram)
        {
            previousProgram = parameter.program.value;
            expectedRoot = 0;
        }
        const auto found = actualBindings.find(key);
        if (found == actualBindings.end())
        {
            Add(report, DiagnosticCode::BindingPlanViolation, "binding", "binding plan is incomplete");
            ++expectedRoot;
            if (parameter.kind != semantic::ProgramParameterKind::ConstantBuffer) ++expectedDescriptor;
            continue;
        }
        const auto& binding = *found->second;
        if (binding.rootParameterIndex != expectedRoot)
            Add(report, DiagnosticCode::BindingPlanViolation, "binding",
                "rootParameterIndex is not the canonical Program-local index");
        const bool descriptorBacked = parameter.kind != semantic::ProgramParameterKind::ConstantBuffer;
        const auto requiredDescriptor = descriptorBacked ? expectedDescriptor : base::InvalidIndex;
        if (binding.descriptorIndex != requiredDescriptor)
            Add(report, DiagnosticCode::BindingPlanViolation, "binding",
                "descriptorIndex is not the canonical Level 3 v1 binding index");
        ++expectedRoot;
        if (descriptorBacked) ++expectedDescriptor;
    }
    for (const auto& [key, binding] : actualBindings)
        if (!requiredBindings.contains(key))
            Add(report, DiagnosticCode::BindingPlanViolation, "binding", "binding plan references an unknown ProgramParameter");

    std::vector<semantic::ResourceId> expectedExternal;
    std::vector<semantic::ResourceId> expectedPresent;
    for (const auto& resource : obligation.resources)
    {
        if (resource.lifetime == semantic::LifetimeIntent::External && resource.kind != semantic::ResourceKind::SurfaceImage)
            expectedExternal.push_back(resource.id);
        if (resource.kind == semantic::ResourceKind::SurfaceImage) expectedPresent.push_back(resource.id);
    }
    if (plan.externalBoundaries != expectedExternal || plan.presentBoundaries != expectedPresent)
        Add(report, DiagnosticCode::BoundaryViolation, "boundary", "External or Present boundary set is incomplete");

    if (plan.resourceInstances.size() > contract.maximumResources || plan.allocations.size() > contract.maximumAllocations ||
        plan.useStates.size() > contract.maximumViews)
        Add(report, DiagnosticCode::ArtifactCardinalityViolation, "cardinality", "Plan exceeds the planning contract cardinality bound");

    report.verified = report.violations.empty();
    return report;
}

base::Result<VerifiedExecutionPlan, VerificationReport> VerifyAndSeal(
    const SemanticObligation& obligation,
    const D3D12PlanningContract& contract,
    const ExecutionPlanIR& plan)
{
    auto report = Verify(obligation, contract, plan);
    if (!report.verified)
        return base::Result<VerifiedExecutionPlan, VerificationReport>::Failure(std::move(report));
    return base::Result<VerifiedExecutionPlan, VerificationReport>::Success(VerifiedExecutionPlan(plan));
}
}
