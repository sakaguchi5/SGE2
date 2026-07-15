#pragma once

#include "../00_Base/Sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sge::package
{
inline constexpr std::array<std::byte, 8> PackageMagic = {
    std::byte{'S'}, std::byte{'G'}, std::byte{'E'}, std::byte{'2'},
    std::byte{'P'}, std::byte{'K'}, std::byte{'G'}, std::byte{0}
};
inline constexpr std::uint16_t ContainerFormatMajor = 1;
inline constexpr std::uint16_t ContainerFormatMinor = 0;
inline constexpr std::uint16_t HeaderBytes = 176;
inline constexpr std::uint16_t SectionDescriptorBytes = 80;
inline constexpr std::uint32_t EndianTag = 0x01020304u;
inline constexpr std::uint32_t DigestAlgorithmSha256 = 1;
inline constexpr std::uint32_t TargetKindD3D12 = 1;
inline constexpr std::uint32_t InvalidIndex = 0xffff'ffffu;

enum class SectionKind : std::uint32_t
{
    Manifest = 0x0000'0001,
    InvocationSchema = 0x0000'0002,
    OperationStreamTable = 0x0000'0003,
    OperationTable = 0x0000'0004,
    OperationPayload = 0x0000'0005,
    InitialData = 0x0000'0006,
    ShaderData = 0x0000'0007,
    NativeObjectData = 0x0000'0008,

    D3D12TargetProfile = 0x0000'1000,
    D3D12ResourceTable = 0x0000'1001,
    D3D12AllocationTable = 0x0000'1002,
    D3D12ViewTable = 0x0000'1003,
    D3D12ShaderTable = 0x0000'1004,
    D3D12ProgramTable = 0x0000'1005,
    D3D12BindingLayoutTable = 0x0000'1006,
    D3D12ExecutableTable = 0x0000'1007,
    D3D12RasterCommandTable = 0x0000'1008,
    D3D12DescriptorPlan = 0x0000'1009,
    D3D12DynamicSlotTable = 0x0000'100A,
    D3D12ExternalSlotTable = 0x0000'100B,
    D3D12SurfaceSlotTable = 0x0000'100C,
    D3D12VertexElementTable = 0x0000'100D,
    D3D12AttachmentOperationTable = 0x0000'100E,
    D3D12RootParameterTable = 0x0000'100F,
    D3D12ComputeExecutableTable = 0x0000'1010,
    D3D12ComputeCommandTable = 0x0000'1011,

    StringTable = 0x0000'7F00,
    DebugMap = 0x0000'7F01,
    Provenance = 0x0000'7F02
};

enum class SectionFlags : std::uint32_t
{
    None = 0,
    Required = 1u << 0,
    ExecutionAffecting = 1u << 1,
    DebugOnly = 1u << 2,
    OpaqueToCore = 1u << 3
};

[[nodiscard]] constexpr SectionFlags operator|(SectionFlags a, SectionFlags b) noexcept
{
    return static_cast<SectionFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool HasFlag(std::uint32_t flags, SectionFlags flag) noexcept
{
    return (flags & static_cast<std::uint32_t>(flag)) != 0;
}

struct PackageHeader final
{
    std::uint16_t formatMajor = ContainerFormatMajor;
    std::uint16_t formatMinor = ContainerFormatMinor;
    std::uint16_t headerBytes = HeaderBytes;
    std::uint16_t sectionDescriptorBytes = SectionDescriptorBytes;
    std::uint32_t targetKind = TargetKindD3D12;
    std::uint32_t targetSchemaVersion = 1;
    std::uint32_t minimumRuntimeVersion = 1;
    std::uint32_t flags = 0;
    std::uint32_t endianTag = EndianTag;
    std::uint32_t digestAlgorithm = DigestAlgorithmSha256;
    std::uint64_t fileBytes = 0;
    std::uint64_t sectionTableOffset = HeaderBytes;
    std::uint32_t sectionCount = 0;
    base::Digest256 targetProfileDigest{};
    base::Digest256 executionDigest{};
    base::Digest256 fileDigest{};
};

struct SectionDescriptor final
{
    SectionKind sectionKind{};
    std::uint16_t schemaVersion = 1;
    std::uint16_t descriptorBytes = SectionDescriptorBytes;
    std::uint32_t flags = 0;
    std::uint32_t alignment = 1;
    std::uint64_t fileOffset = 0;
    std::uint64_t storedBytes = 0;
    std::uint64_t logicalBytes = 0;
    std::uint32_t elementCount = 0;
    std::uint32_t elementStride = 0;
    base::Digest256 sectionDigest{};
};

struct PackageSectionInput final
{
    SectionKind kind{};
    std::uint16_t schemaVersion = 1;
    std::uint32_t flags = 0;
    std::uint32_t alignment = 1;
    std::uint32_t elementCount = 0;
    std::uint32_t elementStride = 0;
    std::vector<std::byte> bytes;
};

struct PackageBuildInput final
{
    std::uint32_t targetKind = TargetKindD3D12;
    std::uint32_t targetSchemaVersion = 1;
    std::uint32_t minimumRuntimeVersion = 1;
    std::uint32_t flags = 0;
    std::vector<PackageSectionInput> sections;
};

enum class PackageErrorCode
{
    BadMagic,
    UnsupportedContainerVersion,
    UnsupportedTarget,
    UnsupportedTargetSchema,
    WrongEndianness,
    FileSizeMismatch,
    InvalidSectionTable,
    DuplicateSection,
    MissingRequiredSection,
    UnknownRequiredSection,
    SectionOutOfBounds,
    SectionOverlap,
    InvalidAlignment,
    DigestMismatch,
    InvalidRecordStride,
    InvalidIdSequence,
    InvalidReference,
    InvalidBlobReference,
    InvalidEnumValue,
    InvalidTargetProfile,
    InvalidOperationStream,
    InvalidInvocationSchema,
    TargetCapabilityMismatch,
    SerializationFailure
};

struct PackageError final
{
    PackageErrorCode code = PackageErrorCode::SerializationFailure;
    SectionKind section{};
    std::uint32_t recordIndex = InvalidIndex;
    std::uint32_t operationIndex = InvalidIndex;
    std::string message;
};
}
