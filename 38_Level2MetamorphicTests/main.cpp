#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../25_Level2Scenarios/Level2Scenarios.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace base = sge::base;
namespace sem = sge::semantic;
namespace compiler = sge::compiler::d3d12;
namespace lvl2 = sge::level2;

struct Compiled final
{
    std::vector<std::byte> bytes;
    std::string digest;
};

bool ReplaceOnce(std::string& text, std::string_view before, std::string_view after)
{
    const auto position = text.find(before);
    if (position == std::string::npos) return false;
    text.replace(position, before.size(), after);
    return true;
}

base::Result<Compiled, std::string> CompileOnce(
    const sem::SemanticGraph& graph,
    const sge::target::D3D12TargetProfile& profile)
{
    auto result = compiler::Compile(graph, profile);
    if (!result)
        return base::Result<Compiled, std::string>::Failure(
            result.Error().stage + ": " + result.Error().message);
    auto value = std::move(result).Value();
    return base::Result<Compiled, std::string>::Success(
        Compiled{std::move(value.packageBytes), std::move(value.executionDigestHex)});
}

bool Same(const Compiled& left, const Compiled& right)
{
    return left.bytes == right.bytes && left.digest == right.digest;
}

bool Different(const Compiled& left, const Compiled& right)
{
    return left.bytes != right.bytes && left.digest != right.digest;
}

sem::Work* FindWork(sem::SemanticGraph& graph, sem::WorkId id)
{
    const auto found = std::find_if(graph.works.begin(), graph.works.end(),
        [id](const sem::Work& work) { return work.id == id; });
    return found == graph.works.end() ? nullptr : &*found;
}

sem::SemanticGraph PermuteStorage(sem::SemanticGraph graph)
{
    std::reverse(graph.resources.begin(), graph.resources.end());
    if (!graph.resourceUses.empty())
        std::rotate(graph.resourceUses.begin(), graph.resourceUses.begin() + 1, graph.resourceUses.end());
    std::reverse(graph.programs.begin(), graph.programs.end());
    if (!graph.works.empty())
        std::rotate(graph.works.begin(), graph.works.begin() + 1, graph.works.end());
    return graph;
}

sem::SemanticGraph PermuteOperandsAndInterfaces(sem::SemanticGraph graph)
{
    for (auto& work : graph.works)
    {
        std::reverse(work.operands.begin(), work.operands.end());
        std::reverse(work.dependencies.begin(), work.dependencies.end());
    }
    for (auto& program : graph.programs)
    {
        std::reverse(program.interface.parameters.begin(), program.interface.parameters.end());
        std::reverse(program.interface.vertexInputs.begin(), program.interface.vertexInputs.end());
    }
    return graph;
}

sem::SemanticGraph RenameDiagnostics(sem::SemanticGraph graph)
{
    std::uint32_t serial = 0;
    for (auto& resource : graph.resources) resource.debugName = "Renamed.Resource." + std::to_string(serial++);
    for (auto& program : graph.programs)
    {
        program.debugName = "Renamed.Program." + std::to_string(serial++);
        for (auto& parameter : program.interface.parameters)
            parameter.debugName = "Renamed.Parameter." + std::to_string(serial++);
    }
    for (auto& work : graph.works) work.debugName = "Renamed.Work." + std::to_string(serial++);
    return graph;
}

bool ExpectEquivalent(
    std::string_view law,
    const Compiled& baseline,
    const sem::SemanticGraph& transformed,
    const sge::target::D3D12TargetProfile& profile)
{
    auto compiled = CompileOnce(transformed, profile);
    if (!compiled)
    {
        std::cerr << law << " rejected an equivalent graph: " << compiled.Error() << '\n';
        return false;
    }
    if (!Same(baseline, compiled.Value()))
    {
        std::cerr << law << " changed Package bytes or execution digest\n";
        return false;
    }
    std::cout << "  equivalent: " << law << '\n';
    return true;
}

