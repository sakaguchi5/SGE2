#include "../06_D3D12TargetCompiler/D3D12TargetCompiler.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../07_FrozenPackageCore/PackageWriter.h"
#include "../08_D3D12PackageSchema/D3D12Encoding.h"
#include "../25_Level2Scenarios/Level2Scenarios.h"
#include "../27_Level2RuntimeScenarios/RuntimeScenarios.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace core = sge::package;
namespace pkg = sge::package::d3d12_v13;
namespace runtime_cases = sge::level2::runtime_scenarios;

std::uint16_t ReadU16(std::span<const std::byte> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8u;
}

std::uint32_t ReadU32(std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8u);
    return value;
}

std::uint64_t ReadU64(std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (std::uint32_t index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8u);
    return value;
}

void WriteU16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value)
{
    for (std::uint32_t index = 0; index < 2; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
}

void WriteU32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    for (std::uint32_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
}

void WriteU64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value)
{
    for (std::uint32_t index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
}

core::PackageBuildInput RebuildInput(const core::FrozenExecutablePackage& package)
{
    core::PackageBuildInput input;
    input.targetKind = package.Header().targetKind;
    input.targetSchemaVersion = package.Header().targetSchemaVersion;
    input.minimumRuntimeVersion = package.Header().minimumRuntimeVersion;
    input.flags = package.Header().flags;
    for (const auto& source : package.Sections())
    {
        core::PackageSectionInput section;
        section.kind = source.descriptor.sectionKind;
        section.schemaVersion = source.descriptor.schemaVersion;
        section.flags = source.descriptor.flags;
        section.alignment = source.descriptor.alignment;
        section.elementCount = source.descriptor.elementCount;
        section.elementStride = source.descriptor.elementStride;
        section.bytes.assign(source.bytes.begin(), source.bytes.end());
        input.sections.push_back(std::move(section));
    }
    return input;
}

core::PackageSectionInput& Section(core::PackageBuildInput& input, core::SectionKind kind)
{
    const auto found = std::find_if(input.sections.begin(), input.sections.end(),
        [kind](const auto& section) { return section.kind == kind; });
    if (found == input.sections.end()) std::terminate();
    return *found;
}

const core::PackageSectionInput& Section(const core::PackageBuildInput& input, core::SectionKind kind)
{
    return Section(const_cast<core::PackageBuildInput&>(input), kind);
}

struct OperationRecord final
{
    std::size_t index = 0;
    std::uint64_t payloadOffset = 0;
    std::uint32_t payloadBytes = 0;
};

OperationRecord FindOperation(const core::PackageBuildInput& input,
                              pkg::D3D12OperationCode code,
                              std::size_t occurrence = 0)
{
    const auto& operations = Section(input, core::SectionKind::OperationTable);
    for (std::size_t index = 0; index < operations.elementCount; ++index)
    {
        const auto record = index * operations.elementStride;
        if (ReadU32(operations.bytes, record) != static_cast<std::uint32_t>(code)) continue;
        if (occurrence-- != 0) continue;
        return {index, ReadU64(operations.bytes, record + 16), ReadU32(operations.bytes, record + 12)};
    }
    std::terminate();
}

std::size_t CountOperations(const core::PackageBuildInput& input, pkg::D3D12OperationCode code)
{
    const auto& operations = Section(input, core::SectionKind::OperationTable);
    std::size_t result = 0;
    for (std::size_t index = 0; index < operations.elementCount; ++index)
        if (ReadU32(operations.bytes, index * operations.elementStride) == static_cast<std::uint32_t>(code))
            ++result;
    return result;
}

template<class Input>
sge::base::Result<core::FrozenExecutablePackage, std::string> Compile(const Input& input)
{
    auto compiled = sge::compiler::d3d12::Compile(input.graph, input.targetProfile);
    if (!compiled)
        return sge::base::Result<core::FrozenExecutablePackage, std::string>::Failure(
            compiled.Error().stage + ": " + compiled.Error().message);
    auto package = core::PackageReader::Read(std::move(compiled).Value().packageBytes);
    if (!package)
        return sge::base::Result<core::FrozenExecutablePackage, std::string>::Failure(package.Error().message);
    auto decoded = pkg::D3D12PackageView::Decode(package.Value());
    if (!decoded)
        return sge::base::Result<core::FrozenExecutablePackage, std::string>::Failure(decoded.Error().message);
    return sge::base::Result<core::FrozenExecutablePackage, std::string>::Success(std::move(package).Value());
}

struct Harness final
{
    int failures = 0;
    std::size_t readerRejected = 0;
    std::size_t writerRejected = 0;
    std::size_t schemaRejected = 0;

    void Fail(std::string_view label, std::string_view detail)
    {
        ++failures;
        std::cerr << "  FAILED: " << label << ": " << detail << '\n';
    }

    void ExpectReaderReject(std::string_view label,
                            std::vector<std::byte> bytes,
                            core::PackageErrorCode expected)
    {
        auto result = core::PackageReader::Read(std::move(bytes));
        if (result || result.Error().code != expected)
        {
            Fail(label, result ? "corruption was accepted" : result.Error().message);
            return;
        }
        ++readerRejected;
    }

    void ExpectWriterReject(std::string_view label,
                            core::PackageBuildInput input,
                            core::PackageErrorCode expected)
    {
        auto result = core::PackageWriter::Write(std::move(input));
        if (result || result.Error().code != expected)
        {
            Fail(label, result ? "invalid build input was accepted" : result.Error().message);
            return;
        }
        ++writerRejected;
    }

    void ExpectSchemaReject(std::string_view label,
                            core::PackageBuildInput input,
                            std::optional<core::PackageErrorCode> expected = std::nullopt)
    {
        auto bytes = core::PackageWriter::Write(std::move(input));
        if (!bytes)
        {
            Fail(label, std::string("PackageWriter rejected mutation before schema validation: ") + bytes.Error().message);
            return;
        }
        auto package = core::PackageReader::Read(std::move(bytes).Value());
        if (!package)
        {
            Fail(label, std::string("PackageReader rejected re-digested mutation: ") + package.Error().message);
            return;
        }
        auto decoded = pkg::D3D12PackageView::Decode(package.Value());
        if (decoded || (expected && decoded.Error().code != *expected))
        {
            Fail(label, decoded ? "adversarial Package was accepted" : decoded.Error().message);
            return;
        }
        ++schemaRejected;
    }
};

int ValidateOperationRegistry(const core::FrozenExecutablePackage& rich,
                              const core::FrozenExecutablePackage& temporal,
                              const core::FrozenExecutablePackage& surface)
{
    const auto contracts = pkg::OperationContracts();
    if (contracts.size() != 26) return 1;
    std::set<std::uint32_t> codes;
    for (const auto& contract : contracts)
    {
        if (contract.version == 0 || !codes.insert(static_cast<std::uint32_t>(contract.code)).second ||
            pkg::OperationVersion(contract.code) != contract.version || !pkg::IsKnownOperation(contract.code))
            return 2;
    }
    if (pkg::IsKnownOperation(static_cast<pkg::D3D12OperationCode>(0xFFFF'FFFFu)) ||
        pkg::OperationVersion(static_cast<pkg::D3D12OperationCode>(0xFFFF'FFFFu)) != 0)
        return 3;

    for (const auto* package : {&rich, &temporal, &surface})
    {
        auto decoded = pkg::D3D12PackageView::Decode(*package);
        if (!decoded) return 4;
        for (const auto& operation : decoded.Value().LoadOperations())
            if (!pkg::IsKnownOperation(operation.opcode) ||
                operation.operationVersion != pkg::OperationVersion(operation.opcode)) return 5;
        for (const auto& operation : decoded.Value().FrameOperations())
            if (!pkg::IsKnownOperation(operation.opcode) ||
                operation.operationVersion != pkg::OperationVersion(operation.opcode)) return 6;
    }
    return 0;
}

void ReaderMutations(Harness& test, const core::FrozenExecutablePackage& package)
{
    const std::vector<std::byte> original(package.FileBytes().begin(), package.FileBytes().end());
    {
        auto bytes = original; bytes[0] ^= std::byte{1};
        test.ExpectReaderReject("container/bad-magic", std::move(bytes), core::PackageErrorCode::BadMagic);
    }
    {
        auto bytes = original; WriteU32(bytes, 28, 1);
        test.ExpectReaderReject("container/header-flags", std::move(bytes), core::PackageErrorCode::UnsupportedContainerVersion);
    }
    {
        auto bytes = original; WriteU32(bytes, 56, 0);
        test.ExpectReaderReject("container/zero-section-count", std::move(bytes), core::PackageErrorCode::InvalidSectionTable);
    }
    {
        auto bytes = original; WriteU32(bytes, 60, 1);
        test.ExpectReaderReject("container/header-reserved", std::move(bytes), core::PackageErrorCode::InvalidSectionTable);
    }
    {
        auto bytes = original; WriteU64(bytes, 40, original.size() + 1u);
        test.ExpectReaderReject("container/file-size", std::move(bytes), core::PackageErrorCode::FileSizeMismatch);
    }
    {
        auto input = RebuildInput(package);
        input.sections.front().schemaVersion = 2;
        auto bytes = core::PackageWriter::Write(std::move(input));
        if (!bytes) test.Fail("container/section-schema-version", bytes.Error().message);
        else test.ExpectReaderReject("container/section-schema-version", std::move(bytes).Value(), core::PackageErrorCode::UnsupportedContainerVersion);
    }
    {
        auto input = RebuildInput(package);
        input.sections.front().flags |= 0x8000'0000u;
        auto bytes = core::PackageWriter::Write(std::move(input));
        if (!bytes) test.Fail("container/section-flags", bytes.Error().message);
        else test.ExpectReaderReject("container/section-flags", std::move(bytes).Value(), core::PackageErrorCode::UnsupportedContainerVersion);
    }
    {
        auto bytes = original; bytes.back() ^= std::byte{0x40};
        test.ExpectReaderReject("container/file-digest", std::move(bytes), core::PackageErrorCode::DigestMismatch);
    }
    {
        auto bytes = original; bytes.resize(bytes.size() - 1);
        test.ExpectReaderReject("container/truncation", std::move(bytes), core::PackageErrorCode::FileSizeMismatch);
    }
}

void WriterMutations(Harness& test, const core::FrozenExecutablePackage& package)
{
    const auto base = RebuildInput(package);
    {
        auto input = base; input.sections.push_back(input.sections.front());
        test.ExpectWriterReject("writer/duplicate-section", std::move(input), core::PackageErrorCode::DuplicateSection);
    }
    {
        auto input = base; input.sections.front().alignment = 3;
        test.ExpectWriterReject("writer/non-power-of-two-alignment", std::move(input), core::PackageErrorCode::InvalidAlignment);
    }
    {
        auto input = base;
        auto& table = Section(input, core::SectionKind::D3D12ResourceTable);
        table.elementStride += 1;
        test.ExpectWriterReject("writer/table-stride", std::move(input), core::PackageErrorCode::InvalidRecordStride);
    }
    {
        auto input = base;
        std::erase_if(input.sections, [](const auto& section) {
            return section.kind == core::SectionKind::D3D12TargetProfile;
        });
        test.ExpectWriterReject("writer/missing-target-profile", std::move(input), core::PackageErrorCode::MissingRequiredSection);
    }
}

void ReferenceMutations(Harness& test, const core::FrozenExecutablePackage& package)
{
    const auto base = RebuildInput(package);
    auto mutate = [&](std::string_view label, const std::function<void(core::PackageBuildInput&)>& function,
                      std::optional<core::PackageErrorCode> code = std::nullopt) {
        auto input = base; function(input); test.ExpectSchemaReject(label, std::move(input), code);
    };

    mutate("reference/manifest-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::Manifest).bytes, 68, 1);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/manifest-count", [](auto& input) {
        auto& bytes = Section(input, core::SectionKind::Manifest).bytes;
        WriteU32(bytes, 0, ReadU32(bytes, 0) + 1);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/profile-queue-count", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12TargetProfile).bytes, 24, 2);
    }, core::PackageErrorCode::InvalidTargetProfile);
    mutate("reference/profile-feature-bits", [](auto& input) {
        WriteU64(Section(input, core::SectionKind::D3D12TargetProfile).bytes, 56, 1);
    }, core::PackageErrorCode::InvalidTargetProfile);
    mutate("reference/resource-first-view", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12ResourceTable).bytes, 80, 1);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/resource-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12ResourceTable).bytes, 12, 0x8000'0000u);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/resource-instance-count", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12ResourceTable).bytes, 16, 0);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/allocation-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12AllocationTable).bytes, 8, 1);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/allocation-alignment", [](auto& input) {
        WriteU64(Section(input, core::SectionKind::D3D12AllocationTable).bytes, 24, 3);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/allocation-alias-contract", [](auto& input) {
        WriteU16(Section(input, core::SectionKind::D3D12AllocationTable).bytes, 4,
                 static_cast<std::uint16_t>(pkg::AllocationKind::Placed));
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/view-descriptor-overlap", [](auto& input) {
        auto& views = Section(input, core::SectionKind::D3D12ViewTable);
        WriteU32(views.bytes, views.elementStride * 3u + 60u, 0);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/view-class", [](auto& input) {
        WriteU16(Section(input, core::SectionKind::D3D12ViewTable).bytes, 8,
                 static_cast<std::uint16_t>(pkg::ViewClass::RenderTarget));
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/view-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12ViewTable).bytes, 16, 0x8000'0000u);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/shader-inner-digest", [](auto& input) {
        auto& shaders = Section(input, core::SectionKind::D3D12ShaderTable);
        shaders.bytes[40] ^= std::byte{1};
    }, core::PackageErrorCode::DigestMismatch);
    mutate("reference/program-flags", [](auto& input) {
        WriteU16(Section(input, core::SectionKind::D3D12ProgramTable).bytes, 6, 1);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/program-stage", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12ProgramTable).bytes, 8, 0);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/binding-layout-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12BindingLayoutTable).bytes, 8, 1);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/root-parameter-visibility", [](auto& input) {
        WriteU16(Section(input, core::SectionKind::D3D12RootParameterTable).bytes, 6, 0xffffu);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/compute-command-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12ComputeCommandTable).bytes, 20, 1);
    }, core::PackageErrorCode::InvalidReference);
    mutate("reference/dynamic-slot-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12DynamicSlotTable).bytes, 28, 1);
    }, core::PackageErrorCode::InvalidInvocationSchema);
    mutate("reference/external-slot-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12ExternalSlotTable).bytes, 44, 0);
    }, core::PackageErrorCode::InvalidInvocationSchema);
    mutate("reference/external-slot-duplicate-resource", [](auto& input) {
        auto& slots = Section(input, core::SectionKind::D3D12ExternalSlotTable);
        WriteU32(slots.bytes, slots.elementStride + 4, ReadU32(slots.bytes, 4));
    }, core::PackageErrorCode::InvalidInvocationSchema);
    mutate("reference/external-size", [](auto& input) {
        WriteU64(Section(input, core::SectionKind::D3D12ExternalSlotTable).bytes, 16, 32);
    }, core::PackageErrorCode::InvalidInvocationSchema);
}

