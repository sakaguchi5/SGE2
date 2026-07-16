#pragma once

#include "../12_Level3PlanModel/Level3PlanModel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sge::level3::verification
{
enum class DiagnosticCode : std::uint32_t
{
    None = 0,
    PlanIdentityMismatch,
    MissingWork,
    DuplicateWork,
    UnknownWork,
    DependencyOrderViolation,
    MissingQueueAssignment,
    DuplicateQueueAssignment,
    QueueCapabilityViolation,
    MissingSynchronization,
    InvalidSynchronization,
    DuplicateResourcePlan,
    ResourceInstanceViolation,
    LifetimeViolation,
    AllocationCoverageViolation,
    AllocationAliasViolation,
    StatePlanViolation,
    BindingPlanViolation,
    BoundaryViolation,
    ArtifactCardinalityViolation
};

struct Violation final
{
    DiagnosticCode code = DiagnosticCode::None;
    std::string stage;
    std::string message;
    std::uint32_t work = base::InvalidIndex;
    std::uint32_t resource = base::InvalidIndex;
    std::uint32_t use = base::InvalidIndex;
};

struct VerificationReport final
{
    bool verified = false;
    std::vector<Violation> violations;
};

[[nodiscard]] VerificationReport Verify(
    const SemanticObligation& obligation,
    const D3D12PlanningContract& contract,
    const ExecutionPlanIR& plan);
}
