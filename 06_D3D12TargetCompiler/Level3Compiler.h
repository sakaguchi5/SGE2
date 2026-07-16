#pragma once

#include "D3D12TargetCompiler.h"
#include "../13_Level3PlanVerifier/Level3PlanVerifier.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sge::compiler::d3d12::level3_compiler
{
struct CandidateRecord final
{
    level3::ExecutionPlanIR plan;
    level3::verification::VerificationReport verification;
    level3::CostVector cost;
    std::string packageExecutionDigestHex;
};

struct CandidateManifest final
{
    std::uint32_t version = 1;
    base::Digest256 obligationDigest{};
    base::Digest256 planningContractDigest{};
    base::Digest256 policyDigest{};
    level3::CandidateBudget budget;
    std::vector<CandidateRecord> candidates;
    base::Digest256 selectedPlanIdentity{};
    std::string fallbackReason;
    std::vector<std::byte> canonicalBytes;
};

struct Level3CompileOutput final
{
    CompileOutput selectedPackage;
    level3::SemanticObligation obligation;
    level3::D3D12PlanningContract planningContract;
    level3::ExecutionPlanIR selectedPlan;
    CandidateManifest manifest;
};

[[nodiscard]] level3::ExecutionPlanIR BuildCanonicalSafePlan(
    const level3::SemanticObligation& obligation,
    const level3::D3D12PlanningContract& contract);
[[nodiscard]] std::vector<level3::ExecutionPlanIR> GenerateCandidatePlans(
    const level3::SemanticObligation& obligation,
    const level3::D3D12PlanningContract& contract,
    const level3::CompilerPolicy& policy);
[[nodiscard]] level3::CostVector CalculateCost(
    const level3::SemanticObligation& obligation,
    const level3::ExecutionPlanIR& plan);
[[nodiscard]] std::vector<std::size_t> ParetoFrontier(
    std::span<const CandidateRecord> candidates);
[[nodiscard]] base::Result<Level3CompileOutput, CompileError> CompileLevel3(
    const semantic::SemanticGraph& graph,
    const target::D3D12TargetProfile& targetProfile,
    const level3::CompilerPolicy& policy,
    const level3::ProfileRecord* profile = nullptr,
    const level3::ProfileSelectionContext* profileContext = nullptr);
}
