#include "Level3Compiler.h"

#include "../00_Base/BinaryIO.h"
#include "../00_Base/Sha256.h"

#include <algorithm>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <tuple>

namespace sge::compiler::d3d12::level3_compiler
{
namespace
{
constexpr std::uint64_t DefaultAlignment = 64ull * 1024ull;
constexpr std::uint64_t ConstantAlignment = 256;

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

std::uint32_t PhysicalInstances(const level3::ObligationResource& resource,
                                const level3::D3D12PlanningContract& contract)
{
    if (resource.kind == semantic::ResourceKind::SurfaceImage) return 0;
    if (resource.lifetime == semantic::LifetimeIntent::External) return 1;
    if (resource.lifetime == semantic::LifetimeIntent::FrameLocal ||
        resource.lifetime == semantic::LifetimeIntent::Temporal) return contract.framesInFlight;
    return 1;
}

std::map<std::uint32_t, std::uint32_t> Positions(const level3::ExecutionPlanIR& plan)
{
    std::map<std::uint32_t, std::uint32_t> result;
    for (std::uint32_t index = 0; index < plan.workSchedule.size(); ++index)
        result[plan.workSchedule[index].value] = index;
    return result;
}

level3::QueueClass PreferredQueue(semantic::WorkKind kind,
                                  const level3::D3D12PlanningContract& contract,
                                  level3::QueueStrategy strategy)
{
    if (strategy == level3::QueueStrategy::AllDirect || strategy == level3::QueueStrategy::MinimizeCrossQueueEdges)
        return level3::QueueClass::Direct;
    if (kind == semantic::WorkKind::Compute && contract.computeQueueCount != 0) return level3::QueueClass::Compute;
    if (kind == semantic::WorkKind::Copy && contract.copyQueueCount != 0) return level3::QueueClass::Copy;
    return level3::QueueClass::Direct;
}

void BuildResourceAndAllocationPlans(level3::ExecutionPlanIR& plan,
                                     const level3::SemanticObligation& obligation,
                                     const level3::D3D12PlanningContract& contract)
{
    plan.resourceInstances.clear();
    plan.allocations.clear();
    const auto positions = Positions(plan);
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> intervals;
    for (const auto& resource : obligation.resources)
    {
        std::uint32_t first = base::InvalidIndex;
        std::uint32_t last = base::InvalidIndex;
        for (const auto& use : obligation.uses)
        {
            if (use.resource != resource.id) continue;
            const auto found = positions.find(use.owner.value);
            if (found == positions.end()) continue;
            first = first == base::InvalidIndex ? found->second : std::min(first, found->second);
            last = last == base::InvalidIndex ? found->second : std::max(last, found->second);
        }
        intervals[resource.id.value] = {first, last};
        plan.resourceInstances.push_back({resource.id, PhysicalInstances(resource, contract), first, last, base::InvalidIndex});
    }

    std::map<std::uint32_t, const level3::ObligationResource*> resources;
    for (const auto& resource : obligation.resources) resources[resource.id.value] = &resource;
    std::map<std::uint32_t, std::uint32_t> explicitPairs;
    if (plan.allocationStrategy == level3::AllocationStrategy::CanonicalSafe ||
        plan.allocationStrategy == level3::AllocationStrategy::CanonicalAlias ||
        plan.allocationStrategy == level3::AllocationStrategy::MemoryAggressiveAlias)
    {
        for (const auto& resource : obligation.resources)
            if (resource.aliasCompatibleWith.IsValid())
                explicitPairs[resource.aliasCompatibleWith.value] = resource.id.value;
    }

    std::set<std::uint32_t> allocated;
    std::uint32_t nextAliasGroup = 0;
    for (const auto& resource : obligation.resources)
    {
        if (resource.lifetime == semantic::LifetimeIntent::External ||
            resource.kind == semantic::ResourceKind::SurfaceImage || allocated.contains(resource.id.value)) continue;

        std::uint32_t partner = base::InvalidIndex;
        const auto asPreparation = explicitPairs.find(resource.id.value);
        if (asPreparation != explicitPairs.end()) partner = asPreparation->second;
        if (partner == base::InvalidIndex)
        {
            const auto asTarget = std::find_if(explicitPairs.begin(), explicitPairs.end(), [&](const auto& pair) {
                return pair.second == resource.id.value;
            });
            if (asTarget != explicitPairs.end()) partner = asTarget->first;
        }

        const bool alias = partner != base::InvalidIndex;
        level3::AllocationPlan allocation;
        allocation.id = static_cast<std::uint32_t>(plan.allocations.size());
        allocation.kind = plan.allocationStrategy == level3::AllocationStrategy::PlacedNoAlias || alias ?
            level3::PlanAllocationKind::Placed : level3::PlanAllocationKind::Committed;
        allocation.alignment = resource.update == semantic::UpdateIntent::DynamicPerFrame ? ConstantAlignment : DefaultAlignment;
        allocation.physicalInstanceCount = PhysicalInstances(resource, contract);
        allocation.sizeBytes = resource.kind == semantic::ResourceKind::Buffer ?
            (resource.update == semantic::UpdateIntent::DynamicPerFrame ? AlignUp(resource.sizeBytes, ConstantAlignment) :
             (alias ? AlignUp(resource.sizeBytes, DefaultAlignment) : resource.sizeBytes)) : 0;
        allocation.aliasGroup = alias ? nextAliasGroup++ : base::InvalidIndex;
        allocation.resources.push_back(resource.id);
        allocated.insert(resource.id.value);
        if (alias)
        {
            allocation.resources.push_back({partner});
            allocated.insert(partner);
        }
        for (const auto id : allocation.resources)
        {
            const auto found = std::find_if(plan.resourceInstances.begin(), plan.resourceInstances.end(),
                [&](const auto& item) { return item.resource == id; });
            if (found != plan.resourceInstances.end()) found->allocation = allocation.id;
        }
        plan.allocations.push_back(std::move(allocation));
    }
}

void RebuildDerived(level3::ExecutionPlanIR& plan,
                    const level3::SemanticObligation& obligation,
                    const level3::D3D12PlanningContract& contract)
{
    std::map<std::uint32_t, const level3::ObligationWork*> works;
    for (const auto& work : obligation.works) works[work.id.value] = &work;
    plan.queueAssignments.clear();
    for (const auto workId : plan.workSchedule)
        plan.queueAssignments.push_back({workId, PreferredQueue(works.at(workId.value)->kind, contract, plan.queueStrategy), 0});

    std::map<std::uint32_t, level3::WorkQueueAssignment> queues;
    for (const auto& assignment : plan.queueAssignments) queues[assignment.work.value] = assignment;
    const auto positions = Positions(plan);
    plan.synchronization.clear();
    for (const auto& dependency : obligation.dependencies)
    {
        const auto& producer = queues.at(dependency.producer.value);
        const auto& consumer = queues.at(dependency.consumer.value);
        if (producer.queueClass == consumer.queueClass && producer.queueIndex == consumer.queueIndex) continue;
        plan.synchronization.push_back({dependency.producer, dependency.consumer, positions.at(dependency.producer.value)});
    }
    std::sort(plan.synchronization.begin(), plan.synchronization.end(), [](const auto& left, const auto& right) {
        return std::tuple{left.consumer.value, left.producer.value} < std::tuple{right.consumer.value, right.producer.value};
    });

    BuildResourceAndAllocationPlans(plan, obligation, contract);
    plan.useStates.clear();
    for (const auto& use : obligation.uses)
        plan.useStates.push_back({use.id, level3::RequiredState(use.role, works.at(use.owner.value)->kind)});
    plan.bindings.clear();
    std::uint32_t root = 0;
    std::uint32_t previousProgram = base::InvalidIndex;
    std::uint32_t descriptor = 0;
    for (const auto& parameter : obligation.parameters)
    {
        if (parameter.program.value != previousProgram) { previousProgram = parameter.program.value; root = 0; }
        const bool descriptorBacked = parameter.kind != semantic::ProgramParameterKind::ConstantBuffer;
        plan.bindings.push_back({parameter.program, parameter.id, root++, descriptorBacked ? descriptor++ : base::InvalidIndex});
    }
    plan.externalBoundaries.clear();
    plan.presentBoundaries.clear();
    for (const auto& resource : obligation.resources)
    {
        if (resource.lifetime == semantic::LifetimeIntent::External && resource.kind != semantic::ResourceKind::SurfaceImage)
            plan.externalBoundaries.push_back(resource.id);
        if (resource.kind == semantic::ResourceKind::SurfaceImage) plan.presentBoundaries.push_back(resource.id);
    }
    plan.identity = level3::ComputePlanIdentity(plan);
}

void RebuildSynchronizationAndIdentity(level3::ExecutionPlanIR& plan,
                                       const level3::SemanticObligation& obligation)
{
    std::map<std::uint32_t, level3::WorkQueueAssignment> queues;
    for (const auto& assignment : plan.queueAssignments) queues[assignment.work.value] = assignment;
    const auto positions = Positions(plan);
    plan.synchronization.clear();
    for (const auto& dependency : obligation.dependencies)
    {
        const auto& producer = queues.at(dependency.producer.value);
        const auto& consumer = queues.at(dependency.consumer.value);
        if (producer.queueClass == consumer.queueClass && producer.queueIndex == consumer.queueIndex) continue;
        plan.synchronization.push_back({dependency.producer, dependency.consumer, positions.at(dependency.producer.value)});
    }
    std::sort(plan.synchronization.begin(), plan.synchronization.end(), [](const auto& left, const auto& right) {
        return std::tuple{left.consumer.value, left.producer.value} < std::tuple{right.consumer.value, right.producer.value};
    });
    plan.identity = level3::ComputePlanIdentity(plan);
}

std::vector<semantic::WorkId> TopologicalOrder(const level3::SemanticObligation& obligation,
                                               level3::ScheduleStrategy strategy)
{
    std::map<std::uint32_t, std::uint32_t> indegree;
    std::map<std::uint32_t, std::vector<std::uint32_t>> outgoing;
    std::map<std::uint32_t, semantic::WorkKind> kinds;
    for (const auto& work : obligation.works) { indegree[work.id.value] = 0; kinds[work.id.value] = work.kind; }
    for (const auto& edge : obligation.dependencies)
    {
        ++indegree[edge.consumer.value];
        outgoing[edge.producer.value].push_back(edge.consumer.value);
    }
    std::map<std::uint32_t, std::uint32_t> critical;
    for (auto iterator = obligation.works.rbegin(); iterator != obligation.works.rend(); ++iterator)
    {
        std::uint32_t length = 1;
        for (const auto next : outgoing[iterator->id.value]) length = std::max(length, 1u + critical[next]);
        critical[iterator->id.value] = length;
    }
    std::vector<semantic::WorkId> output;
    while (output.size() != obligation.works.size())
    {
        std::vector<std::uint32_t> ready;
        for (const auto& [id, degree] : indegree) if (degree == 0) ready.push_back(id);
        if (ready.empty()) break;
        std::sort(ready.begin(), ready.end(), [&](std::uint32_t left, std::uint32_t right) {
            if (strategy == level3::ScheduleStrategy::MaximumId) return left > right;
            if (strategy == level3::ScheduleStrategy::CriticalPath && critical[left] != critical[right]) return critical[left] > critical[right];
            if (strategy == level3::ScheduleStrategy::QueueAffinity && kinds[left] != kinds[right]) return kinds[left] < kinds[right];
            return left < right;
        });
        const auto selected = ready.front();
        output.push_back({selected});
        indegree[selected] = base::InvalidIndex;
        for (const auto next : outgoing[selected]) --indegree[next];
    }
    return output;
}

base::Digest256 PolicyDigest(const level3::CompilerPolicy& policy)
{
    base::BinaryWriter writer;
    writer.WriteU16(static_cast<std::uint16_t>(policy.kind));
    writer.WriteU32(policy.budget.maxProposedCandidates); writer.WriteU32(policy.budget.maxVerifiedCandidates);
    writer.WriteU32(policy.budget.maxCandidatesPerAxis); writer.WriteU32(policy.budget.compileWorkUnitBudget);
    return base::Sha256(writer.Bytes());
}

std::vector<std::byte> EncodeManifest(const CandidateManifest& manifest)
{
    std::ostringstream text;
    text << "manifest-version=" << manifest.version << '\n';
    text << "obligation=" << base::ToHex(manifest.obligationDigest) << '\n';
    text << "planning-contract=" << base::ToHex(manifest.planningContractDigest) << '\n';
    text << "policy=" << base::ToHex(manifest.policyDigest) << '\n';
    text << "budget=" << manifest.budget.maxProposedCandidates << ',' << manifest.budget.maxVerifiedCandidates << ','
         << manifest.budget.maxCandidatesPerAxis << ',' << manifest.budget.compileWorkUnitBudget << '\n';
    for (const auto& candidate : manifest.candidates)
    {
        text << "candidate=" << base::ToHex(candidate.plan.identity)
             << "|verified=" << (candidate.verification.verified ? 1 : 0)
             << "|diagnostics=" << candidate.verification.violations.size()
             << "|schedule=" << static_cast<std::uint16_t>(candidate.plan.scheduleStrategy)
             << "|queue=" << static_cast<std::uint16_t>(candidate.plan.queueStrategy)
             << "|allocation=" << static_cast<std::uint16_t>(candidate.plan.allocationStrategy)
             << "|peak=" << candidate.cost.peakAllocationBytes
             << "|reserved=" << candidate.cost.totalReservedBytes
             << "|handoffs=" << candidate.cost.queueHandoffCount
             << "|operations=" << candidate.cost.operationCount
             << "|execution=" << candidate.packageExecutionDigestHex << '\n';
    }
    text << "selected=" << base::ToHex(manifest.selectedPlanIdentity) << '\n';
    text << "fallback=" << manifest.fallbackReason << '\n';
    const auto value = text.str();
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                  reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

bool Better(const CandidateRecord& left, const CandidateRecord& right, level3::CompilerPolicyKind policy)
{
    const auto identity = [&] { return left.plan.identity < right.plan.identity; };
    if (policy == level3::CompilerPolicyKind::CanonicalSafe)
    {
        const bool leftSafe = left.plan.scheduleStrategy == level3::ScheduleStrategy::CanonicalMinimumId &&
            left.plan.queueStrategy == level3::QueueStrategy::CanonicalSafe &&
            left.plan.allocationStrategy == level3::AllocationStrategy::CanonicalSafe;
        const bool rightSafe = right.plan.scheduleStrategy == level3::ScheduleStrategy::CanonicalMinimumId &&
            right.plan.queueStrategy == level3::QueueStrategy::CanonicalSafe &&
            right.plan.allocationStrategy == level3::AllocationStrategy::CanonicalSafe;
        return leftSafe != rightSafe ? leftSafe : identity();
    }
    if (policy == level3::CompilerPolicyKind::MinimizePeakMemory)
        return std::tie(left.cost.peakAllocationBytes, left.cost.totalReservedBytes, left.cost.allocationCount, left.plan.identity) <
               std::tie(right.cost.peakAllocationBytes, right.cost.totalReservedBytes, right.cost.allocationCount, right.plan.identity);
    if (policy == level3::CompilerPolicyKind::MinimizeQueueHandoffs)
    {
        const auto leftDirect = std::count_if(left.plan.queueAssignments.begin(), left.plan.queueAssignments.end(),
            [](const auto& item) { return item.queueClass == level3::QueueClass::Direct; });
        const auto rightDirect = std::count_if(right.plan.queueAssignments.begin(), right.plan.queueAssignments.end(),
            [](const auto& item) { return item.queueClass == level3::QueueClass::Direct; });
        if (left.cost.queueHandoffCount != right.cost.queueHandoffCount)
            return left.cost.queueHandoffCount < right.cost.queueHandoffCount;
        if (leftDirect != rightDirect) return leftDirect > rightDirect;
        return std::tie(left.cost.waitCount, left.cost.operationCount, left.plan.identity) <
               std::tie(right.cost.waitCount, right.cost.operationCount, right.plan.identity);
    }
    if (policy == level3::CompilerPolicyKind::PreferDedicatedQueues)
    {
        const auto leftDedicated = std::count_if(left.plan.queueAssignments.begin(), left.plan.queueAssignments.end(), [](const auto& item) { return item.queueClass != level3::QueueClass::Direct; });
        const auto rightDedicated = std::count_if(right.plan.queueAssignments.begin(), right.plan.queueAssignments.end(), [](const auto& item) { return item.queueClass != level3::QueueClass::Direct; });
        return leftDedicated != rightDedicated ? leftDedicated > rightDedicated : identity();
    }
    return std::tie(left.cost.operationCount, left.cost.transitionCount, left.plan.identity) <
           std::tie(right.cost.operationCount, right.cost.transitionCount, right.plan.identity);
}
}

level3::ExecutionPlanIR BuildCanonicalSafePlan(const level3::SemanticObligation& obligation,
                                               const level3::D3D12PlanningContract& contract)
{
    level3::ExecutionPlanIR plan;
    // SemanticObligation already stores Works in the Level 2 canonical order.
    // Re-running a Source-ID priority sort here would lose that canonicalization
    // for graphs whose stable identity is deliberately independent of Source IDs.
    for (const auto& work : obligation.works) plan.workSchedule.push_back(work.id);
    plan.scheduleStrategy = level3::ScheduleStrategy::CanonicalMinimumId;
    plan.queueStrategy = level3::QueueStrategy::CanonicalSafe;
    plan.allocationStrategy = level3::AllocationStrategy::CanonicalSafe;
    RebuildDerived(plan, obligation, contract);
    return plan;
}

std::vector<level3::ExecutionPlanIR> GenerateCandidatePlans(const level3::SemanticObligation& obligation,
                                                            const level3::D3D12PlanningContract& contract,
                                                            const level3::CompilerPolicy& policy)
{
    std::vector<level3::ExecutionPlanIR> output;
    std::set<std::string> identities;
    const std::uint32_t workUnits = static_cast<std::uint32_t>(
        obligation.works.size() + obligation.uses.size() + obligation.dependencies.size());
    std::uint32_t consumedWorkUnits = 0;
    std::uint32_t scheduleAxisCount = 0;
    std::uint32_t queueAxisCount = 0;
    std::uint32_t allocationAxisCount = 0;
    auto append = [&](level3::ExecutionPlanIR plan, bool rebuild = true) {
        if (output.size() >= policy.budget.maxProposedCandidates) return;
        const bool safe = plan.scheduleStrategy == level3::ScheduleStrategy::CanonicalMinimumId &&
            plan.queueStrategy == level3::QueueStrategy::CanonicalSafe &&
            plan.allocationStrategy == level3::AllocationStrategy::CanonicalSafe;
        if (!safe && consumedWorkUnits + workUnits > policy.budget.compileWorkUnitBudget) return;
        if (!safe && plan.scheduleStrategy != level3::ScheduleStrategy::CanonicalMinimumId &&
            scheduleAxisCount >= policy.budget.maxCandidatesPerAxis) return;
        if (!safe && plan.queueStrategy != level3::QueueStrategy::CanonicalSafe &&
            queueAxisCount >= policy.budget.maxCandidatesPerAxis) return;
        if (!safe && plan.allocationStrategy != level3::AllocationStrategy::CanonicalSafe &&
            allocationAxisCount >= policy.budget.maxCandidatesPerAxis) return;
        if (rebuild) RebuildDerived(plan, obligation, contract);
        if (identities.insert(base::ToHex(plan.identity)).second)
        {
            if (!safe)
            {
                consumedWorkUnits += workUnits;
                if (plan.scheduleStrategy != level3::ScheduleStrategy::CanonicalMinimumId) ++scheduleAxisCount;
                if (plan.queueStrategy != level3::QueueStrategy::CanonicalSafe) ++queueAxisCount;
                if (plan.allocationStrategy != level3::AllocationStrategy::CanonicalSafe) ++allocationAxisCount;
            }
            output.push_back(std::move(plan));
        }
    };

    auto safe = BuildCanonicalSafePlan(obligation, contract);
    append(safe);
    for (const auto strategy : {level3::ScheduleStrategy::MaximumId, level3::ScheduleStrategy::CriticalPath,
                                level3::ScheduleStrategy::QueueAffinity})
    {
        auto plan = safe; plan.scheduleStrategy = strategy; plan.workSchedule = TopologicalOrder(obligation, strategy); append(std::move(plan));
    }
    for (const auto strategy : {level3::QueueStrategy::AllDirect, level3::QueueStrategy::KindPreferredDedicated,
                                level3::QueueStrategy::MinimizeCrossQueueEdges})
    {
        auto plan = safe; plan.queueStrategy = strategy; append(std::move(plan));
    }
    std::uint32_t boundedAlternatives = 0;
    for (const auto& work : obligation.works)
    {
        if (boundedAlternatives >= policy.budget.maxCandidatesPerAxis) break;
        auto plan = safe;
        auto assignment = std::find_if(plan.queueAssignments.begin(), plan.queueAssignments.end(),
            [&](const auto& item) { return item.work == work.id; });
        if (assignment == plan.queueAssignments.end()) continue;
        if (assignment->queueClass == level3::QueueClass::Compute || assignment->queueClass == level3::QueueClass::Copy)
            assignment->queueClass = level3::QueueClass::Direct;
        else if (work.kind == semantic::WorkKind::Compute && contract.computeQueueCount != 0)
            assignment->queueClass = level3::QueueClass::Compute;
        else if (work.kind == semantic::WorkKind::Copy && contract.copyQueueCount != 0)
            assignment->queueClass = level3::QueueClass::Copy;
        else continue;
        plan.queueStrategy = level3::QueueStrategy::BoundedAlternative;
        RebuildSynchronizationAndIdentity(plan, obligation);
        const auto before = output.size();
        append(std::move(plan), false);
        if (output.size() != before) ++boundedAlternatives;
    }
    for (const auto strategy : {level3::AllocationStrategy::ConservativeCommitted,
                                level3::AllocationStrategy::PlacedNoAlias,
                                level3::AllocationStrategy::CanonicalAlias,
                                level3::AllocationStrategy::MemoryAggressiveAlias})
    {
        auto plan = safe; plan.allocationStrategy = strategy; append(std::move(plan));
    }
    for (const auto schedule : {level3::ScheduleStrategy::MaximumId, level3::ScheduleStrategy::CriticalPath})
        for (const auto queue : {level3::QueueStrategy::AllDirect, level3::QueueStrategy::KindPreferredDedicated})
            for (const auto allocation : {level3::AllocationStrategy::ConservativeCommitted, level3::AllocationStrategy::CanonicalAlias})
            {
                auto plan = safe; plan.scheduleStrategy = schedule; plan.workSchedule = TopologicalOrder(obligation, schedule);
                plan.queueStrategy = queue; plan.allocationStrategy = allocation; append(std::move(plan));
            }
    return output;
}

level3::CostVector CalculateCost(const level3::SemanticObligation& obligation,
                                 const level3::ExecutionPlanIR& plan)
{
    level3::CostVector cost;
    for (const auto& allocation : plan.allocations)
    {
        const auto bytes = AlignUp(allocation.sizeBytes, std::max<std::uint64_t>(1, allocation.alignment)) *
            allocation.physicalInstanceCount;
        cost.totalReservedBytes += bytes;
        cost.peakAllocationBytes += bytes;
        cost.physicalInstanceCount += allocation.physicalInstanceCount;
        if (allocation.aliasGroup != base::InvalidIndex) { ++cost.aliasGroupCount; cost.aliasActivationCount += static_cast<std::uint32_t>(allocation.resources.size()); }
    }
    cost.allocationCount = static_cast<std::uint32_t>(plan.allocations.size());
    cost.queueHandoffCount = static_cast<std::uint32_t>(plan.synchronization.size());
    cost.waitCount = cost.queueHandoffCount; cost.signalCount = static_cast<std::uint32_t>(plan.workSchedule.size());
    cost.transitionCount = static_cast<std::uint32_t>(plan.useStates.size()); cost.barrierCount = cost.transitionCount;
    cost.descriptorCount = static_cast<std::uint32_t>(std::count_if(plan.bindings.begin(), plan.bindings.end(), [](const auto& item) { return item.descriptorIndex != base::InvalidIndex; }));
    cost.operationCount = static_cast<std::uint32_t>(plan.workSchedule.size() * 4 + plan.useStates.size() * 2 + plan.synchronization.size());
    cost.estimatedParallelSlack = static_cast<std::uint32_t>(std::count_if(plan.queueAssignments.begin(), plan.queueAssignments.end(), [](const auto& item) { return item.queueClass != level3::QueueClass::Direct; }));
    cost.compileWorkUnits = static_cast<std::uint32_t>(obligation.works.size() + obligation.uses.size() + obligation.dependencies.size());
    return cost;
}

std::vector<std::size_t> ParetoFrontier(std::span<const CandidateRecord> candidates)
{
    const auto dominates = [](const level3::CostVector& left, const level3::CostVector& right) {
        const bool noWorse = left.peakAllocationBytes <= right.peakAllocationBytes &&
            left.totalReservedBytes <= right.totalReservedBytes &&
            left.queueHandoffCount <= right.queueHandoffCount &&
            left.operationCount <= right.operationCount &&
            left.descriptorCount <= right.descriptorCount;
        const bool better = left.peakAllocationBytes < right.peakAllocationBytes ||
            left.totalReservedBytes < right.totalReservedBytes ||
            left.queueHandoffCount < right.queueHandoffCount ||
            left.operationCount < right.operationCount ||
            left.descriptorCount < right.descriptorCount;
        return noWorse && better;
    };
    std::vector<std::size_t> frontier;
    for (std::size_t candidate = 0; candidate < candidates.size(); ++candidate)
    {
        if (!candidates[candidate].verification.verified) continue;
        bool dominated = false;
        for (std::size_t other = 0; other < candidates.size() && !dominated; ++other)
            if (other != candidate && candidates[other].verification.verified &&
                dominates(candidates[other].cost, candidates[candidate].cost)) dominated = true;
        if (!dominated) frontier.push_back(candidate);
    }
    std::sort(frontier.begin(), frontier.end(), [&](std::size_t left, std::size_t right) {
        return candidates[left].plan.identity < candidates[right].plan.identity;
    });
    return frontier;
}

base::Result<Level3CompileOutput, CompileError> CompileLevel3(
    const semantic::SemanticGraph& graph, const target::D3D12TargetProfile& targetProfile,
    const level3::CompilerPolicy& policy, const level3::ProfileRecord* profile,
    const level3::ProfileSelectionContext* profileContext)
{
    auto validated = ValidateSourceStage(graph, targetProfile);
    if (!validated) return base::Result<Level3CompileOutput, CompileError>::Failure(validated.Error());
    auto obligationResult = level3::BuildSemanticObligation(graph, validated.Value().analyzed);
    if (!obligationResult) return base::Result<Level3CompileOutput, CompileError>::Failure({"semantic-obligation", obligationResult.Error()});
    auto obligation = std::move(obligationResult).Value();
    auto contract = level3::BuildPlanningContract(targetProfile);
    const auto minimumWorkUnits = static_cast<std::uint32_t>(
        obligation.works.size() + obligation.uses.size() + obligation.dependencies.size());
    if (policy.budget.maxProposedCandidates == 0 || policy.budget.maxVerifiedCandidates == 0 ||
        policy.budget.maxCandidatesPerAxis == 0 || policy.budget.compileWorkUnitBudget < minimumWorkUnits)
        return base::Result<Level3CompileOutput, CompileError>::Failure({"candidate-budget", "candidate budget cannot construct and verify the CanonicalSafePlan"});
    auto plans = GenerateCandidatePlans(obligation, contract, policy);
    if (plans.empty()) return base::Result<Level3CompileOutput, CompileError>::Failure({"candidate-generation", "candidate budget produced no plan"});

    CandidateManifest manifest;
    manifest.obligationDigest = obligation.digest; manifest.planningContractDigest = contract.digest;
    manifest.policyDigest = PolicyDigest(policy); manifest.budget = policy.budget;
    std::vector<CompileOutput> packages;
    std::uint32_t verifiedCount = 0;
    for (auto& plan : plans)
    {
        CandidateRecord record;
        record.plan = std::move(plan);
        record.verification = level3::verification::Verify(obligation, contract, record.plan);
        record.cost = CalculateCost(obligation, record.plan);
        if (record.verification.verified && verifiedCount < policy.budget.maxVerifiedCandidates)
        {
            auto package = CompileSelectedPlan(graph, targetProfile, record.plan);
            if (!package) return base::Result<Level3CompileOutput, CompileError>::Failure(package.Error());
            record.packageExecutionDigestHex = package.Value().executionDigestHex;
            packages.push_back(std::move(package).Value());
            ++verifiedCount;
        }
        else packages.push_back({});
        manifest.candidates.push_back(std::move(record));
    }

    std::vector<std::size_t> selectable;
    for (std::size_t index = 0; index < manifest.candidates.size(); ++index)
        if (manifest.candidates[index].verification.verified && !manifest.candidates[index].packageExecutionDigestHex.empty())
            selectable.push_back(index);
    if (selectable.empty()) return base::Result<Level3CompileOutput, CompileError>::Failure({"plan-verification", "no candidate passed the independent verifier"});

    if (profile != nullptr)
    {
        if (profileContext == nullptr || profile->version != 1 || profile->targetProfileDigest != contract.digest ||
            profile->adapterDriverFingerprint != profileContext->adapterDriverFingerprint ||
            profile->measurementScenarioDigest != profileContext->measurementScenarioDigest ||
            profile->sampleCount < profileContext->minimumSampleCount)
            return base::Result<Level3CompileOutput, CompileError>::Failure({"profile-validation", "Profile provenance does not match the planning contract"});
        const auto found = std::find_if(selectable.begin(), selectable.end(), [&](std::size_t index) {
            return manifest.candidates[index].plan.identity == profile->planIdentity;
        });
        if (found == selectable.end())
            return base::Result<Level3CompileOutput, CompileError>::Failure({"profile-validation", "Profile PlanIdentity is not an accepted candidate"});
        const auto& candidate = manifest.candidates[*found];
        if (base::ToHex(profile->packageExecutionDigest) != candidate.packageExecutionDigestHex)
            return base::Result<Level3CompileOutput, CompileError>::Failure({"profile-validation", "Profile Package execution digest is stale"});
        selectable = {*found};
    }
    const auto selected = *std::min_element(selectable.begin(), selectable.end(), [&](std::size_t left, std::size_t right) {
        return Better(manifest.candidates[left], manifest.candidates[right], policy.kind);
    });
    manifest.selectedPlanIdentity = manifest.candidates[selected].plan.identity;
    if (policy.kind != level3::CompilerPolicyKind::CanonicalSafe && selectable.size() == 1)
        manifest.fallbackReason = "only-one-verified-candidate";
    manifest.canonicalBytes = EncodeManifest(manifest);

    Level3CompileOutput output;
    output.selectedPackage = std::move(packages[selected]); output.obligation = std::move(obligation);
    output.planningContract = contract; output.selectedPlan = manifest.candidates[selected].plan;
    output.manifest = std::move(manifest);
    return base::Result<Level3CompileOutput, CompileError>::Success(std::move(output));
}
}