void OperationMutations(Harness& test, const core::FrozenExecutablePackage& package)
{
    const auto base = RebuildInput(package);
    auto mutate = [&](std::string_view label, const std::function<void(core::PackageBuildInput&)>& function) {
        auto input = base; function(input); test.ExpectSchemaReject(label, std::move(input), core::PackageErrorCode::InvalidOperationStream);
    };

    mutate("operation/unknown-opcode", [](auto& input) {
        auto& operations = Section(input, core::SectionKind::OperationTable);
        WriteU32(operations.bytes, 0, 0xffff'ffffu);
    });
    mutate("operation/version", [](auto& input) {
        auto& operations = Section(input, core::SectionKind::OperationTable);
        WriteU16(operations.bytes, 4, ReadU16(operations.bytes, 4) + 1);
    });
    mutate("operation/flags", [](auto& input) {
        WriteU16(Section(input, core::SectionKind::OperationTable).bytes, 6, 1);
    });
    mutate("operation/queue", [](auto& input) {
        const auto operation = FindOperation(input, pkg::D3D12OperationCode::ExecuteCompute);
        auto& operations = Section(input, core::SectionKind::OperationTable);
        WriteU32(operations.bytes, operation.index * operations.elementStride + 8, 0xffff'fffeu);
    });
    mutate("operation/noncanonical-payload-offset", [](auto& input) {
        auto& operations = Section(input, core::SectionKind::OperationTable);
        WriteU64(operations.bytes, 16, ReadU64(operations.bytes, 16) + 8);
    });
    mutate("operation/trailing-payload", [](auto& input) {
        auto& payload = Section(input, core::SectionKind::OperationPayload);
        payload.bytes.resize(payload.bytes.size() + 8);
    });
    mutate("operation/stream-overlap", [](auto& input) {
        auto& streams = Section(input, core::SectionKind::OperationStreamTable);
        WriteU32(streams.bytes, streams.elementStride + 4, 0);
    });
    mutate("operation/stream-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::OperationStreamTable).bytes, 12, 1);
    });
    mutate("operation/transition-state-bits", [](auto& input) {
        const auto operation = FindOperation(input, pkg::D3D12OperationCode::Transition);
        auto& payload = Section(input, core::SectionKind::OperationPayload);
        WriteU32(payload.bytes, operation.payloadOffset + 12, 0x8000'0000u);
    });
    mutate("operation/copy-view-class", [](auto& input) {
        const auto operation = FindOperation(input, pkg::D3D12OperationCode::ExecuteCopy);
        auto& payload = Section(input, core::SectionKind::OperationPayload);
        WriteU32(payload.bytes, operation.payloadOffset, 0);
    });
    mutate("operation/future-signal", [](auto& input) {
        const auto operation = FindOperation(input, pkg::D3D12OperationCode::WaitQueue);
        auto& payload = Section(input, core::SectionKind::OperationPayload);
        WriteU32(payload.bytes, operation.payloadOffset, 0xffff'fffeu);
    });
    mutate("operation/duplicate-dynamic-slot", [](auto& input) {
        if (CountOperations(input, pkg::D3D12OperationCode::ApplyDynamicData) < 2) std::terminate();
        const auto first = FindOperation(input, pkg::D3D12OperationCode::ApplyDynamicData, 0);
        const auto second = FindOperation(input, pkg::D3D12OperationCode::ApplyDynamicData, 1);
        auto& payload = Section(input, core::SectionKind::OperationPayload);
        WriteU32(payload.bytes, second.payloadOffset, ReadU32(payload.bytes, first.payloadOffset));
    });
    mutate("operation/missing-external-release", [](auto& input) {
        const auto operation = FindOperation(input, pkg::D3D12OperationCode::ReleaseExternal);
        auto& operations = Section(input, core::SectionKind::OperationTable);
        const auto record = operation.index * operations.elementStride;
        WriteU32(operations.bytes, record, static_cast<std::uint32_t>(pkg::D3D12OperationCode::SignalQueue));
        WriteU16(operations.bytes, record + 4, pkg::OperationVersion(pkg::D3D12OperationCode::SignalQueue));
    });
}

void SurfaceMutations(Harness& test, const core::FrozenExecutablePackage& package)
{
    const auto base = RebuildInput(package);
    auto mutate = [&](std::string_view label, const std::function<void(core::PackageBuildInput&)>& function,
                      core::PackageErrorCode code) {
        auto input = base; function(input); test.ExpectSchemaReject(label, std::move(input), code);
    };
    mutate("surface/profile-count", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12TargetProfile).bytes, 36, 0);
    }, core::PackageErrorCode::InvalidReference);
    mutate("surface/slot-flags", [](auto& input) {
        WriteU32(Section(input, core::SectionKind::D3D12SurfaceSlotTable).bytes, 28, 1);
    }, core::PackageErrorCode::InvalidInvocationSchema);
    mutate("surface/present-slot", [](auto& input) {
        const auto operation = FindOperation(input, pkg::D3D12OperationCode::PresentSurface);
        WriteU32(Section(input, core::SectionKind::OperationPayload).bytes,
                 operation.payloadOffset, 0xffff'fffeu);
    }, core::PackageErrorCode::InvalidOperationStream);
    mutate("surface/raster-color-view", [](auto& input) {
        auto& command = Section(input, core::SectionKind::D3D12RasterCommandTable);
        WriteU32(command.bytes, 20, ReadU32(command.bytes, 8));
    }, core::PackageErrorCode::InvalidReference);
}
}