bool ExpectSensitive(
    std::string_view law,
    const Compiled& baseline,
    const sem::SemanticGraph& transformed,
    const sge::target::D3D12TargetProfile& profile)
{
    auto compiled = CompileOnce(transformed, profile);
    if (!compiled)
    {
        std::cerr << law << " unexpectedly rejected a valid semantic change: " << compiled.Error() << '\n';
        return false;
    }
    if (!Different(baseline, compiled.Value()))
    {
        std::cerr << law << " did not change both Package bytes and execution digest\n";
        return false;
    }
    std::cout << "  sensitive:  " << law << '\n';
    return true;
}

bool ExpectRejected(
    std::string_view law,
    const sem::SemanticGraph& graph,
    const sge::target::D3D12TargetProfile& profile,
    std::string_view expectedStage)
{
    auto compiled = compiler::Compile(graph, profile);
    if (compiled)
    {
        std::cerr << law << " was accepted\n";
        return false;
    }
    if (compiled.Error().stage != expectedStage)
    {
        std::cerr << law << " failed at " << compiled.Error().stage
                  << " instead of " << expectedStage << '\n';
        return false;
    }
    std::cout << "  rejected:   " << law << " at " << expectedStage << '\n';
    return true;
}

base::Result<lvl2::StageHInput, std::string> Build(lvl2::StageHScenario scenario)
{
    return lvl2::BuildStageHScenario(scenario);
}

int RunEquivalentLaws()
{
    auto zigzagResult = Build(lvl2::StageHScenario::ComputeCopyZigZag);
    auto diamondResult = Build(lvl2::StageHScenario::ComputeDiamond);
    auto rasterResult = Build(lvl2::StageHScenario::TwoPassRaster);
    if (!zigzagResult || !diamondResult || !rasterResult)
    {
        std::cerr << "Stage-I source scenario construction failed\n";
        return 1;
    }
    auto zigzag = std::move(zigzagResult).Value();
    auto diamond = std::move(diamondResult).Value();
    auto raster = std::move(rasterResult).Value();

    auto zigzagBaseline = CompileOnce(zigzag.graph, zigzag.targetProfile);
    auto diamondBaseline = CompileOnce(diamond.graph, diamond.targetProfile);
    auto rasterBaseline = CompileOnce(raster.graph, raster.targetProfile);
    if (!zigzagBaseline || !diamondBaseline || !rasterBaseline)
    {
        std::cerr << "Stage-I baseline Compile failed\n";
        return 2;
    }

    if (!ExpectEquivalent("top-level storage permutation", zigzagBaseline.Value(),
                          PermuteStorage(zigzag.graph), zigzag.targetProfile)) return 3;
    if (!ExpectEquivalent("WorkOperand / ProgramParameter vector permutation", zigzagBaseline.Value(),
                          PermuteOperandsAndInterfaces(zigzag.graph), zigzag.targetProfile)) return 4;
    if (!ExpectEquivalent("diagnostic-name mutation", zigzagBaseline.Value(),
                          RenameDiagnostics(zigzag.graph), zigzag.targetProfile)) return 5;

    auto redundant = zigzag.graph;
    const auto& order = zigzag.expectations.canonicalWorkOrder;
    auto* finalWork = FindWork(redundant, order.back());
    if (finalWork == nullptr) return 6;
    finalWork->dependencies.push_back(order.front());
    if (!ExpectEquivalent("transitively redundant explicit dependency", zigzagBaseline.Value(),
                          redundant, zigzag.targetProfile)) return 7;

    auto explicitDiamond = diamond.graph;
    auto analyzed = sge::analysis::Analyze(explicitDiamond);
    if (!analyzed) return 8;
    const auto mergeId = analyzed.Value().canonicalWorkOrder.back();
    std::vector<sem::WorkId> producers;
    for (const auto& edge : analyzed.Value().dependencies)
        if (edge.consumer == mergeId) producers.push_back(edge.producer);
    std::sort(producers.begin(), producers.end(), [](sem::WorkId left, sem::WorkId right) {
        return left.value < right.value;
    });
    producers.erase(std::unique(producers.begin(), producers.end()), producers.end());
    if (producers.size() < 2) return 9;
    auto* merge = FindWork(explicitDiamond, mergeId);
    if (merge == nullptr) return 10;
    merge->dependencies = producers;
    if (!ExpectEquivalent("derived hazards restated as explicit dependencies", diamondBaseline.Value(),
                          explicitDiamond, diamond.targetProfile)) return 11;
    std::reverse(merge->dependencies.begin(), merge->dependencies.end());
    if (!ExpectEquivalent("explicit dependency vector permutation", diamondBaseline.Value(),
                          explicitDiamond, diamond.targetProfile)) return 12;

    if (!ExpectEquivalent("Raster vertex-input / operand permutation", rasterBaseline.Value(),
                          PermuteOperandsAndInterfaces(raster.graph), raster.targetProfile)) return 13;

    return 0;
}

