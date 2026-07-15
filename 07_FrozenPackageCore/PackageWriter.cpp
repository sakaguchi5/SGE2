#include "PackageWriter.h"

#include "../00_Base/BinaryIO.h"
#include "../00_Base/CheckedMath.h"
#include "../00_Base/Sha256.h"
#include "PackageDigest.h"

#include <algorithm>
#include <exception>

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

void WriteHeader(base::BinaryWriter& writer, const PackageHeader& header)
{
    writer.WriteBytes(PackageMagic);
    writer.WriteU16(header.formatMajor);
    writer.WriteU16(header.formatMinor);
    writer.WriteU16(header.headerBytes);
    writer.WriteU16(header.sectionDescriptorBytes);
    writer.WriteU32(header.targetKind);
    writer.WriteU32(header.targetSchemaVersion);
    writer.WriteU32(header.minimumRuntimeVersion);
    writer.WriteU32(header.flags);
    writer.WriteU32(header.endianTag);
    writer.WriteU32(header.digestAlgorithm);
    writer.WriteU64(header.fileBytes);
    writer.WriteU64(header.sectionTableOffset);
    writer.WriteU32(header.sectionCount);
    writer.WriteU32(0);
    writer.WriteBytes(header.targetProfileDigest);
    writer.WriteBytes(header.executionDigest);
    writer.WriteBytes(header.fileDigest);
    writer.WriteZeroes(16);
}

void WriteDescriptor(base::BinaryWriter& writer, const SectionDescriptor& descriptor)
{
    writer.WriteU32(static_cast<std::uint32_t>(descriptor.sectionKind));
    writer.WriteU16(descriptor.schemaVersion);
    writer.WriteU16(descriptor.descriptorBytes);
    writer.WriteU32(descriptor.flags);
    writer.WriteU32(descriptor.alignment);
    writer.WriteU64(descriptor.fileOffset);
    writer.WriteU64(descriptor.storedBytes);
    writer.WriteU64(descriptor.logicalBytes);
    writer.WriteU32(descriptor.elementCount);
    writer.WriteU32(descriptor.elementStride);
    writer.WriteBytes(descriptor.sectionDigest);
}
}

base::Result<std::vector<std::byte>, PackageError> PackageWriter::Write(PackageBuildInput input)
{
    try
    {
        if (input.targetKind == 0)
            return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::UnsupportedTarget, "target kind must be explicit"));

        std::sort(input.sections.begin(), input.sections.end(), [](const auto& a, const auto& b) {
            return static_cast<std::uint32_t>(a.kind) < static_cast<std::uint32_t>(b.kind);
        });
        for (std::size_t i = 0; i < input.sections.size(); ++i)
        {
            const auto& section = input.sections[i];
            if (!base::IsPowerOfTwo(section.alignment))
                return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::InvalidAlignment, "section alignment must be a non-zero power of two", section.kind));
            if (i != 0 && input.sections[i - 1].kind == section.kind)
                return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::DuplicateSection, "duplicate section kind", section.kind));
            if (section.elementCount != 0 && section.elementStride == 0)
                return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "table section has zero element stride", section.kind));
            if (section.elementCount != 0 && static_cast<std::uint64_t>(section.elementCount) * section.elementStride != section.bytes.size())
                return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "table bytes do not match element count and stride", section.kind));
        }

        const auto profileIt = std::find_if(input.sections.begin(), input.sections.end(), [](const auto& section) {
            return section.kind == SectionKind::D3D12TargetProfile;
        });
        if (input.targetKind == TargetKindD3D12 && profileIt == input.sections.end())
            return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::MissingRequiredSection, "D3D12 package requires a target profile", SectionKind::D3D12TargetProfile));

        PackageHeader header;
        header.targetKind = input.targetKind;
        header.targetSchemaVersion = input.targetSchemaVersion;
        header.minimumRuntimeVersion = input.minimumRuntimeVersion;
        header.flags = input.flags;
        header.sectionCount = static_cast<std::uint32_t>(input.sections.size());
        if (profileIt != input.sections.end()) header.targetProfileDigest = base::Sha256(profileIt->bytes);
        header.executionDigest = ComputeExecutionDigest(input.sections);

        std::vector<SectionDescriptor> descriptors;
        descriptors.reserve(input.sections.size());
        std::uint64_t cursor = HeaderBytes + static_cast<std::uint64_t>(input.sections.size()) * SectionDescriptorBytes;
        for (const auto& section : input.sections)
        {
            cursor = base::AlignUp(cursor, section.alignment);
            SectionDescriptor descriptor;
            descriptor.sectionKind = section.kind;
            descriptor.schemaVersion = section.schemaVersion;
            descriptor.flags = section.flags;
            descriptor.alignment = section.alignment;
            descriptor.fileOffset = cursor;
            descriptor.storedBytes = section.bytes.size();
            descriptor.logicalBytes = section.bytes.size();
            descriptor.elementCount = section.elementCount;
            descriptor.elementStride = section.elementStride;
            descriptor.sectionDigest = base::Sha256(section.bytes);
            descriptors.push_back(descriptor);
            std::uint64_t next = 0;
            if (!base::CheckedAdd(cursor, descriptor.storedBytes, next))
                return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::SectionOutOfBounds, "package size overflow", section.kind));
            cursor = next;
        }
        header.fileBytes = cursor;

        base::BinaryWriter writer;
        WriteHeader(writer, header);
        for (const auto& descriptor : descriptors) WriteDescriptor(writer, descriptor);
        for (std::size_t i = 0; i < input.sections.size(); ++i)
        {
            writer.Align(input.sections[i].alignment);
            if (writer.Size() != descriptors[i].fileOffset)
                return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::SerializationFailure, "internal section layout mismatch", input.sections[i].kind));
            writer.WriteBytes(input.sections[i].bytes);
        }
        if (writer.Size() != header.fileBytes)
            return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::SerializationFailure, "internal file size mismatch"));

        auto bytes = std::move(writer).Take();
        header.fileDigest = ComputeFileDigest(bytes);
        std::copy(header.fileDigest.begin(), header.fileDigest.end(), bytes.begin() + 0x80);
        return base::Result<std::vector<std::byte>, PackageError>::Success(std::move(bytes));
    }
    catch (const std::exception& exception)
    {
        return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::SerializationFailure, exception.what()));
    }
}
}