int main()
{
    std::cout << "Stage-L adversarial boundary / backend conformance...\n";

    auto richInput = runtime_cases::Build(runtime_cases::Scenario::DynamicExternalPipeline);
    auto temporalInput = runtime_cases::Build(runtime_cases::Scenario::CrossQueueTemporal);
    auto surfaceInput = sge::level2::BuildStageHScenario(sge::level2::StageHScenario::TwoPassRaster);
    if (!richInput || !temporalInput || !surfaceInput)
    {
        std::cerr << "Stage-L scenario construction failed\n";
        return 1;
    }
    auto rich = Compile(richInput.Value());
    auto temporal = Compile(temporalInput.Value());
    auto surface = Compile(surfaceInput.Value());
    if (!rich || !temporal || !surface)
    {
        std::cerr << "Stage-L baseline Package compile/decode failed: "
                  << (!rich ? rich.Error() : !temporal ? temporal.Error() : surface.Error()) << '\n';
        return 2;
    }

    if (const auto conformance = ValidateOperationRegistry(rich.Value(), temporal.Value(), surface.Value());
        conformance != 0)
    {
        std::cerr << "Stage-L operation registry conformance failed: " << conformance << '\n';
        return 3;
    }
    std::cout << "  operation registry: 26 opcode/version contracts are unique and match every baseline Package.\n";

    Harness test;
    ReaderMutations(test, rich.Value());
    WriterMutations(test, rich.Value());
    ReferenceMutations(test, rich.Value());
    OperationMutations(test, rich.Value());
    SurfaceMutations(test, surface.Value());

    if (test.failures != 0)
    {
        std::cerr << "Stage-L failed with " << test.failures << " accepted or misclassified mutation(s).\n";
        return 4;
    }

    std::cout << "  core reader: " << test.readerRejected
              << " corrupt container mutations rejected before Package decode.\n";
    std::cout << "  core writer: " << test.writerRejected
              << " invalid construction contracts rejected before serialization.\n";
    std::cout << "  D3D12 schema: " << test.schemaRejected
              << " re-digested reference/operation mutations rejected before D3D12 materialization.\n";
    std::cout << "Stage-L adversarial boundary and backend conformance passed.\n";
    return 0;
}