int RunSensitivityLaws()
{
    auto zigzagResult = Build(lvl2::StageHScenario::ComputeCopyZigZag);
    auto rasterResult = Build(lvl2::StageHScenario::TwoPassRaster);
    if (!zigzagResult || !rasterResult) return 1;
    auto zigzag = std::move(zigzagResult).Value();
    auto raster = std::move(rasterResult).Value();
    auto zigzagBaseline = CompileOnce(zigzag.graph, zigzag.targetProfile);
    auto rasterBaseline = CompileOnce(raster.graph, raster.targetProfile);
    if (!zigzagBaseline || !rasterBaseline) return 2;

    auto dispatch = zigzag.graph;
    const auto firstWorkId = zigzag.expectations.canonicalWorkOrder.front();
    auto* firstWork = FindWork(dispatch, firstWorkId);
    if (firstWork == nullptr || firstWork->kind != sem::WorkKind::Compute) return 3;
    ++firstWork->compute.threadGroupCountX;
    if (!ExpectSensitive("Compute dispatch dimensions", zigzagBaseline.Value(), dispatch,
                         zigzag.targetProfile)) return 4;

    auto copyBytes = zigzag.graph;
    auto copy = std::find_if(copyBytes.works.begin(), copyBytes.works.end(), [](const sem::Work& work) {
        return work.kind == sem::WorkKind::Copy;
    });
    if (copy == copyBytes.works.end()) return 5;
    copy->copy.bytes -= 16;
    if (!ExpectSensitive("Copy byte count", zigzagBaseline.Value(), copyBytes,
                         zigzag.targetProfile)) return 6;

    auto shader = zigzag.graph;
    auto shaderProgram = std::find_if(shader.programs.begin(), shader.programs.end(), [](const sem::Program& program) {
        return program.source.hlslSource.find("* 2.0f") != std::string::npos;
    });
    if (shaderProgram == shader.programs.end() ||
        !ReplaceOnce(shaderProgram->source.hlslSource, "* 2.0f", "* 3.0f")) return 7;
    if (!ExpectSensitive("Shader binary semantics", zigzagBaseline.Value(), shader,
                         zigzag.targetProfile)) return 8;

    auto shaderRegister = zigzag.graph;
    auto registerProgram = std::find_if(shaderRegister.programs.begin(), shaderRegister.programs.end(), [](const sem::Program& program) {
        return program.source.hlslSource.find("CSSeed") != std::string::npos;
    });
    if (registerProgram == shaderRegister.programs.end()) return 9;
    auto parameter = std::find_if(registerProgram->interface.parameters.begin(),
        registerProgram->interface.parameters.end(), [](const sem::ProgramParameter& value) {
            return value.kind == sem::ProgramParameterKind::ReadOnlyBuffer;
        });
    if (parameter == registerProgram->interface.parameters.end() ||
        !ReplaceOnce(registerProgram->source.hlslSource, "register(t0)", "register(t1)")) return 10;
    parameter->shaderRegister = 1;
    if (!ExpectSensitive("ProgramParameter shader register", zigzagBaseline.Value(), shaderRegister,
                         zigzag.targetProfile)) return 11;

    auto resourceSize = zigzag.graph;
    auto resultResource = std::find_if(resourceSize.resources.begin(), resourceSize.resources.end(), [](const sem::Resource& resource) {
        return resource.debugName.find("Result") != std::string::npos;
    });
    if (resultResource == resourceSize.resources.end()) return 12;
    resultResource->buffer.sizeBytes += resultResource->buffer.strideBytes;
    if (!ExpectSensitive("Resource shape", zigzagBaseline.Value(), resourceSize,
                         zigzag.targetProfile)) return 13;

    auto fallbackProfile = zigzag.targetProfile;
    fallbackProfile.computeQueueCount = 0;
    if (!ExpectSensitive("Queue profile / fallback assignment", zigzagBaseline.Value(), zigzag.graph,
                         fallbackProfile)) return 14;

    auto vertexCount = raster.graph;
    auto rasterWork = std::find_if(vertexCount.works.begin(), vertexCount.works.end(), [](const sem::Work& work) {
        return work.kind == sem::WorkKind::Raster;
    });
    if (rasterWork == vertexCount.works.end() || rasterWork->raster.vertexCount < 2) return 15;
    --rasterWork->raster.vertexCount;
    if (!ExpectSensitive("Raster vertex count", rasterBaseline.Value(), vertexCount,
                         raster.targetProfile)) return 16;

    return 0;
}

