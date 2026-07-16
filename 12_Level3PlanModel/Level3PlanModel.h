#pragma once

#include "../00_Base/Result.h"
#include "../00_Base/Sha256.h"
#include "../02_SemanticModel/SemanticModel.h"
#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../05_TargetModel/TargetModel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sge::level3
{
inline constexpr std::uint32_t PlanContractVersion = 1;

enum class QueueClass : std::uint16_t { Direct = 1, Compute = 2, Copy = 3 };
enum class PlanAllocationKind : std::uint16_t { Committed = 1, Placed = 2 };
enum class AbstractState : std::uint16_t
{
    Common = 1, Present = 2, VertexBuffer = 3, ConstantBuffer = 4,
    ShaderRead = 5, UnorderedWrite = 6, RenderTarget = 7,
    DepthWrite = 8, CopySource = 9, CopyDestination = 10
};
enum class ScheduleStrategy : std::uint16_t
{
    CanonicalMinimumId = 1, MaximumId = 2, CriticalPath = 3, QueueAffinity = 4
};
enum class QueueStrategy : std::uint16_t
{
    CanonicalSafe = 1, AllDirect = 2, KindPreferredDedicated = 3,
    MinimizeCrossQueueEdges = 4, BoundedAlternative = 5
};
enum class AllocationStrategy : std::uint16_t
{
    CanonicalSafe = 1, ConservativeCommitted = 2, PlacedNoAlias = 3,
    CanonicalAlias = 4, MemoryAggressiveAlias = 5
};
enum class CompilerPolicyKind : std::uint16_t
{
    CanonicalSafe = 1, MinimizePeakMemory = 2, MinimizeQueueHandoffs = 3,
    PreferDedicatedQueues = 4, MinimizeOperationCount = 5, ProfileGuided = 6
};

struct ObligationResource final
{
    semantic::ResourceId id;
    semantic::ResourceKind kind = semantic::ResourceKind::Buffer;
    semantic::LifetimeIntent lifetime = semantic::LifetimeIntent::Persistent;
    semantic::UpdateIntent update = semantic::UpdateIntent::Immutable;
    semantic::Visibility visibility = semantic::Visibility::Internal;
    std::uint64_t sizeBytes = 0;
    std::uint32_t alignment = 1;
    std::uint32_t firstUse = base::InvalidIndex;
    std::uint32_t lastUse = base::InvalidIndex;
    bool usedByWork = false;
    semantic::ResourceId aliasCompatibleWith;
};

struct ObligationUse final
{
    semantic::ResourceUseId id;
    semantic::ResourceId resource;
    semantic::WorkId owner;
    semantic::Effect effect = semantic::Effect::Read;
    semantic::ViewRole role = semantic::ViewRole::VertexData;
    semantic::TemporalRelation temporalRelation = semantic::TemporalRelation::Current;
};

struct ObligationWork final
{
    semantic::WorkId id;
    semantic::WorkKind kind = semantic::WorkKind::Compute;
    std::vector<semantic::ResourceUseId> uses;
};

struct ObligationParameter final
{
    semantic::ProgramId program;
    semantic::ProgramParameterId id;
    semantic::ProgramParameterKind kind = semantic::ProgramParameterKind::ConstantBuffer;
    semantic::ShaderStage stage = semantic::ShaderStage::Vertex;
    std::uint32_t shaderRegister = 0;
    std::uint64_t requiredBytes = 0;
};

struct SemanticObligation final
{
    std::uint32_t version = PlanContractVersion;
    std::vector<ObligationResource> resources;
    std::vector<ObligationUse> uses;
    std::vector<ObligationWork> works;
    std::vector<ObligationParameter> parameters;
    std::vector<analysis::DependencyEdge> dependencies;
    base::Digest256 digest{};
};

struct D3D12PlanningContract final
{
    std::uint32_t version = PlanContractVersion;
    std::uint32_t framesInFlight = 0;
    std::uint32_t directQueueCount = 0;
    std::uint32_t computeQueueCount = 0;
    std::uint32_t copyQueueCount = 0;
    std::uint32_t maximumResources = base::InvalidIndex;
    std::uint32_t maximumAllocations = base::InvalidIndex;
    std::uint32_t maximumViews = base::InvalidIndex;
    std::uint32_t maximumOperations = base::InvalidIndex;
    bool standalonePlacedAllocation = false;
    base::Digest256 digest{};
};

struct WorkQueueAssignment final
{
    semantic::WorkId work;
    QueueClass queueClass = QueueClass::Direct;
    std::uint32_t queueIndex = 0;
};

struct SignalWaitEdge final
{
    semantic::WorkId producer;
    semantic::WorkId consumer;
    std::uint32_t signalPoint = base::InvalidIndex;
};

struct ResourceInstancePlan final
{
    semantic::ResourceId resource;
    std::uint32_t physicalInstanceCount = 0;
    std::uint32_t firstUse = base::InvalidIndex;
    std::uint32_t lastUse = base::InvalidIndex;
    std::uint32_t allocation = base::InvalidIndex;
};

struct AllocationPlan final
{
    std::uint32_t id = base::InvalidIndex;
    PlanAllocationKind kind = PlanAllocationKind::Committed;
    std::uint64_t sizeBytes = 0;
    std::uint64_t alignment = 0;
    std::uint32_t physicalInstanceCount = 1;
    std::uint32_t aliasGroup = base::InvalidIndex;
    std::vector<semantic::ResourceId> resources;
};

struct UseStatePlan final
{
    semantic::ResourceUseId use;
    AbstractState required = AbstractState::Common;
};

struct BindingPlan final
{
    semantic::ProgramId program;
    semantic::ProgramParameterId parameter;
    std::uint32_t rootParameterIndex = 0;
    std::uint32_t descriptorIndex = base::InvalidIndex;
};

struct ExecutionPlanIR final
{
    std::uint32_t version = PlanContractVersion;
    ScheduleStrategy scheduleStrategy = ScheduleStrategy::CanonicalMinimumId;
    QueueStrategy queueStrategy = QueueStrategy::CanonicalSafe;
    AllocationStrategy allocationStrategy = AllocationStrategy::CanonicalSafe;
    std::vector<semantic::WorkId> workSchedule;
    std::vector<WorkQueueAssignment> queueAssignments;
    std::vector<SignalWaitEdge> synchronization;
    std::vector<ResourceInstancePlan> resourceInstances;
    std::vector<AllocationPlan> allocations;
    std::vector<UseStatePlan> useStates;
    std::vector<BindingPlan> bindings;
    std::vector<semantic::ResourceId> externalBoundaries;
    std::vector<semantic::ResourceId> presentBoundaries;
    base::Digest256 identity{};
};

struct CandidateBudget final
{
    std::uint32_t maxProposedCandidates = 64;
    std::uint32_t maxVerifiedCandidates = 32;
    std::uint32_t maxCandidatesPerAxis = 8;
    std::uint32_t compileWorkUnitBudget = 4096;
};

struct CompilerPolicy final
{
    CompilerPolicyKind kind = CompilerPolicyKind::CanonicalSafe;
    CandidateBudget budget;
};

struct CostVector final
{
    std::uint64_t peakAllocationBytes = 0;
    std::uint64_t totalReservedBytes = 0;
    std::uint32_t allocationCount = 0;
    std::uint32_t aliasGroupCount = 0;
    std::uint32_t aliasActivationCount = 0;
    std::uint32_t barrierCount = 0;
    std::uint32_t transitionCount = 0;
    std::uint32_t queueHandoffCount = 0;
    std::uint32_t waitCount = 0;
    std::uint32_t signalCount = 0;
    std::uint32_t operationCount = 0;
    std::uint32_t descriptorCount = 0;
    std::uint32_t physicalInstanceCount = 0;
    std::uint32_t estimatedParallelSlack = 0;
    std::uint32_t compileWorkUnits = 0;
};

struct ProfileRecord final
{
    std::uint32_t version = 1;
    base::Digest256 planIdentity{};
    base::Digest256 packageExecutionDigest{};
    base::Digest256 targetProfileDigest{};
    base::Digest256 adapterDriverFingerprint{};
    base::Digest256 measurementScenarioDigest{};
    std::uint32_t sampleCount = 0;
    std::uint64_t measuredNanoseconds = 0;
    std::uint64_t observedPeakBytes = 0;
};

struct ProfileSelectionContext final
{
    base::Digest256 adapterDriverFingerprint{};
    base::Digest256 measurementScenarioDigest{};
    std::uint32_t minimumSampleCount = 1;
};

[[nodiscard]] base::Result<SemanticObligation, std::string> BuildSemanticObligation(
    const semantic::SemanticGraph& graph, const analysis::AnalyzedGraph& analyzed);
[[nodiscard]] D3D12PlanningContract BuildPlanningContract(const target::D3D12TargetProfile& profile);
[[nodiscard]] std::vector<std::byte> EncodeCanonical(const SemanticObligation& value);
[[nodiscard]] std::vector<std::byte> EncodeCanonical(const D3D12PlanningContract& value);
[[nodiscard]] std::vector<std::byte> EncodeCanonical(const ExecutionPlanIR& value, bool includeIdentity = false);
[[nodiscard]] std::vector<std::byte> EncodeCanonical(const ProfileRecord& value);
[[nodiscard]] base::Digest256 ComputePlanIdentity(const ExecutionPlanIR& value);
[[nodiscard]] AbstractState RequiredState(semantic::ViewRole role, semantic::WorkKind workKind);
}
