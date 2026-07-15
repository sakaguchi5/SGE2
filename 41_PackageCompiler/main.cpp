#include "../00_Base/FileIO.h"
#include "../05_TargetModel/TargetModel.h"
#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../20_ClassicalFrontend/ClassicalTriangle.h"
#include "../21_SdfFrontend/SdfFrontend.h"
#include "../22_PgaFrontend/PgaFrontend.h"
#include "../23_ExperimentDomain/ExperimentDomain.h"

#include <filesystem>
#include <iostream>
#include <string_view>
#include <utility>

namespace
{
enum class FrontendSelection
{
    All,
    Classical,
    Sdf,
    Pga
};

FrontendSelection ParseSelection(int argc, char** argv)
{
    if (argc < 3) return FrontendSelection::All;
    const std::string_view value = argv[2];
    if (value == "classical" || value == "--frontend=classical") return FrontendSelection::Classical;
    if (value == "sdf" || value == "--frontend=sdf") return FrontendSelection::Sdf;
    if (value == "pga" || value == "--frontend=pga") return FrontendSelection::Pga;
    return FrontendSelection::All; // "all" and the Slice-14 "both" spelling are accepted aliases.
}

struct DeterministicCompile final
{
    sge::compiler::d3d12::CompileOutput output;
};

sge::base::Result<DeterministicCompile, std::string> CompileTwice(
    const sge::semantic::SemanticGraph& graph,
    const sge::target::D3D12TargetProfile& profile,
    std::string_view frontendName)
{
    auto first = sge::compiler::d3d12::Compile(graph, profile);
    auto second = sge::compiler::d3d12::Compile(graph, profile);
    if (!first || !second)
    {
        const auto& error = !first ? first.Error() : second.Error();
        return sge::base::Result<DeterministicCompile, std::string>::Failure(
            std::string(frontendName) + " compile failed at " + error.stage + ": " + error.message);
    }
    if (first.Value().packageBytes != second.Value().packageBytes ||
        first.Value().executionDigestHex != second.Value().executionDigestHex)
        return sge::base::Result<DeterministicCompile, std::string>::Failure(
            std::string(frontendName) + " determinism failed");

    return sge::base::Result<DeterministicCompile, std::string>::Success(
        {std::move(first.Value())});
}

bool SamePackage(
    const sge::compiler::d3d12::CompileOutput& left,
    const sge::compiler::d3d12::CompileOutput& right)
{
    return left.packageBytes == right.packageBytes &&
           left.executionDigestHex == right.executionDigestHex;
}
}

int main(int argc, char** argv)
{
    const std::filesystem::path outputPath = argc >= 2
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("CommonExperiment.sgep");
    const FrontendSelection selection = ParseSelection(argc, argv);

    sge::target::D3D12TargetProfile profile;
    sge::compiler::d3d12::CompileOutput selectedOutput;
    const char* selectedLabel = "Classical + SDF + PGA equivalence";

    if (selection == FrontendSelection::Classical)
    {
        auto graph = sge::classical::BuildTriangleGraph();
        if (!graph) { std::cerr << "Classical frontend failed: " << graph.Error() << '\n'; return 1; }
        auto compiled = CompileTwice(graph.Value(), profile, "Classical");
        if (!compiled) { std::cerr << compiled.Error() << '\n'; return 2; }
        selectedOutput = std::move(compiled.Value().output);
        selectedLabel = "Classical explicit mesh";
    }
    else if (selection == FrontendSelection::Sdf)
    {
        auto graph = sge::sdf::BuildTriangleGraph();
        if (!graph) { std::cerr << "SDF frontend failed: " << graph.Error() << '\n'; return 1; }
        auto compiled = CompileTwice(graph.Value(), profile, "SDF");
        if (!compiled) { std::cerr << compiled.Error() << '\n'; return 2; }
        selectedOutput = std::move(compiled.Value().output);
        selectedLabel = "SDF half-space field";
    }
    else if (selection == FrontendSelection::Pga)
    {
        auto graph = sge::pga::BuildTriangleGraph();
        if (!graph) { std::cerr << "PGA frontend failed: " << graph.Error() << '\n'; return 1; }
        auto compiled = CompileTwice(graph.Value(), profile, "PGA");
        if (!compiled) { std::cerr << compiled.Error() << '\n'; return 2; }
        selectedOutput = std::move(compiled.Value().output);
        selectedLabel = "PGA projective line algebra";
    }
    else
    {
        const auto classicalGeometry = sge::classical::BuildTriangleGeometry();
        auto sdfGeometry = sge::sdf::BuildTriangleGeometry();
        auto pgaGeometry = sge::pga::BuildTriangleGeometry();
        if (!sdfGeometry || !pgaGeometry)
        {
            std::cerr << "Mathematical geometry extraction failed: "
                      << (!sdfGeometry ? sdfGeometry.Error() : pgaGeometry.Error()) << '\n';
            return 1;
        }
        if (!sge::experiment::GeometryBitwiseEqual(classicalGeometry, sdfGeometry.Value()) ||
            !sge::experiment::GeometryBitwiseEqual(classicalGeometry, pgaGeometry.Value()))
        {
            std::cerr << "Classical, SDF, and PGA geometry are not bit-identical.\n";
            return 2;
        }

        auto classicalGraph = sge::classical::BuildTriangleGraph();
        auto sdfGraph = sge::sdf::BuildTriangleGraph();
        auto pgaGraph = sge::pga::BuildTriangleGraph();
        if (!classicalGraph || !sdfGraph || !pgaGraph)
        {
            std::cerr << "Frontend failed: "
                      << (!classicalGraph ? classicalGraph.Error() :
                          !sdfGraph ? sdfGraph.Error() : pgaGraph.Error()) << '\n';
            return 3;
        }

        auto classicalCompiled = CompileTwice(classicalGraph.Value(), profile, "Classical");
        auto sdfCompiled = CompileTwice(sdfGraph.Value(), profile, "SDF");
        auto pgaCompiled = CompileTwice(pgaGraph.Value(), profile, "PGA");
        if (!classicalCompiled || !sdfCompiled || !pgaCompiled)
        {
            std::cerr << (!classicalCompiled ? classicalCompiled.Error() :
                          !sdfCompiled ? sdfCompiled.Error() : pgaCompiled.Error()) << '\n';
            return 4;
        }
        if (!SamePackage(classicalCompiled.Value().output, sdfCompiled.Value().output) ||
            !SamePackage(classicalCompiled.Value().output, pgaCompiled.Value().output))
        {
            std::cerr << "Classical, SDF, and PGA frontends did not converge to one Frozen Package.\n";
            return 5;
        }
        selectedOutput = std::move(classicalCompiled.Value().output);
    }

    auto verified = sge::package::PackageReader::Read(selectedOutput.packageBytes);
    if (!verified)
    {
        std::cerr << "Package validation failed: " << verified.Error().message << '\n';
        return 6;
    }
    auto written = sge::base::WriteAllBytes(outputPath, selectedOutput.packageBytes);
    if (!written)
    {
        std::cerr << "Write failed: " << written.Error() << '\n';
        return 7;
    }

    std::cout << "Created: " << outputPath.string() << '\n';
    std::cout << "Frontend: " << selectedLabel << '\n';
    std::cout << "Bytes: " << selectedOutput.packageBytes.size() << '\n';
    std::cout << "Execution digest: " << selectedOutput.executionDigestHex << '\n';
    std::cout << "Determinism: byte-identical\n";
    std::cout << "Slice 15 contract: independent Classical, SDF, and direct PGA frontends converge through the neutral experiment boundary to one Semantic execution and one Frozen Package\n";
    return 0;
}