int RunRejectionLaws()
{
    auto zigzagResult = Build(lvl2::StageHScenario::ComputeCopyZigZag);
    if (!zigzagResult) return 1;
    auto input = std::move(zigzagResult).Value();
    const auto& order = input.expectations.canonicalWorkOrder;

    auto cycle = input.graph;
    auto* first = FindWork(cycle, order.front());
    if (first == nullptr) return 2;
    first->dependencies.push_back(order.back());
    if (!ExpectRejected("dependency cycle", cycle, input.targetProfile, "semantic-analysis")) return 3;

    auto duplicateBinding = input.graph;
    auto compute = std::find_if(duplicateBinding.works.begin(), duplicateBinding.works.end(), [](const sem::Work& work) {
        return work.kind == sem::WorkKind::Compute && work.operands.size() >= 2;
    });
    if (compute == duplicateBinding.works.end()) return 4;
    compute->operands[1].parameter = compute->operands[0].parameter;
    if (!ExpectRejected("duplicate ProgramParameter binding", duplicateBinding,
                        input.targetProfile, "semantic-analysis")) return 5;

    auto danglingParameter = input.graph;
    auto danglingWork = std::find_if(danglingParameter.works.begin(), danglingParameter.works.end(), [](const sem::Work& work) {
        return work.kind == sem::WorkKind::Compute && !work.operands.empty();
    });
    if (danglingWork == danglingParameter.works.end()) return 6;
    danglingWork->operands.front().parameter = sem::ProgramParameterId{999};
    if (!ExpectRejected("unknown ProgramParameter identity", danglingParameter,
                        input.targetProfile, "semantic-analysis")) return 7;

    auto duplicateParameterId = input.graph;
    auto program = std::find_if(duplicateParameterId.programs.begin(), duplicateParameterId.programs.end(), [](const sem::Program& value) {
        return value.interface.parameters.size() >= 2;
    });
    if (program == duplicateParameterId.programs.end()) return 8;
    program->interface.parameters[1].id = program->interface.parameters[0].id;
    if (!ExpectRejected("duplicate ProgramParameter declaration identity", duplicateParameterId,
                        input.targetProfile, "semantic-analysis")) return 9;

    auto copyOverflow = input.graph;
    auto copy = std::find_if(copyOverflow.works.begin(), copyOverflow.works.end(), [](const sem::Work& work) {
        return work.kind == sem::WorkKind::Copy;
    });
    if (copy == copyOverflow.works.end()) return 10;
    copy->copy.bytes = 1024;
    if (!ExpectRejected("Copy range overflow", copyOverflow,
                        input.targetProfile, "semantic-analysis")) return 11;

    auto unsupportedProfile = input.targetProfile;
    unsupportedProfile.directQueueCount = 2;
    if (!ExpectRejected("unsupported queue topology", input.graph,
                        unsupportedProfile, "target-feasibility")) return 12;

    return 0;
}
}

int main()
{
    std::cout << "Stage-I equivalent-representation laws...\n";
    if (const auto result = RunEquivalentLaws(); result != 0) return 10 + result;

    std::cout << "Stage-I semantic-sensitivity laws...\n";
    if (const auto result = RunSensitivityLaws(); result != 0) return 40 + result;

    std::cout << "Stage-I rejection-boundary laws...\n";
    if (const auto result = RunRejectionLaws(); result != 0) return 70 + result;

    std::cout << "Stage-I canonicalization and metamorphic laws passed.\n";
    return 0;
}
