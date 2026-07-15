#include "PackageReader.h"

#include "../00_Base/BinaryIO.h"
#include "../00_Base/CheckedMath.h"
#include "../00_Base/Sha256.h"
#include "PackageDigest.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace sge::package
{
namespace
{
PackageError Error(PackageErrorCode code, std::string message, SectionKind section = {})
{
    PackageError error;
    error.code = code;
    error.section = section;
    error.message = std::move(message);
    return error;
}

template<class T>
bool Pull(base::Result<T, std::string>& result, T& out, PackageError& error)
{
    if (!result) { error = Error(PackageErrorCode::InvalidSectionTable, result.Error()); return false; }
    out = result.Value();
    return true;
}

bool PullBytes(base::Result<std::span<const std::byte>, std::string>& result, std::span<const std::byte>& out, PackageError& error)
{
    if (!result) { error = Error(PackageErrorCode::InvalidSectionTable, result.Error()); return false; }
    out = result.Value();
    return true;
}

bool IsKnownSectionKind(SectionKind kind)
{
    switch (kind)
    {
    case SectionKind::Manifest:
    case SectionKind::InvocationSchema:
    case SectionKind::OperationStreamTable:
    case SectionKind::OperationTable:
    case SectionKind::OperationPayload:
    case SectionKind::InitialData:
    case SectionKind::ShaderData:
    case SectionKind::NativeObjectData:
    case SectionKind::D3D12TargetProfile:
    case SectionKind::D3D12ResourceTable:
    case SectionKind::D3D12AllocationTable:
    case SectionKind::D3D12ViewTable:
    case SectionKind::D3D12ShaderTable:
    case SectionKind::D3D12ProgramTable:
    case SectionKind::D3D12BindingLayoutTable:
    case SectionKind::D3D12ExecutableTable:
    case SectionKind::D3D12RasterCommandTable:
    case SectionKind::D3D12DescriptorPlan:
    case SectionKind::D3D12DynamicSlotTable:
    case SectionKind::D3D12ExternalSlotTable:
    case SectionKind::D3D12SurfaceSlotTable:
    case SectionKind::D3D12VertexElementTable:
    case SectionKind::D3D12AttachmentOperationTable:
    case SectionKind::D3D12RootParameterTable:
    case SectionKind::D3D12ComputeExecutableTable:
    case SectionKind::D3D12ComputeCommandTable:
    case SectionKind::StringTable:
    case SectionKind::DebugMap:
    case SectionKind::Provenance:
        return true;
    default:
        return false;
    }
}

base::Result<PackageHeader, PackageError> ReadHeader(std::span<const std::byte> bytes)
{
    if (bytes.size() < HeaderBytes)
        return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::FileSizeMismatch, "file is smaller than the fixed package header"));
    if (!std::equal(PackageMagic.begin(), PackageMagic.end(), bytes.begin()))
        return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::BadMagic, "package magic does not match SGE2PKG"));

    base::BinaryReader reader(bytes.subspan(8, HeaderBytes - 8));
    PackageHeader header;
    PackageError readError;
    auto u16 = reader.ReadU16(); if (!Pull(u16, header.formatMajor, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u16 = reader.ReadU16(); if (!Pull(u16, header.formatMinor, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u16 = reader.ReadU16(); if (!Pull(u16, header.headerBytes, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u16 = reader.ReadU16(); if (!Pull(u16, header.sectionDescriptorBytes, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    auto u32 = reader.ReadU32(); if (!Pull(u32, header.targetKind, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u32 = reader.ReadU32(); if (!Pull(u32, header.targetSchemaVersion, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u32 = reader.ReadU32(); if (!Pull(u32, header.minimumRuntimeVersion, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u32 = reader.ReadU32(); if (!Pull(u32, header.flags, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u32 = reader.ReadU32(); if (!Pull(u32, header.endianTag, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u32 = reader.ReadU32(); if (!Pull(u32, header.digestAlgorithm, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    auto u64 = reader.ReadU64(); if (!Pull(u64, header.fileBytes, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u64 = reader.ReadU64(); if (!Pull(u64, header.sectionTableOffset, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    u32 = reader.ReadU32(); if (!Pull(u32, header.sectionCount, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    std::uint32_t reserved0 = 1; u32 = reader.ReadU32(); if (!Pull(u32, reserved0, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    if (reserved0 != 0) return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::InvalidSectionTable, "header reserved0 must be zero"));
    std::span<const std::byte> digestBytes;
    auto raw = reader.ReadBytes(32); if (!PullBytes(raw, digestBytes, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError); std::copy(digestBytes.begin(), digestBytes.end(), header.targetProfileDigest.begin());
    raw = reader.ReadBytes(32); if (!PullBytes(raw, digestBytes, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError); std::copy(digestBytes.begin(), digestBytes.end(), header.executionDigest.begin());
    raw = reader.ReadBytes(32); if (!PullBytes(raw, digestBytes, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError); std::copy(digestBytes.begin(), digestBytes.end(), header.fileDigest.begin());
    raw = reader.ReadBytes(16); if (!PullBytes(raw, digestBytes, readError)) return base::Result<PackageHeader, PackageError>::Failure(readError);
    if (std::any_of(digestBytes.begin(), digestBytes.end(), [](std::byte value) { return value != std::byte{0}; }))
        return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::InvalidSectionTable, "header reserved1 must be zero"));

    if (header.formatMajor != ContainerFormatMajor || header.formatMinor > ContainerFormatMinor)
        return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::UnsupportedContainerVersion, "unsupported package container version"));
    if (header.headerBytes != HeaderBytes || header.sectionDescriptorBytes != SectionDescriptorBytes)
        return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::UnsupportedContainerVersion, "unexpected fixed header or section descriptor size"));
    if (header.endianTag != EndianTag)
        return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::WrongEndianness, "package is not little endian"));
    if (header.digestAlgorithm != DigestAlgorithmSha256)
        return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::UnsupportedContainerVersion, "unsupported digest algorithm"));
    if (header.fileBytes != bytes.size())
        return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::FileSizeMismatch, "header fileBytes does not match the actual file size"));
    if (header.sectionTableOffset != HeaderBytes)
        return base::Result<PackageHeader, PackageError>::Failure(Error(PackageErrorCode::InvalidSectionTable, "V1 section table must immediately follow the header"));
    return base::Result<PackageHeader, PackageError>::Success(header);
}

base::Result<std::vector<SectionDescriptor>, PackageError> ReadDescriptors(
    std::span<const std::byte> bytes,
    const PackageHeader& header)
{
    std::uint64_t tableBytes = static_cast<std::uint64_t>(header.sectionCount) * SectionDescriptorBytes;
    std::uint64_t tableEnd = 0;
    if (!base::CheckedAdd(header.sectionTableOffset, tableBytes, tableEnd) || tableEnd > bytes.size())
        return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(PackageErrorCode::InvalidSectionTable, "section table is out of bounds"));

    base::BinaryReader reader(bytes.subspan(static_cast<std::size_t>(header.sectionTableOffset), static_cast<std::size_t>(tableBytes)));
    std::vector<SectionDescriptor> descriptors;
    descriptors.reserve(header.sectionCount);
    for (std::uint32_t index = 0; index < header.sectionCount; ++index)
    {
        SectionDescriptor descriptor;
        PackageError readError;
        std::uint32_t kind = 0;
        auto u32 = reader.ReadU32(); if (!Pull(u32, kind, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError); descriptor.sectionKind = static_cast<SectionKind>(kind);
        auto u16 = reader.ReadU16(); if (!Pull(u16, descriptor.schemaVersion, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError);
        u16 = reader.ReadU16(); if (!Pull(u16, descriptor.descriptorBytes, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError);
        u32 = reader.ReadU32(); if (!Pull(u32, descriptor.flags, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError);
        u32 = reader.ReadU32(); if (!Pull(u32, descriptor.alignment, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError);
        auto u64 = reader.ReadU64(); if (!Pull(u64, descriptor.fileOffset, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError);
        u64 = reader.ReadU64(); if (!Pull(u64, descriptor.storedBytes, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError);
        u64 = reader.ReadU64(); if (!Pull(u64, descriptor.logicalBytes, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError);
        u32 = reader.ReadU32(); if (!Pull(u32, descriptor.elementCount, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError);
        u32 = reader.ReadU32(); if (!Pull(u32, descriptor.elementStride, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError);
        auto raw = reader.ReadBytes(32); std::span<const std::byte> digest; if (!PullBytes(raw, digest, readError)) return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(readError); std::copy(digest.begin(), digest.end(), descriptor.sectionDigest.begin());

        if (descriptor.descriptorBytes != SectionDescriptorBytes)
            return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(PackageErrorCode::UnsupportedContainerVersion, "section descriptor size mismatch", descriptor.sectionKind));
        if (HasFlag(descriptor.flags, SectionFlags::Required) && !IsKnownSectionKind(descriptor.sectionKind))
            return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(PackageErrorCode::UnknownRequiredSection, "unknown required section", descriptor.sectionKind));
        if (descriptor.fileOffset < tableEnd)
            return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(PackageErrorCode::SectionOverlap, "section overlaps the header or section table", descriptor.sectionKind));
        if (!base::IsPowerOfTwo(descriptor.alignment) || descriptor.fileOffset % descriptor.alignment != 0)
            return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(PackageErrorCode::InvalidAlignment, "section alignment is invalid", descriptor.sectionKind));
        if (descriptor.logicalBytes != descriptor.storedBytes)
            return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(PackageErrorCode::UnsupportedContainerVersion, "V1 does not permit compressed sections", descriptor.sectionKind));
        if (descriptor.elementCount != 0 && (descriptor.elementStride == 0 || static_cast<std::uint64_t>(descriptor.elementCount) * descriptor.elementStride != descriptor.logicalBytes))
            return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "section table record dimensions are inconsistent", descriptor.sectionKind));
        std::uint64_t end = 0;
        if (!base::CheckedAdd(descriptor.fileOffset, descriptor.storedBytes, end) || end > bytes.size())
            return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(PackageErrorCode::SectionOutOfBounds, "section lies outside the file", descriptor.sectionKind));
        if (!descriptors.empty() && static_cast<std::uint32_t>(descriptors.back().sectionKind) >= kind)
            return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(
                descriptors.back().sectionKind == descriptor.sectionKind ? PackageErrorCode::DuplicateSection : PackageErrorCode::InvalidSectionTable,
                "sections must be unique and sorted by kind", descriptor.sectionKind));
        descriptors.push_back(descriptor);
    }

    std::vector<SectionDescriptor> byOffset = descriptors;
    std::sort(byOffset.begin(), byOffset.end(), [](const auto& a, const auto& b) { return a.fileOffset < b.fileOffset; });
    for (std::size_t i = 1; i < byOffset.size(); ++i)
    {
        const auto previousEnd = byOffset[i - 1].fileOffset + byOffset[i - 1].storedBytes;
        if (previousEnd > byOffset[i].fileOffset)
            return base::Result<std::vector<SectionDescriptor>, PackageError>::Failure(Error(PackageErrorCode::SectionOverlap, "sections overlap", byOffset[i].sectionKind));
    }
    return base::Result<std::vector<SectionDescriptor>, PackageError>::Success(std::move(descriptors));
}
}

base::Result<FrozenExecutablePackage, PackageError> PackageReader::Read(std::span<const std::byte> bytes)
{
    return Read(std::vector<std::byte>(bytes.begin(), bytes.end()));
}

base::Result<FrozenExecutablePackage, PackageError> PackageReader::Read(std::vector<std::byte> bytes)
{
    const auto headerResult = ReadHeader(bytes);
    if (!headerResult) return base::Result<FrozenExecutablePackage, PackageError>::Failure(headerResult.Error());
    const auto header = headerResult.Value();

    if (ComputeFileDigest(bytes) != header.fileDigest)
        return base::Result<FrozenExecutablePackage, PackageError>::Failure(Error(PackageErrorCode::DigestMismatch, "file digest mismatch"));

    const auto descriptorsResult = ReadDescriptors(bytes, header);
    if (!descriptorsResult) return base::Result<FrozenExecutablePackage, PackageError>::Failure(descriptorsResult.Error());

    auto storage = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    FrozenExecutablePackage package;
    package.storage_ = storage;
    package.header_ = header;
    for (const auto& descriptor : descriptorsResult.Value())
    {
        const auto sectionBytes = std::span<const std::byte>(*storage).subspan(
            static_cast<std::size_t>(descriptor.fileOffset),
            static_cast<std::size_t>(descriptor.storedBytes));
        if (base::Sha256(sectionBytes) != descriptor.sectionDigest)
            return base::Result<FrozenExecutablePackage, PackageError>::Failure(Error(PackageErrorCode::DigestMismatch, "section digest mismatch", descriptor.sectionKind));
        package.sections_.push_back(SectionView{descriptor, sectionBytes});
    }

    if (ComputeExecutionDigestFromViews(package.sections_) != header.executionDigest)
        return base::Result<FrozenExecutablePackage, PackageError>::Failure(Error(PackageErrorCode::DigestMismatch, "execution digest mismatch"));

    if (header.targetKind == TargetKindD3D12)
    {
        const auto* profile = package.FindSection(SectionKind::D3D12TargetProfile);
        if (profile == nullptr)
            return base::Result<FrozenExecutablePackage, PackageError>::Failure(Error(PackageErrorCode::MissingRequiredSection, "D3D12 target profile is missing", SectionKind::D3D12TargetProfile));
        if (base::Sha256(profile->bytes) != header.targetProfileDigest)
            return base::Result<FrozenExecutablePackage, PackageError>::Failure(Error(PackageErrorCode::DigestMismatch, "target profile digest mismatch", SectionKind::D3D12TargetProfile));
    }

    return base::Result<FrozenExecutablePackage, PackageError>::Success(std::move(package));
}
}
