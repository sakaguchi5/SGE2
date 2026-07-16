#include "D3D12Encoding.h"

#include "../00_Base/BinaryIO.h"
#include "../00_Base/CheckedMath.h"
#include "../00_Base/Sha256.h"
#include "../07_FrozenPackageCore/PackageWriter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace sge::package::d3d12_v13
{
namespace
{
constexpr std::uint32_t TargetSchemaVersion = 16;
constexpr std::uint32_t MinimumRuntimeVersion = 16;
constexpr std::uint32_t ManifestStride = 72;
constexpr std::uint32_t ProfileStride = 80;
constexpr std::uint32_t ResourceStride = 96;
constexpr std::uint32_t AllocationStride = 40;
constexpr std::uint32_t ViewStride = 72;
constexpr std::uint32_t ShaderStride = 72;
constexpr std::uint32_t ProgramStride = 56;
constexpr std::uint32_t BindingLayoutStride = 104;
constexpr std::uint32_t RootParameterStride = 40;
constexpr std::uint32_t VertexElementStride = 32;
constexpr std::uint32_t ExecutableStride = 104;
constexpr std::uint32_t ComputeExecutableStride = 48;
constexpr std::uint32_t ComputeCommandStride = 24;
constexpr std::uint32_t AttachmentOperationStride = 48;
constexpr std::uint32_t RasterCommandStride = 80;
constexpr std::uint32_t DynamicSlotStride = 32;
constexpr std::uint32_t ExternalSlotStride = 48;
constexpr std::uint32_t SurfaceSlotStride = 32;
constexpr std::uint32_t OperationStreamStride = 16;
constexpr std::uint32_t OperationStride = 24;

PackageError Error(PackageErrorCode code, std::string message, SectionKind section = {})
{
    PackageError error;
    error.code = code;
    error.section = section;
    error.message = std::move(message);
    return error;
}

std::uint32_t Flags(SectionFlags a, SectionFlags b)
{
    return static_cast<std::uint32_t>(a | b);
}

void WriteState(base::BinaryWriter& writer, const ResourceState& state)
{
    writer.WriteU16(static_cast<std::uint16_t>(state.stateClass));
    writer.WriteU16(state.reserved);
    writer.WriteU32(state.explicitBits);
}

base::Result<ResourceState, PackageError> ReadState(base::BinaryReader& reader, SectionKind section)
{
    auto stateClass = reader.ReadU16();
    auto reserved = reader.ReadU16();
    auto bits = reader.ReadU32();
    if (!stateClass || !reserved || !bits)
        return base::Result<ResourceState, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "resource state is truncated", section));
    ResourceState state{static_cast<StateClass>(stateClass.Value()), reserved.Value(), bits.Value()};
    if (state.reserved != 0)
        return base::Result<ResourceState, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "resource state reserved field must be zero", section));
    if (state.stateClass != StateClass::Common && state.stateClass != StateClass::Present && state.stateClass != StateClass::Explicit)
        return base::Result<ResourceState, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "resource state class is unknown", section));
    if (state.stateClass != StateClass::Explicit && state.explicitBits != 0)
        return base::Result<ResourceState, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "non-explicit state contains explicit bits", section));
    return base::Result<ResourceState, PackageError>::Success(state);
}

bool IsCopyQueueState(const ResourceState& state) noexcept
{
    if (state.reserved != 0) return false;
    if (state.stateClass == StateClass::Common) return state.explicitBits == 0;
    if (state.stateClass != StateClass::Explicit) return false;
    const auto copySource = static_cast<std::uint32_t>(ExplicitStateBits::CopySource);
    const auto copyDestination = static_cast<std::uint32_t>(ExplicitStateBits::CopyDestination);
    return state.explicitBits == copySource || state.explicitBits == copyDestination;
}

void WriteBlobRef(base::BinaryWriter& writer, const BlobRef& blob)
{
    writer.WriteU32(static_cast<std::uint32_t>(blob.section));
    writer.WriteU32(blob.reserved);
    writer.WriteU64(blob.offset);
    writer.WriteU64(blob.size);
}

base::Result<BlobRef, PackageError> ReadBlobRef(base::BinaryReader& reader, SectionKind owner)
{
    auto section = reader.ReadU32();
    auto reserved = reader.ReadU32();
    auto offset = reader.ReadU64();
    auto size = reader.ReadU64();
    if (!section || !reserved || !offset || !size)
        return base::Result<BlobRef, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "blob reference is truncated", owner));
    BlobRef blob{static_cast<SectionKind>(section.Value()), reserved.Value(), offset.Value(), size.Value()};
    if (blob.reserved != 0)
        return base::Result<BlobRef, PackageError>::Failure(Error(PackageErrorCode::InvalidBlobReference, "blob reference reserved field must be zero", owner));
    return base::Result<BlobRef, PackageError>::Success(blob);
}

void WriteRange(base::BinaryWriter& writer, IndexRange range)
{
    writer.WriteU32(range.first);
    writer.WriteU32(range.count);
}

base::Result<IndexRange, PackageError> ReadRange(base::BinaryReader& reader, SectionKind section)
{
    auto first = reader.ReadU32();
    auto count = reader.ReadU32();
    if (!first || !count)
        return base::Result<IndexRange, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "index range is truncated", section));
    return base::Result<IndexRange, PackageError>::Success({first.Value(), count.Value()});
}

template<class Id>
void WriteId(base::BinaryWriter& writer, Id id)
{
    writer.WriteU32(id.value);
}

template<class T, class EncodeOne>
std::vector<std::byte> EncodeTable(std::span<const T> records, std::uint32_t stride, EncodeOne encodeOne)
{
    base::BinaryWriter writer;
    for (const auto& record : records)
    {
        const auto start = writer.Size();
        encodeOne(writer, record);
        if (writer.Size() - start > stride) throw std::runtime_error("record encoder exceeded declared stride");
        writer.WriteZeroes(stride - (writer.Size() - start));
    }
    return std::move(writer).Take();
}

PackageSectionInput MakeSection(
    SectionKind kind,
    std::vector<std::byte> bytes,
    std::uint32_t count,
    std::uint32_t stride,
    std::uint32_t alignment = 8)
{
    PackageSectionInput section;
    section.kind = kind;
    section.schemaVersion = 1;
    section.flags = Flags(SectionFlags::Required, SectionFlags::ExecutionAffecting);
    section.alignment = alignment;
    section.elementCount = count;
    section.elementStride = stride;
    section.bytes = std::move(bytes);
    return section;
}

PackageSectionInput MakeBlobSection(SectionKind kind, std::vector<std::byte> bytes, bool required = true)
{
    PackageSectionInput section;
    section.kind = kind;
    section.schemaVersion = 1;
    section.flags = static_cast<std::uint32_t>(SectionFlags::ExecutionAffecting) |
        (required ? static_cast<std::uint32_t>(SectionFlags::Required) : 0u);
    section.alignment = 16;
    section.bytes = std::move(bytes);
    return section;
}

std::vector<std::byte> EncodeManifest(const PackageManifest& value)
{
    base::BinaryWriter writer;
    writer.WriteU32(value.resourceCount);
    writer.WriteU32(value.allocationCount);
    writer.WriteU32(value.viewCount);
    writer.WriteU32(value.shaderCount);
    writer.WriteU32(value.programCount);
    writer.WriteU32(value.bindingLayoutCount);
    writer.WriteU32(value.executableCount);
    writer.WriteU32(value.rasterCommandCount);
    writer.WriteU32(value.computeExecutableCount);
    writer.WriteU32(value.computeCommandCount);
    writer.WriteU32(value.vertexElementCount);
    writer.WriteU32(value.attachmentOperationCount);
    writer.WriteU32(value.dynamicSlotCount);
    writer.WriteU32(value.externalSlotCount);
    writer.WriteU32(value.surfaceSlotCount);
    writer.WriteU32(value.loadOperationStream);
    writer.WriteU32(value.frameOperationStream);
    writer.WriteU32(value.flags);
    return std::move(writer).Take();
}

std::vector<std::byte> EncodeProfile(const TargetProfile& profile)
{
    base::BinaryWriter writer;
    writer.WriteU32(profile.minimumFeatureLevel);
    writer.WriteU16(profile.shaderModelMajor);
    writer.WriteU16(profile.shaderModelMinor);
    writer.WriteU16(profile.rootSignatureMajor);
    writer.WriteU16(profile.rootSignatureMinor);
    writer.WriteU32(static_cast<std::uint32_t>(profile.barrierModel));
    writer.WriteU16(static_cast<std::uint16_t>(profile.shaderBinaryFormat));
    writer.WriteU16(0);
    writer.WriteU32(profile.framesInFlight);
    writer.WriteU32(profile.directQueueCount);
    writer.WriteU32(profile.computeQueueCount);
    writer.WriteU32(profile.copyQueueCount);
    writer.WriteU32(profile.surfaceImageCount);
    writer.WriteU32(profile.rtvDescriptorCount);
    writer.WriteU32(profile.dsvDescriptorCount);
    writer.WriteU32(profile.shaderDescriptorCount);
    writer.WriteU32(profile.samplerDescriptorCount);
    writer.WriteU64(profile.requiredFeatureBits0);
    writer.WriteU64(profile.requiredFeatureBits1);
    writer.WriteZeroes(8);
    return std::move(writer).Take();
}

template<class Records>
bool Dense(const Records& records)
{
    for (std::size_t i = 0; i < records.size(); ++i)
        if (records[i].id.value != i) return false;
    return true;
}

bool DenseIds(const D3D12PackageDescription& description)
{
    if (!Dense(description.resources) || !Dense(description.allocations) || !Dense(description.views) ||
        !Dense(description.shaders) || !Dense(description.programs) || !Dense(description.bindingLayouts) ||
        !Dense(description.rootParameters) || !Dense(description.executables) || !Dense(description.computeExecutables) ||
        !Dense(description.computeCommands) || !Dense(description.attachmentOperations) || !Dense(description.rasterCommands) || !Dense(description.dynamicSlots) || !Dense(description.externalSlots) || !Dense(description.surfaceSlots))
        return false;
    for (std::size_t i = 0; i < description.vertexElements.size(); ++i)
        if (description.vertexElements[i].id != i) return false;
    return true;
}

base::Result<const SectionView*, PackageError> RequireSection(
    const FrozenExecutablePackage& package,
    SectionKind kind,
    std::uint32_t stride = 0)
{
    const auto* section = package.FindSection(kind);
    if (section == nullptr)
        return base::Result<const SectionView*, PackageError>::Failure(Error(PackageErrorCode::MissingRequiredSection, "required D3D12 schema section is missing", kind));
    if (stride != 0 && section->descriptor.elementStride != stride)
        return base::Result<const SectionView*, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "D3D12 schema record stride mismatch", kind));
    return base::Result<const SectionView*, PackageError>::Success(section);
}

template<class T, class DecodeOne>
base::Result<std::vector<T>, PackageError> DecodeTable(const SectionView& section, std::uint32_t stride, DecodeOne decodeOne)
{
    if (section.descriptor.elementStride != stride ||
        section.bytes.size() != static_cast<std::size_t>(section.descriptor.elementCount) * stride)
        return base::Result<std::vector<T>, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "table dimensions do not match schema", section.descriptor.sectionKind));

    std::vector<T> output;
    output.reserve(section.descriptor.elementCount);
    for (std::uint32_t i = 0; i < section.descriptor.elementCount; ++i)
    {
        base::BinaryReader reader(section.bytes.subspan(static_cast<std::size_t>(i) * stride, stride));
        auto value = decodeOne(reader, section.descriptor.sectionKind);
        if (!value)
        {
            auto error = value.Error();
            error.recordIndex = i;
            return base::Result<std::vector<T>, PackageError>::Failure(std::move(error));
        }
        output.push_back(std::move(value).Value());
    }
    return base::Result<std::vector<T>, PackageError>::Success(std::move(output));
}

base::Result<PackageManifest, PackageError> DecodeManifestRecord(base::BinaryReader& reader, SectionKind section)
{
    PackageManifest manifest;
    std::uint32_t* fields[] = {
        &manifest.resourceCount, &manifest.allocationCount, &manifest.viewCount, &manifest.shaderCount,
        &manifest.programCount, &manifest.bindingLayoutCount, &manifest.executableCount, &manifest.rasterCommandCount,
        &manifest.computeExecutableCount, &manifest.computeCommandCount, &manifest.vertexElementCount, &manifest.attachmentOperationCount, &manifest.dynamicSlotCount, &manifest.externalSlotCount,
        &manifest.surfaceSlotCount, &manifest.loadOperationStream, &manifest.frameOperationStream, &manifest.flags
    };
    for (auto* field : fields)
    {
        auto value = reader.ReadU32();
        if (!value) return base::Result<PackageManifest, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "manifest is truncated", section));
        *field = value.Value();
    }
    return base::Result<PackageManifest, PackageError>::Success(manifest);
}

base::Result<TargetProfile, PackageError> DecodeProfileRecord(base::BinaryReader& reader, SectionKind section)
{
    TargetProfile profile;
    auto featureLevel = reader.ReadU32();
    auto shaderMajor = reader.ReadU16();
    auto shaderMinor = reader.ReadU16();
    auto rootMajor = reader.ReadU16();
    auto rootMinor = reader.ReadU16();
    auto barrier = reader.ReadU32();
    auto binaryFormat = reader.ReadU16();
    auto reserved = reader.ReadU16();
    auto frames = reader.ReadU32();
    auto direct = reader.ReadU32();
    auto compute = reader.ReadU32();
    auto copy = reader.ReadU32();
    auto surface = reader.ReadU32();
    auto rtv = reader.ReadU32();
    auto dsv = reader.ReadU32();
    auto shaderDescriptors = reader.ReadU32();
    auto samplers = reader.ReadU32();
    auto bits0 = reader.ReadU64();
    auto bits1 = reader.ReadU64();
    auto tail = reader.ReadBytes(8);
    if (!featureLevel || !shaderMajor || !shaderMinor || !rootMajor || !rootMinor || !barrier || !binaryFormat ||
        !reserved || !frames || !direct || !compute || !copy || !surface || !rtv || !dsv || !shaderDescriptors ||
        !samplers || !bits0 || !bits1 || !tail)
        return base::Result<TargetProfile, PackageError>::Failure(Error(PackageErrorCode::InvalidTargetProfile, "target profile is truncated", section));
    if (reserved.Value() != 0 || std::any_of(tail.Value().begin(), tail.Value().end(), [](std::byte value) { return value != std::byte{0}; }))
        return base::Result<TargetProfile, PackageError>::Failure(Error(PackageErrorCode::InvalidTargetProfile, "target profile reserved bytes must be zero", section));

    profile.minimumFeatureLevel = featureLevel.Value();
    profile.shaderModelMajor = shaderMajor.Value();
    profile.shaderModelMinor = shaderMinor.Value();
    profile.rootSignatureMajor = rootMajor.Value();
    profile.rootSignatureMinor = rootMinor.Value();
    profile.barrierModel = static_cast<BarrierModel>(barrier.Value());
    profile.shaderBinaryFormat = static_cast<ShaderBinaryFormat>(binaryFormat.Value());
    profile.framesInFlight = frames.Value();
    profile.directQueueCount = direct.Value();
    profile.computeQueueCount = compute.Value();
    profile.copyQueueCount = copy.Value();
    profile.surfaceImageCount = surface.Value();
    profile.rtvDescriptorCount = rtv.Value();
    profile.dsvDescriptorCount = dsv.Value();
    profile.shaderDescriptorCount = shaderDescriptors.Value();
    profile.samplerDescriptorCount = samplers.Value();
    profile.requiredFeatureBits0 = bits0.Value();
    profile.requiredFeatureBits1 = bits1.Value();

    const auto queueCount = static_cast<std::uint64_t>(profile.directQueueCount) +
                            profile.computeQueueCount + profile.copyQueueCount;
    if (profile.framesInFlight == 0 || profile.directQueueCount == 0 || queueCount > 64 ||
        profile.barrierModel != BarrierModel::Legacy ||
        profile.shaderBinaryFormat != ShaderBinaryFormat::Dxbc ||
        profile.shaderModelMajor != 5 || profile.shaderModelMinor > 1 ||
        profile.rootSignatureMajor != 1 || profile.rootSignatureMinor > 1)
        return base::Result<TargetProfile, PackageError>::Failure(Error(PackageErrorCode::InvalidTargetProfile, "target profile is outside the supported D3D12 v16 capability", section));
    return base::Result<TargetProfile, PackageError>::Success(profile);
}

base::Result<ResourceArtifact, PackageError> DecodeResource(base::BinaryReader& reader, SectionKind section)
{
    ResourceArtifact value;
    auto id = reader.ReadU32();
    auto kind = reader.ReadU16();
    auto origin = reader.ReadU16();
    auto rebuild = reader.ReadU16();
    auto extent = reader.ReadU16();
    auto flags = reader.ReadU32();
    auto physical = reader.ReadU32();
    auto allocation = reader.ReadU32();
    auto format = reader.ReadU32();
    auto usage = reader.ReadU32();
    auto state = ReadState(reader, section);
    auto size = reader.ReadU64();
    auto width = reader.ReadU32();
    auto height = reader.ReadU32();
    auto depth = reader.ReadU16();
    auto mips = reader.ReadU16();
    auto samples = reader.ReadU16();
    auto planes = reader.ReadU16();
    auto dataOffset = reader.ReadU64();
    auto dataSize = reader.ReadU64();
    auto firstView = reader.ReadU32();
    auto viewCount = reader.ReadU32();
    auto reserved = reader.ReadBytes(8);
    if (!id || !kind || !origin || !rebuild || !extent || !flags || !physical || !allocation || !format || !usage ||
        !state || !size || !width || !height || !depth || !mips || !samples || !planes || !dataOffset || !dataSize ||
        !firstView || !viewCount || !reserved)
        return base::Result<ResourceArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "resource record is truncated", section));
    if (std::any_of(reserved.Value().begin(), reserved.Value().end(), [](std::byte byte) { return byte != std::byte{0}; }))
        return base::Result<ResourceArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "resource reserved bytes must be zero", section));

    value.id = {id.Value()};
    value.resourceKind = static_cast<ResourceKind>(kind.Value());
    value.origin = static_cast<ResourceOrigin>(origin.Value());
    value.rebuildPolicy = static_cast<RebuildPolicy>(rebuild.Value());
    value.extentMode = static_cast<ExtentMode>(extent.Value());
    value.flags = flags.Value();
    value.physicalInstanceCount = physical.Value();
    value.allocation = {allocation.Value()};
    value.format = static_cast<Format>(format.Value());
    value.usageFlags = usage.Value();
    value.initialState = state.Value();
    value.sizeBytes = size.Value();
    value.width = width.Value();
    value.height = height.Value();
    value.depthOrArraySize = depth.Value();
    value.mipLevels = mips.Value();
    value.sampleCount = samples.Value();
    value.planeCount = planes.Value();
    value.initialDataOffset = dataOffset.Value();
    value.initialDataSize = dataSize.Value();
    value.firstView = firstView.Value();
    value.viewCount = viewCount.Value();
    return base::Result<ResourceArtifact, PackageError>::Success(value);
}

base::Result<AllocationArtifact, PackageError> DecodeAllocation(base::BinaryReader& reader, SectionKind section)
{
    AllocationArtifact value;
    auto id = reader.ReadU32();
    auto kind = reader.ReadU16();
    auto heap = reader.ReadU16();
    auto flags = reader.ReadU32();
    auto physical = reader.ReadU32();
    auto size = reader.ReadU64();
    auto alignment = reader.ReadU64();
    auto alias = reader.ReadU32();
    auto reserved = reader.ReadU32();
    if (!id || !kind || !heap || !flags || !physical || !size || !alignment || !alias || !reserved)
        return base::Result<AllocationArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "allocation record is truncated", section));
    if (reserved.Value() != 0)
        return base::Result<AllocationArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "allocation reserved field must be zero", section));
    value.id = {id.Value()};
    value.kind = static_cast<AllocationKind>(kind.Value());
    value.heapClass = static_cast<HeapClass>(heap.Value());
    value.flags = flags.Value();
    value.physicalInstanceCount = physical.Value();
    value.sizeBytes = size.Value();
    value.alignment = alignment.Value();
    value.aliasGroup = alias.Value();
    return base::Result<AllocationArtifact, PackageError>::Success(value);
}

base::Result<ResourceViewArtifact, PackageError> DecodeView(base::BinaryReader& reader, SectionKind section)
{
    ResourceViewArtifact value;
    auto id = reader.ReadU32();
    auto resource = reader.ReadU32();
    auto viewClass = reader.ReadU16();
    auto reserved16 = reader.ReadU16();
    auto format = reader.ReadU32();
    auto flags = reader.ReadU32();
    auto reserved32 = reader.ReadU32();
    auto offset = reader.ReadU64();
    auto size = reader.ReadU64();
    auto stride = reader.ReadU32();
    auto firstMip = reader.ReadU16();
    auto mipCount = reader.ReadU16();
    auto firstArray = reader.ReadU16();
    auto arrayCount = reader.ReadU16();
    auto firstPlane = reader.ReadU16();
    auto planeCount = reader.ReadU16();
    auto heap = reader.ReadU32();
    auto descriptorIndex = reader.ReadU32();
    auto instanceStride = reader.ReadU32();
    auto tail = reader.ReadU32();
    if (!id || !resource || !viewClass || !reserved16 || !format || !flags || !reserved32 || !offset || !size ||
        !stride || !firstMip || !mipCount || !firstArray || !arrayCount || !firstPlane || !planeCount || !heap ||
        !descriptorIndex || !instanceStride || !tail)
        return base::Result<ResourceViewArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "view record is truncated", section));
    if (reserved16.Value() != 0 || reserved32.Value() != 0 || tail.Value() != 0)
        return base::Result<ResourceViewArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "view reserved field must be zero", section));

    value.id = {id.Value()};
    value.resource = {resource.Value()};
    value.viewClass = static_cast<ViewClass>(viewClass.Value());
    value.format = static_cast<Format>(format.Value());
    value.flags = flags.Value();
    value.byteOffset = offset.Value();
    value.byteSize = size.Value();
    value.strideBytes = stride.Value();
    value.firstMip = firstMip.Value();
    value.mipCount = mipCount.Value();
    value.firstArrayLayer = firstArray.Value();
    value.arrayLayerCount = arrayCount.Value();
    value.firstPlane = firstPlane.Value();
    value.planeCount = planeCount.Value();
    value.descriptorHeapClass = heap.Value();
    value.descriptorIndex = descriptorIndex.Value();
    value.descriptorInstanceStride = instanceStride.Value();
    return base::Result<ResourceViewArtifact, PackageError>::Success(value);
}

base::Result<ShaderArtifact, PackageError> DecodeShader(base::BinaryReader& reader, SectionKind section)
{
    ShaderArtifact value;
    auto id = reader.ReadU32();
    auto stage = reader.ReadU16();
    auto format = reader.ReadU16();
    auto major = reader.ReadU16();
    auto minor = reader.ReadU16();
    auto flags = reader.ReadU32();
    auto blob = ReadBlobRef(reader, section);
    auto digest = reader.ReadBytes(32);
    if (!id || !stage || !format || !major || !minor || !flags || !blob || !digest)
        return base::Result<ShaderArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "shader record is truncated", section));
    value.id = {id.Value()};
    value.stage = static_cast<ShaderStage>(stage.Value());
    value.format = static_cast<ShaderBinaryFormat>(format.Value());
    value.shaderModelMajor = major.Value();
    value.shaderModelMinor = minor.Value();
    value.flags = flags.Value();
    value.bytecode = blob.Value();
    std::copy(digest.Value().begin(), digest.Value().end(), value.bytecodeDigest.begin());
    return base::Result<ShaderArtifact, PackageError>::Success(value);
}

base::Result<ProgramArtifact, PackageError> DecodeProgram(base::BinaryReader& reader, SectionKind section)
{
    ProgramArtifact value;
    auto id = reader.ReadU32();
    auto kind = reader.ReadU16();
    auto flags = reader.ReadU16();
    auto vertex = reader.ReadU32();
    auto pixel = reader.ReadU32();
    auto compute = reader.ReadU32();
    auto layout = reader.ReadU32();
    auto digest = reader.ReadBytes(32);
    if (!id || !kind || !flags || !vertex || !pixel || !compute || !layout || !digest)
        return base::Result<ProgramArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "program record is truncated", section));
    value.id = {id.Value()};
    value.kind = static_cast<ProgramKind>(kind.Value());
    value.flags = flags.Value();
    value.vertexShader = {vertex.Value()};
    value.pixelShader = {pixel.Value()};
    value.computeShader = {compute.Value()};
    value.bindingLayout = {layout.Value()};
    std::copy(digest.Value().begin(), digest.Value().end(), value.interfaceDigest.begin());
    return base::Result<ProgramArtifact, PackageError>::Success(value);
}

base::Result<BindingLayoutArtifact, PackageError> DecodeBinding(base::BinaryReader& reader, SectionKind section)
{
    BindingLayoutArtifact value;
    auto id = reader.ReadU32();
    auto major = reader.ReadU16();
    auto minor = reader.ReadU16();
    auto flags = reader.ReadU32();
    auto parameters = ReadRange(reader, section);
    auto descriptors = ReadRange(reader, section);
    auto samplers = ReadRange(reader, section);
    auto blob = ReadBlobRef(reader, section);
    auto digest = reader.ReadBytes(32);
    auto reserved = reader.ReadU32();
    if (!id || !major || !minor || !flags || !parameters || !descriptors || !samplers || !blob || !digest || !reserved)
        return base::Result<BindingLayoutArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "binding layout record is truncated", section));
    if (reserved.Value() != 0)
        return base::Result<BindingLayoutArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "binding layout reserved field must be zero", section));
    value.id = {id.Value()};
    value.rootSignatureMajor = major.Value();
    value.rootSignatureMinor = minor.Value();
    value.flags = flags.Value();
    value.parameterRange = parameters.Value();
    value.descriptorRange = descriptors.Value();
    value.staticSamplerRange = samplers.Value();
    value.serializedRootSignature = blob.Value();
    std::copy(digest.Value().begin(), digest.Value().end(), value.layoutDigest.begin());
    return base::Result<BindingLayoutArtifact, PackageError>::Success(value);
}

base::Result<RootParameterArtifact, PackageError> DecodeRootParameter(base::BinaryReader& reader, SectionKind section)
{
    RootParameterArtifact value;
    auto id = reader.ReadU32();
    auto kind = reader.ReadU16();
    auto visibility = reader.ReadU16();
    auto rootIndex = reader.ReadU32();
    auto shaderRegister = reader.ReadU32();
    auto registerSpace = reader.ReadU32();
    auto dynamicSlot = reader.ReadU32();
    auto staticView = reader.ReadU32();
    auto flags = reader.ReadU32();
    auto reserved = reader.ReadU32();
    auto tail = reader.ReadU32();
    if (!id || !kind || !visibility || !rootIndex || !shaderRegister || !registerSpace || !dynamicSlot || !staticView || !flags || !reserved || !tail)
        return base::Result<RootParameterArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "root parameter record is truncated", section));
    if (reserved.Value() != 0 || tail.Value() != 0)
        return base::Result<RootParameterArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "root parameter reserved field must be zero", section));
    value.id = {id.Value()};
    value.kind = static_cast<RootParameterKind>(kind.Value());
    value.visibility = static_cast<ShaderVisibility>(visibility.Value());
    value.rootParameterIndex = rootIndex.Value();
    value.shaderRegister = shaderRegister.Value();
    value.registerSpace = registerSpace.Value();
    value.dynamicSlot = {dynamicSlot.Value()};
    value.staticView = {staticView.Value()};
    value.flags = flags.Value();
    return base::Result<RootParameterArtifact, PackageError>::Success(value);
}

base::Result<VertexElementArtifact, PackageError> DecodeVertexElement(base::BinaryReader& reader, SectionKind section)
{
    VertexElementArtifact value;
    auto id = reader.ReadU32();
    auto meaning = reader.ReadU16();
    auto semanticIndex = reader.ReadU16();
    auto format = reader.ReadU32();
    auto inputSlot = reader.ReadU32();
    auto offset = reader.ReadU32();
    auto step = reader.ReadU32();
    auto flags = reader.ReadU32();
    auto reserved = reader.ReadU32();
    if (!id || !meaning || !semanticIndex || !format || !inputSlot || !offset || !step || !flags || !reserved)
        return base::Result<VertexElementArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "vertex element record is truncated", section));
    if (reserved.Value() != 0)
        return base::Result<VertexElementArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "vertex element reserved field must be zero", section));
    value.id = id.Value();
    value.meaning = static_cast<VertexMeaning>(meaning.Value());
    value.semanticIndex = semanticIndex.Value();
    value.format = static_cast<Format>(format.Value());
    value.inputSlot = inputSlot.Value();
    value.alignedByteOffset = offset.Value();
    value.instanceStepRate = step.Value();
    value.flags = flags.Value();
    return base::Result<VertexElementArtifact, PackageError>::Success(value);
}

base::Result<RasterExecutableArtifact, PackageError> DecodeExecutable(base::BinaryReader& reader, SectionKind section)
{
    RasterExecutableArtifact value;
    auto id = reader.ReadU32();
    auto program = reader.ReadU32();
    auto layout = reader.ReadU32();
    auto reserved0 = reader.ReadU32();
    auto vertexRange = ReadRange(reader, section);
    auto colorRange = ReadRange(reader, section);
    auto colorFormat = reader.ReadU32();
    auto depthFormat = reader.ReadU32();
    auto topology = reader.ReadU32();
    auto topologyType = reader.ReadU32();
    auto rasterState = reader.ReadU32();
    auto blendState = reader.ReadU32();
    auto depthState = reader.ReadU32();
    auto samples = reader.ReadU32();
    auto quality = reader.ReadU32();
    auto reserved1 = reader.ReadU32();
    auto digest = reader.ReadBytes(32);
    if (!id || !program || !layout || !reserved0 || !vertexRange || !colorRange || !colorFormat || !depthFormat ||
        !topology || !topologyType || !rasterState || !blendState || !depthState || !samples || !quality || !reserved1 || !digest)
        return base::Result<RasterExecutableArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "executable record is truncated", section));
    if (reserved0.Value() != 0 || reserved1.Value() != 0)
        return base::Result<RasterExecutableArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "executable reserved field must be zero", section));
    value.id = {id.Value()};
    value.program = {program.Value()};
    value.bindingLayout = {layout.Value()};
    value.vertexElementRange = vertexRange.Value();
    value.colorFormatRange = colorRange.Value();
    value.colorFormat = static_cast<Format>(colorFormat.Value());
    value.depthFormat = static_cast<Format>(depthFormat.Value());
    value.primitiveTopology = static_cast<PrimitiveTopology>(topology.Value());
    value.primitiveTopologyType = static_cast<PrimitiveTopologyType>(topologyType.Value());
    value.rasterStateId = rasterState.Value();
    value.blendStateId = blendState.Value();
    value.depthStateId = depthState.Value();
    value.sampleCount = samples.Value();
    value.sampleQuality = quality.Value();
    std::copy(digest.Value().begin(), digest.Value().end(), value.specializationDigest.begin());
    return base::Result<RasterExecutableArtifact, PackageError>::Success(value);
}

base::Result<ComputeExecutableArtifact, PackageError> DecodeComputeExecutable(base::BinaryReader& reader, SectionKind section)
{
    ComputeExecutableArtifact value;
    auto id = reader.ReadU32();
    auto program = reader.ReadU32();
    auto layout = reader.ReadU32();
    auto flags = reader.ReadU32();
    auto digest = reader.ReadBytes(32);
    if (!id || !program || !layout || !flags || !digest)
        return base::Result<ComputeExecutableArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "compute executable record is truncated", section));
    value.id = {id.Value()};
    value.program = {program.Value()};
    value.bindingLayout = {layout.Value()};
    value.flags = flags.Value();
    std::copy(digest.Value().begin(), digest.Value().end(), value.specializationDigest.begin());
    return base::Result<ComputeExecutableArtifact, PackageError>::Success(value);
}

base::Result<ComputeCommandArtifact, PackageError> DecodeComputeCommand(base::BinaryReader& reader, SectionKind section)
{
    ComputeCommandArtifact value;
    auto id = reader.ReadU32();
    auto executable = reader.ReadU32();
    auto x = reader.ReadU32();
    auto y = reader.ReadU32();
    auto z = reader.ReadU32();
    auto flags = reader.ReadU32();
    if (!id || !executable || !x || !y || !z || !flags)
        return base::Result<ComputeCommandArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "compute command record is truncated", section));
    value.id = {id.Value()};
    value.executable = {executable.Value()};
    value.threadGroupCountX = x.Value();
    value.threadGroupCountY = y.Value();
    value.threadGroupCountZ = z.Value();
    value.flags = flags.Value();
    return base::Result<ComputeCommandArtifact, PackageError>::Success(value);
}

base::Result<AttachmentOperationArtifact, PackageError> DecodeAttachment(base::BinaryReader& reader, SectionKind section)
{
    AttachmentOperationArtifact value;
    auto id = reader.ReadU32();
    auto colorLoad = reader.ReadU16();
    auto colorStore = reader.ReadU16();
    if (!id || !colorLoad || !colorStore)
        return base::Result<AttachmentOperationArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "attachment record is truncated", section));
    value.id = {id.Value()};
    value.colorLoad = static_cast<AttachmentLoadOp>(colorLoad.Value());
    value.colorStore = static_cast<AttachmentStoreOp>(colorStore.Value());
    for (float& component : value.clearColor)
    {
        auto bits = reader.ReadU32();
        if (!bits) return base::Result<AttachmentOperationArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "attachment clear color is truncated", section));
        component = std::bit_cast<float>(bits.Value());
        if (!std::isfinite(component))
            return base::Result<AttachmentOperationArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "attachment clear color must be finite", section));
    }
    auto depthLoad = reader.ReadU16();
    auto depthStore = reader.ReadU16();
    auto clearDepthBits = reader.ReadU32();
    auto clearStencil = reader.ReadU32();
    auto reserved0 = reader.ReadU32();
    auto reserved1 = reader.ReadU32();
    auto reserved2 = reader.ReadU32();
    if (!depthLoad || !depthStore || !clearDepthBits || !clearStencil || !reserved0 || !reserved1 || !reserved2)
        return base::Result<AttachmentOperationArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "depth attachment fields are truncated", section));
    value.depthLoad = static_cast<AttachmentLoadOp>(depthLoad.Value());
    value.depthStore = static_cast<AttachmentStoreOp>(depthStore.Value());
    value.clearDepth = std::bit_cast<float>(clearDepthBits.Value());
    value.clearStencil = clearStencil.Value();
    if (!std::isfinite(value.clearDepth) || value.clearDepth < 0.0f || value.clearDepth > 1.0f || value.clearStencil > 255u)
        return base::Result<AttachmentOperationArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "depth clear value is invalid", section));
    if (reserved0.Value() != 0 || reserved1.Value() != 0 || reserved2.Value() != 0)
        return base::Result<AttachmentOperationArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "attachment reserved fields must be zero", section));
    return base::Result<AttachmentOperationArtifact, PackageError>::Success(value);
}

base::Result<RasterCommandArtifact, PackageError> DecodeRasterCommand(base::BinaryReader& reader, SectionKind section)
{
    RasterCommandArtifact value;
    auto id = reader.ReadU32();
    auto executable = reader.ReadU32();
    auto vertexViews = ReadRange(reader, section);
    auto indexView = reader.ReadU32();
    auto colorAttachments = ReadRange(reader, section);
    auto depthAttachment = reader.ReadU32();
    auto vertexCount = reader.ReadU32();
    auto instanceCount = reader.ReadU32();
    auto firstVertex = reader.ReadU32();
    auto firstInstance = reader.ReadU32();
    auto indexCount = reader.ReadU32();
    auto firstIndex = reader.ReadU32();
    auto baseVertex = reader.ReadI32();
    auto viewport = reader.ReadU32();
    auto scissor = reader.ReadU32();
    auto attachment = reader.ReadU32();
    auto reserved = reader.ReadBytes(4);
    if (!id || !executable || !vertexViews || !indexView || !colorAttachments || !depthAttachment || !vertexCount ||
        !instanceCount || !firstVertex || !firstInstance || !indexCount || !firstIndex || !baseVertex || !viewport ||
        !scissor || !attachment || !reserved)
        return base::Result<RasterCommandArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "raster command is truncated", section));
    if (std::any_of(reserved.Value().begin(), reserved.Value().end(), [](std::byte byte) { return byte != std::byte{0}; }))
        return base::Result<RasterCommandArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidEnumValue, "raster command reserved bytes must be zero", section));
    value.id = {id.Value()};
    value.executable = {executable.Value()};
    value.vertexViewRange = vertexViews.Value();
    value.indexView = {indexView.Value()};
    value.colorAttachmentRange = colorAttachments.Value();
    value.depthAttachment = {depthAttachment.Value()};
    value.vertexCount = vertexCount.Value();
    value.instanceCount = instanceCount.Value();
    value.firstVertex = firstVertex.Value();
    value.firstInstance = firstInstance.Value();
    value.indexCount = indexCount.Value();
    value.firstIndex = firstIndex.Value();
    value.baseVertex = baseVertex.Value();
    value.viewportId = viewport.Value();
    value.scissorId = scissor.Value();
    value.attachmentOperation = {attachment.Value()};
    return base::Result<RasterCommandArtifact, PackageError>::Success(value);
}

base::Result<DynamicDataSlotArtifact, PackageError> DecodeDynamicSlot(base::BinaryReader& reader, SectionKind section)
{
    DynamicDataSlotArtifact value;
    auto id = reader.ReadU32();
    auto resource = reader.ReadU32();
    auto offset = reader.ReadU64();
    auto bytes = reader.ReadU64();
    auto alignment = reader.ReadU32();
    auto flags = reader.ReadU32();
    if (!id || !resource || !offset || !bytes || !alignment || !flags)
        return base::Result<DynamicDataSlotArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "dynamic slot record is truncated", section));
    value.id = {id.Value()};
    value.destinationResource = {resource.Value()};
    value.destinationOffset = offset.Value();
    value.requiredBytes = bytes.Value();
    value.requiredAlignment = alignment.Value();
    value.flags = flags.Value();
    return base::Result<DynamicDataSlotArtifact, PackageError>::Success(value);
}

base::Result<ExternalResourceSlotArtifact, PackageError> DecodeExternalSlot(base::BinaryReader& reader, SectionKind section)
{
    ExternalResourceSlotArtifact value;
    auto id = reader.ReadU32(); auto resource = reader.ReadU32(); auto kind = reader.ReadU32(); auto format = reader.ReadU32();
    auto minimumBytes = reader.ReadU64(); auto incoming = ReadState(reader, section); auto outgoing = ReadState(reader, section);
    auto synchronization = reader.ReadU32(); auto flags = reader.ReadU32();
    if (!id || !resource || !kind || !format || !minimumBytes || !incoming || !outgoing || !synchronization || !flags)
        return base::Result<ExternalResourceSlotArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "external slot record is truncated", section));
    value.id={id.Value()}; value.resource={resource.Value()}; value.requiredKind=static_cast<ResourceKind>(kind.Value());
    value.requiredFormat=static_cast<Format>(format.Value()); value.minimumBytes=minimumBytes.Value();
    value.requiredIncomingState=incoming.Value(); value.guaranteedOutgoingState=outgoing.Value();
    value.synchronizationContract=static_cast<ExternalSynchronizationContract>(synchronization.Value()); value.flags=flags.Value();
    return base::Result<ExternalResourceSlotArtifact, PackageError>::Success(value);
}

base::Result<SurfaceSlotArtifact, PackageError> DecodeSurface(base::BinaryReader& reader, SectionKind section)
{
    SurfaceSlotArtifact value;
    auto id = reader.ReadU32();
    auto resource = reader.ReadU32();
    auto format = reader.ReadU32();
    auto acquired = ReadState(reader, section);
    auto presented = ReadState(reader, section);
    auto flags = reader.ReadU32();
    if (!id || !resource || !format || !acquired || !presented || !flags)
        return base::Result<SurfaceSlotArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidRecordStride, "surface slot is truncated", section));
    value.id = {id.Value()};
    value.imageResource = {resource.Value()};
    value.requiredFormat = static_cast<Format>(format.Value());
    value.acquiredState = acquired.Value();
    value.presentedState = presented.Value();
    value.flags = flags.Value();
    return base::Result<SurfaceSlotArtifact, PackageError>::Success(value);
}

base::Result<OperationStreamArtifact, PackageError> DecodeStream(base::BinaryReader& reader, SectionKind section)
{
    OperationStreamArtifact value;
    auto kind = reader.ReadU32();
    auto first = reader.ReadU32();
    auto count = reader.ReadU32();
    auto flags = reader.ReadU32();
    if (!kind || !first || !count || !flags)
        return base::Result<OperationStreamArtifact, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "operation stream is truncated", section));
    value.kind = static_cast<OperationStreamKind>(kind.Value());
    value.firstOperation = first.Value();
    value.operationCount = count.Value();
    value.flags = flags.Value();
    return base::Result<OperationStreamArtifact, PackageError>::Success(value);
}

struct RawOperation final
{
    D3D12OperationCode opcode{};
    std::uint16_t version = 1;
    std::uint16_t flags = 0;
    QueueId queue;
    std::uint32_t payloadBytes = 0;
    std::uint64_t payloadOffset = 0;
};

base::Result<RawOperation, PackageError> DecodeOperation(base::BinaryReader& reader, SectionKind section)
{
    RawOperation value;
    auto code = reader.ReadU32();
    auto version = reader.ReadU16();
    auto flags = reader.ReadU16();
    auto queue = reader.ReadU32();
    auto payloadBytes = reader.ReadU32();
    auto payloadOffset = reader.ReadU64();
    if (!code || !version || !flags || !queue || !payloadBytes || !payloadOffset)
        return base::Result<RawOperation, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "operation record is truncated", section));
    value.opcode = static_cast<D3D12OperationCode>(code.Value());
    value.version = version.Value();
    value.flags = flags.Value();
    value.queue = {queue.Value()};
    value.payloadBytes = payloadBytes.Value();
    value.payloadOffset = payloadOffset.Value();
    return base::Result<RawOperation, PackageError>::Success(value);
}

std::uint16_t ExpectedOperationVersion(D3D12OperationCode code)
{
    switch (code)
    {
    case D3D12OperationCode::SignalQueue:
    case D3D12OperationCode::WaitQueue:
    case D3D12OperationCode::WaitTemporal:
    case D3D12OperationCode::ReleaseExternal:
        return 2;
    default:
        return 1;
    }
}

bool KnownOpcode(D3D12OperationCode code)
{
    switch (code)
    {
    case D3D12OperationCode::CreateDescriptorHeaps:
    case D3D12OperationCode::CreateResource:
    case D3D12OperationCode::UploadBuffer:
    case D3D12OperationCode::CreateRootSignature:
    case D3D12OperationCode::CreateGraphicsPipeline:
    case D3D12OperationCode::CreateComputePipeline:
    case D3D12OperationCode::InitializeState:
    case D3D12OperationCode::VerifyBufferContents:
    case D3D12OperationCode::UploadTexture:
    case D3D12OperationCode::VerifyTextureContents:
    case D3D12OperationCode::AcquireSurfaceImage:
    case D3D12OperationCode::AcquireExternal:
    case D3D12OperationCode::WaitExternal:
    case D3D12OperationCode::ApplyDynamicData:
    case D3D12OperationCode::BeginQueueBatch:
    case D3D12OperationCode::Transition:
    case D3D12OperationCode::ActivateAlias:
    case D3D12OperationCode::ExecuteRaster:
    case D3D12OperationCode::ExecuteCompute:
    case D3D12OperationCode::ExecuteCopy:
    case D3D12OperationCode::EndQueueBatch:
    case D3D12OperationCode::SignalQueue:
    case D3D12OperationCode::WaitQueue:
    case D3D12OperationCode::WaitTemporal:
    case D3D12OperationCode::ReleaseExternal:
    case D3D12OperationCode::PresentSurface:
        return true;
    default:
        return false;
    }
}

base::Result<void, PackageError> ValidateOperations(const D3D12PackageView& view)
{
    const auto fail = [](const char* message)
    {
        return base::Result<void, PackageError>::Failure(
            Error(PackageErrorCode::InvalidOperationStream, message, SectionKind::OperationTable));
    };
    const auto queueCount = static_cast<std::uint64_t>(view.Profile().directQueueCount) +
                            view.Profile().computeQueueCount + view.Profile().copyQueueCount;
    const auto validQueue = [queueCount](QueueId queue) {
        return queue.IsValid() && queue.value < queueCount;
    };
    const auto isCopyQueue = [&](QueueId queue) {
        return validQueue(queue) &&
               queue.value >= view.Profile().directQueueCount + view.Profile().computeQueueCount;
    };
    const auto inInitialData = [&](std::uint64_t offset, std::uint64_t bytes) {
        return offset <= view.InitialData().size() && bytes <= view.InitialData().size() - offset;
    };
    const auto resourceValid = [&](ResourceId id) { return id.IsValid() && id.value < view.Resources().size(); };
    const auto viewValid = [&](ViewId id) { return id.IsValid() && id.value < view.Views().size(); };

    std::vector<bool> resourceCreated(view.Resources().size(), false);
    std::vector<bool> layoutCreated(view.BindingLayouts().size(), false);
    std::vector<bool> rasterCreated(view.Executables().size(), false);
    std::vector<bool> computeCreated(view.ComputeExecutables().size(), false);

    const auto validateStream = [&](std::span<const OperationView> operations, bool load)
        -> base::Result<void, PackageError>
    {
        std::vector<bool> batchOpen(static_cast<std::size_t>(queueCount), false);
        std::map<std::uint32_t, std::pair<QueueId, std::size_t>> signalPoints;
        std::uint32_t nextSignalPoint = 0;
        for (std::size_t index = 0; index < operations.size(); ++index)
        {
            const auto& operation = operations[index];
            if (operation.opcode != D3D12OperationCode::SignalQueue) continue;
            auto payload = DecodeSignalQueue(operation.payload);
            if (!payload || !payload.Value().signalPoint.IsValid() ||
                payload.Value().signalPoint.value != nextSignalPoint++ ||
                !validQueue(operation.queue) ||
                !signalPoints.emplace(payload.Value().signalPoint.value,
                    std::pair{operation.queue, index}).second)
                return fail("SignalQueue identities must be valid, dense, and unique within each stream");
        }

        for (std::size_t operationIndex = 0; operationIndex < operations.size(); ++operationIndex)
        {
            const auto& operation = operations[operationIndex];
            const auto expectedVersion = ExpectedOperationVersion(operation.opcode);
            if (!KnownOpcode(operation.opcode) || operation.operationVersion != expectedVersion || operation.flags != 0)
                return fail("operation envelope or operation version is invalid");

            const bool metadataOperation =
                operation.opcode == D3D12OperationCode::CreateDescriptorHeaps ||
                operation.opcode == D3D12OperationCode::CreateResource ||
                operation.opcode == D3D12OperationCode::CreateRootSignature ||
                operation.opcode == D3D12OperationCode::CreateGraphicsPipeline ||
                operation.opcode == D3D12OperationCode::CreateComputePipeline ||
                operation.opcode == D3D12OperationCode::AcquireSurfaceImage ||
                operation.opcode == D3D12OperationCode::AcquireExternal ||
                operation.opcode == D3D12OperationCode::ApplyDynamicData ||
                operation.opcode == D3D12OperationCode::ReleaseExternal ||
                operation.opcode == D3D12OperationCode::PresentSurface;
            if (metadataOperation ? operation.queue.IsValid() : !validQueue(operation.queue))
                return fail("operation queue is outside the target profile");

            switch (operation.opcode)
            {
            case D3D12OperationCode::CreateDescriptorHeaps:
                if (!load || !operation.payload.empty()) return fail("CreateDescriptorHeaps is invalid");
                break;
            case D3D12OperationCode::CreateResource:
            {
                if (!load) return fail("CreateResource is only valid in the load stream");
                auto payload = DecodeCreateResource(operation.payload);
                if (!payload || !resourceValid(payload.Value().resource) ||
                    view.Resources()[payload.Value().resource.value].origin != ResourceOrigin::PackageOwned ||
                    resourceCreated[payload.Value().resource.value])
                    return fail("CreateResource payload is invalid or duplicated");
                resourceCreated[payload.Value().resource.value] = true;
                break;
            }
            case D3D12OperationCode::UploadBuffer:
            {
                auto payload = DecodeUploadBuffer(operation.payload);
                if (!load || !payload || !resourceValid(payload.Value().resource) || payload.Value().bytes == 0 ||
                    !inInitialData(payload.Value().sourceOffset, payload.Value().bytes))
                    return fail("UploadBuffer payload is invalid");
                break;
            }
            case D3D12OperationCode::UploadTexture:
            {
                auto payload = DecodeUploadTexture(operation.payload);
                if (!load || !payload || !resourceValid(payload.Value().resource) ||
                    payload.Value().sourceSliceBytes == 0 ||
                    !inInitialData(payload.Value().sourceOffset, payload.Value().sourceSliceBytes))
                    return fail("UploadTexture payload is invalid");
                break;
            }
            case D3D12OperationCode::InitializeState:
            {
                auto payload = DecodeInitializeState(operation.payload);
                if (!load || !payload || !resourceValid(payload.Value().resource) ||
                    (isCopyQueue(operation.queue) &&
                     (!IsCopyQueueState(payload.Value().before) || !IsCopyQueueState(payload.Value().after))))
                    return fail("InitializeState payload is invalid");
                break;
            }
            case D3D12OperationCode::VerifyBufferContents:
            {
                auto payload = DecodeVerifyBufferContents(operation.payload);
                if (!load || !payload || !resourceValid(payload.Value().resource) || payload.Value().bytes == 0 ||
                    !inInitialData(payload.Value().expectedDataOffset, payload.Value().bytes))
                    return fail("VerifyBufferContents payload is invalid");
                break;
            }
            case D3D12OperationCode::VerifyTextureContents:
            {
                auto payload = DecodeVerifyTextureContents(operation.payload);
                const auto bytes = payload ? static_cast<std::uint64_t>(payload.Value().expectedRowBytes) * payload.Value().height : 0;
                if (!load || !payload || !resourceValid(payload.Value().resource) || bytes == 0 ||
                    !inInitialData(payload.Value().expectedDataOffset, bytes))
                    return fail("VerifyTextureContents payload is invalid");
                break;
            }
            case D3D12OperationCode::CreateRootSignature:
            {
                auto payload = DecodeCreateRootSignature(operation.payload);
                if (!load || !payload || !payload.Value().layout.IsValid() ||
                    payload.Value().layout.value >= view.BindingLayouts().size() ||
                    layoutCreated[payload.Value().layout.value])
                    return fail("CreateRootSignature payload is invalid or duplicated");
                layoutCreated[payload.Value().layout.value] = true;
                break;
            }
            case D3D12OperationCode::CreateGraphicsPipeline:
            {
                auto payload = DecodeCreateGraphicsPipeline(operation.payload);
                if (!load || !payload || !payload.Value().executable.IsValid() ||
                    payload.Value().executable.value >= view.Executables().size() ||
                    rasterCreated[payload.Value().executable.value])
                    return fail("CreateGraphicsPipeline payload is invalid or duplicated");
                rasterCreated[payload.Value().executable.value] = true;
                break;
            }
            case D3D12OperationCode::CreateComputePipeline:
            {
                auto payload = DecodeCreateComputePipeline(operation.payload);
                if (!load || !payload || !payload.Value().executable.IsValid() ||
                    payload.Value().executable.value >= view.ComputeExecutables().size() ||
                    computeCreated[payload.Value().executable.value])
                    return fail("CreateComputePipeline payload is invalid or duplicated");
                computeCreated[payload.Value().executable.value] = true;
                break;
            }
            case D3D12OperationCode::AcquireSurfaceImage:
            {
                auto payload = DecodeAcquireSurfaceImage(operation.payload);
                if (load || !payload || !payload.Value().slot.IsValid() ||
                    payload.Value().slot.value >= view.SurfaceSlots().size())
                    return fail("AcquireSurfaceImage payload is invalid");
                break;
            }
            case D3D12OperationCode::AcquireExternal:
            {
                auto payload = DecodeAcquireExternal(operation.payload);
                if (load || !payload || !payload.Value().slot.IsValid() ||
                    payload.Value().slot.value >= view.ExternalSlots().size())
                    return fail("AcquireExternal payload is invalid");
                break;
            }
            case D3D12OperationCode::WaitExternal:
            {
                auto payload = DecodeWaitExternal(operation.payload);
                if (load || !payload || !payload.Value().slot.IsValid() ||
                    payload.Value().slot.value >= view.ExternalSlots().size())
                    return fail("WaitExternal payload is invalid");
                break;
            }
            case D3D12OperationCode::ApplyDynamicData:
            {
                auto payload = DecodeApplyDynamicData(operation.payload);
                if (load || !payload || !payload.Value().slot.IsValid() ||
                    payload.Value().slot.value >= view.DynamicSlots().size())
                    return fail("ApplyDynamicData payload is invalid");
                break;
            }
            case D3D12OperationCode::BeginQueueBatch:
                if (!operation.payload.empty() || batchOpen[operation.queue.value])
                    return fail("queue batch begin is invalid");
                batchOpen[operation.queue.value] = true;
                break;
            case D3D12OperationCode::EndQueueBatch:
                if (!operation.payload.empty() || !batchOpen[operation.queue.value])
                    return fail("queue batch end is invalid");
                batchOpen[operation.queue.value] = false;
                break;
            case D3D12OperationCode::Transition:
            {
                auto payload = DecodeTransition(operation.payload);
                if (load || !payload || !viewValid(payload.Value().view) || payload.Value().flags != 0 ||
                    !batchOpen[operation.queue.value] ||
                    (isCopyQueue(operation.queue) &&
                     (!IsCopyQueueState(payload.Value().before) || !IsCopyQueueState(payload.Value().after))))
                    return fail("Transition payload is invalid");
                break;
            }
            case D3D12OperationCode::ActivateAlias:
            {
                auto payload = DecodeActivateAlias(operation.payload);
                if (!payload || !resourceValid(payload.Value().after) ||
                    (payload.Value().before.IsValid() && !resourceValid(payload.Value().before)))
                    return fail("ActivateAlias payload is invalid");
                break;
            }
            case D3D12OperationCode::ExecuteRaster:
            {
                auto payload = DecodeExecuteRaster(operation.payload);
                if (load || !payload || !batchOpen[operation.queue.value] ||
                    !payload.Value().command.IsValid() || payload.Value().command.value >= view.RasterCommands().size())
                    return fail("ExecuteRaster payload is invalid");
                break;
            }
            case D3D12OperationCode::ExecuteCompute:
            {
                auto payload = DecodeExecuteCompute(operation.payload);
                if (load || !payload || !batchOpen[operation.queue.value] ||
                    !payload.Value().command.IsValid() || payload.Value().command.value >= view.ComputeCommands().size())
                    return fail("ExecuteCompute payload is invalid");
                break;
            }
            case D3D12OperationCode::ExecuteCopy:
            {
                auto payload = DecodeCopyBuffer(operation.payload);
                if (load || !payload || !batchOpen[operation.queue.value] ||
                    !resourceValid(payload.Value().source) || !resourceValid(payload.Value().destination) ||
                    payload.Value().bytes == 0)
                    return fail("ExecuteCopy payload is invalid");
                break;
            }
            case D3D12OperationCode::SignalQueue:
            {
                auto payload = DecodeSignalQueue(operation.payload);
                const auto signal = payload ? signalPoints.find(payload.Value().signalPoint.value) : signalPoints.end();
                if (!payload || signal == signalPoints.end() || signal->second.first != operation.queue ||
                    signal->second.second != operationIndex || batchOpen[operation.queue.value])
                    return fail("SignalQueue must declare its dense identity outside a queue batch");
                break;
            }
            case D3D12OperationCode::WaitQueue:
            {
                auto payload = DecodeWaitQueue(operation.payload);
                const auto signal = payload ? signalPoints.find(payload.Value().signalPoint.value) : signalPoints.end();
                if (!payload || signal == signalPoints.end() || signal->second.second >= operationIndex ||
                    signal->second.first == operation.queue || batchOpen[operation.queue.value])
                    return fail("WaitQueue must reference a preceding signal point on another queue");
                break;
            }
            case D3D12OperationCode::WaitTemporal:
            {
                auto payload = DecodeWaitTemporal(operation.payload);
                const auto signal = payload ? signalPoints.find(payload.Value().producerSignalPoint.value) : signalPoints.end();
                if (load || !payload || !resourceValid(payload.Value().resource) || signal == signalPoints.end() ||
                    batchOpen[operation.queue.value] ||
                    (view.Resources()[payload.Value().resource.value].flags &
                     static_cast<std::uint32_t>(ResourceFlags::Temporal)) == 0)
                    return fail("WaitTemporal must reference a Temporal resource and a declared previous-frame producer signal");
                break;
            }
            case D3D12OperationCode::ReleaseExternal:
            {
                auto payload = DecodeReleaseExternal(operation.payload);
                const auto signal = payload ? signalPoints.find(payload.Value().releaseSignalPoint.value) : signalPoints.end();
                if (load || !payload || !payload.Value().slot.IsValid() ||
                    payload.Value().slot.value >= view.ExternalSlots().size() ||
                    signal == signalPoints.end() || signal->second.second >= operationIndex)
                    return fail("ReleaseExternal must reference a preceding last-use signal point");
                break;
            }
            case D3D12OperationCode::PresentSurface:
            {
                auto payload = DecodePresentSurface(operation.payload);
                if (load || !payload || !payload.Value().slot.IsValid() ||
                    payload.Value().slot.value >= view.SurfaceSlots().size())
                    return fail("PresentSurface payload is invalid");
                break;
            }
            default:
                return fail("operation opcode is unsupported");
            }
        }
        if (std::any_of(batchOpen.begin(), batchOpen.end(), [](bool open) { return open; }))
            return fail("operation stream ends with an open queue batch");
        return base::Result<void, PackageError>::Success();
    };

    auto loadResult = validateStream(view.LoadOperations(), true);
    if (!loadResult) return loadResult;
    for (std::size_t index = 0; index < view.Resources().size(); ++index)
        if (view.Resources()[index].origin == ResourceOrigin::PackageOwned && !resourceCreated[index])
            return fail("a Package-owned resource has no CreateResource operation");
    if (std::any_of(layoutCreated.begin(), layoutCreated.end(), [](bool value) { return !value; }) ||
        std::any_of(rasterCreated.begin(), rasterCreated.end(), [](bool value) { return !value; }) ||
        std::any_of(computeCreated.begin(), computeCreated.end(), [](bool value) { return !value; }))
        return fail("a binding layout or executable has no load materialization operation");
    return validateStream(view.FrameOperations(), false);
}

base::Result<void, PackageError> ValidateReferences(D3D12PackageView& view)
{
    const auto fail = [](PackageErrorCode code, const char* message,
                         SectionKind section = SectionKind::Manifest)
    {
        return base::Result<void, PackageError>::Failure(Error(code, message, section));
    };
    const auto& manifest = view.Manifest();
    if (manifest.resourceCount != view.Resources().size() ||
        manifest.allocationCount != view.Allocations().size() ||
        manifest.viewCount != view.Views().size() || manifest.shaderCount != view.Shaders().size() ||
        manifest.programCount != view.Programs().size() ||
        manifest.bindingLayoutCount != view.BindingLayouts().size() ||
        manifest.executableCount != view.Executables().size() ||
        manifest.rasterCommandCount != view.RasterCommands().size() ||
        manifest.computeExecutableCount != view.ComputeExecutables().size() ||
        manifest.computeCommandCount != view.ComputeCommands().size() ||
        manifest.vertexElementCount != view.VertexElements().size() ||
        manifest.attachmentOperationCount != view.AttachmentOperations().size() ||
        manifest.dynamicSlotCount != view.DynamicSlots().size() ||
        manifest.externalSlotCount != view.ExternalSlots().size() ||
        manifest.surfaceSlotCount != view.SurfaceSlots().size())
        return fail(PackageErrorCode::InvalidReference, "manifest counts do not match decoded tables");

    if (!Dense(view.Resources()) || !Dense(view.Allocations()) || !Dense(view.Views()) ||
        !Dense(view.Shaders()) || !Dense(view.Programs()) || !Dense(view.BindingLayouts()) ||
        !Dense(view.RootParameters()) || !Dense(view.Executables()) ||
        !Dense(view.ComputeExecutables()) || !Dense(view.ComputeCommands()) ||
        !Dense(view.AttachmentOperations()) || !Dense(view.RasterCommands()) ||
        !Dense(view.DynamicSlots()) || !Dense(view.ExternalSlots()) || !Dense(view.SurfaceSlots()))
        return fail(PackageErrorCode::InvalidIdSequence, "Package IDs must be dense and ordered");
    for (std::size_t index = 0; index < view.VertexElements().size(); ++index)
        if (view.VertexElements()[index].id != index)
            return fail(PackageErrorCode::InvalidIdSequence, "vertex element IDs must be dense and ordered",
                        SectionKind::D3D12VertexElementTable);

    const auto rangeValid = [](IndexRange range, std::size_t count) {
        return range.first <= count && range.count <= count - range.first;
    };
    const auto blobValid = [&](const BlobRef& blob, const base::Digest256& digest) {
        auto bytes = view.ResolveBlob(blob);
        return bytes && base::Sha256(bytes.Value()) == digest;
    };

    for (std::size_t index = 0; index < view.Resources().size(); ++index)
    {
        const auto& resource = view.Resources()[index];
        if (resource.firstView > view.Views().size() ||
            resource.viewCount > view.Views().size() - resource.firstView ||
            resource.initialDataOffset > view.InitialData().size() ||
            resource.initialDataSize > view.InitialData().size() - resource.initialDataOffset)
            return fail(PackageErrorCode::InvalidReference, "resource range is out of bounds",
                        SectionKind::D3D12ResourceTable);
        for (std::uint32_t viewIndex = resource.firstView;
             viewIndex < resource.firstView + resource.viewCount; ++viewIndex)
            if (view.Views()[viewIndex].resource.value != index)
                return fail(PackageErrorCode::InvalidReference, "resource view range is not contiguous",
                            SectionKind::D3D12ResourceTable);

        if (resource.origin == ResourceOrigin::PackageOwned)
        {
            if (!resource.allocation.IsValid() || resource.allocation.value >= view.Allocations().size() ||
                resource.rebuildPolicy != RebuildPolicy::RecreateFromPackage ||
                resource.physicalInstanceCount == 0)
                return fail(PackageErrorCode::InvalidReference, "Package-owned resource contract is invalid",
                            SectionKind::D3D12ResourceTable);
            const auto& allocation = view.Allocations()[resource.allocation.value];
            if (allocation.physicalInstanceCount != resource.physicalInstanceCount)
                return fail(PackageErrorCode::InvalidReference, "resource/allocation instance counts differ",
                            SectionKind::D3D12AllocationTable);
        }
        else if (resource.origin == ResourceOrigin::External)
        {
            if (resource.allocation.IsValid() || resource.rebuildPolicy != RebuildPolicy::RequireExternalRebind ||
                resource.physicalInstanceCount != 1 || resource.initialDataSize != 0)
                return fail(PackageErrorCode::InvalidReference, "External resource contract is invalid",
                            SectionKind::D3D12ResourceTable);
        }
        else if (resource.origin == ResourceOrigin::Surface)
        {
            if (resource.allocation.IsValid() || resource.rebuildPolicy != RebuildPolicy::RuntimeManaged ||
                resource.physicalInstanceCount != 0 || resource.initialDataSize != 0)
                return fail(PackageErrorCode::InvalidReference, "Surface resource contract is invalid",
                            SectionKind::D3D12ResourceTable);
        }
        else
        {
            return fail(PackageErrorCode::InvalidEnumValue, "resource origin is unknown",
                        SectionKind::D3D12ResourceTable);
        }
    }

    for (const auto& allocation : view.Allocations())
    {
        if (allocation.kind != AllocationKind::Committed && allocation.kind != AllocationKind::Placed &&
            allocation.kind != AllocationKind::RuntimeManaged)
            return fail(PackageErrorCode::InvalidEnumValue, "allocation kind is unknown",
                        SectionKind::D3D12AllocationTable);
        if (allocation.physicalInstanceCount == 0 || allocation.alignment == 0)
            return fail(PackageErrorCode::InvalidReference, "allocation shape is invalid",
                        SectionKind::D3D12AllocationTable);
        if (allocation.kind == AllocationKind::Placed && allocation.aliasGroup == InvalidIndex)
            return fail(PackageErrorCode::InvalidReference, "placed allocation has no alias group",
                        SectionKind::D3D12AllocationTable);
    }

    for (const auto& resourceView : view.Views())
    {
        if (!resourceView.resource.IsValid() || resourceView.resource.value >= view.Resources().size())
            return fail(PackageErrorCode::InvalidReference, "view references an unknown resource",
                        SectionKind::D3D12ViewTable);
        std::uint32_t capacity = 0;
        if (resourceView.descriptorHeapClass == 1) capacity = view.Profile().rtvDescriptorCount;
        else if (resourceView.descriptorHeapClass == 2) capacity = view.Profile().shaderDescriptorCount;
        else if (resourceView.descriptorHeapClass == 3) capacity = view.Profile().dsvDescriptorCount;
        else if (resourceView.descriptorHeapClass != 0)
            return fail(PackageErrorCode::InvalidReference, "view descriptor heap class is unknown",
                        SectionKind::D3D12ViewTable);

        if (resourceView.descriptorIndex != InvalidIndex)
        {
            const auto& resource = view.Resources()[resourceView.resource.value];
            const auto instances = resource.origin == ResourceOrigin::Surface ?
                view.Profile().surfaceImageCount : resource.physicalInstanceCount;
            const auto last = static_cast<std::uint64_t>(resourceView.descriptorIndex) +
                static_cast<std::uint64_t>(resourceView.descriptorInstanceStride) *
                (instances == 0 ? 0 : instances - 1);
            if (capacity == 0 || last >= capacity)
                return fail(PackageErrorCode::InvalidReference, "view descriptor range exceeds its heap",
                            SectionKind::D3D12ViewTable);
        }
        else if (resourceView.descriptorHeapClass != 0)
        {
            return fail(PackageErrorCode::InvalidReference, "descriptor-backed view has no descriptor index",
                        SectionKind::D3D12ViewTable);
        }
    }

    for (const auto& shader : view.Shaders())
        if (!blobValid(shader.bytecode, shader.bytecodeDigest))
            return fail(PackageErrorCode::DigestMismatch, "shader bytecode digest mismatch",
                        SectionKind::D3D12ShaderTable);

    for (const auto& layout : view.BindingLayouts())
    {
        if (!rangeValid(layout.parameterRange, view.RootParameters().size()) ||
            !blobValid(layout.serializedRootSignature, layout.layoutDigest))
            return fail(PackageErrorCode::InvalidReference, "binding layout range or digest is invalid",
                        SectionKind::D3D12BindingLayoutTable);
        std::uint32_t descriptorCount = 0;
        for (std::uint32_t index = layout.parameterRange.first;
             index < layout.parameterRange.first + layout.parameterRange.count; ++index)
        {
            const auto& parameter = view.RootParameters()[index];
            if (parameter.rootParameterIndex != index - layout.parameterRange.first)
                return fail(PackageErrorCode::InvalidReference, "root parameter indices are not canonical",
                            SectionKind::D3D12RootParameterTable);
            if (parameter.kind == RootParameterKind::ConstantBuffer)
            {
                if (!parameter.dynamicSlot.IsValid() || parameter.dynamicSlot.value >= view.DynamicSlots().size() ||
                    parameter.staticView.IsValid())
                    return fail(PackageErrorCode::InvalidReference, "constant root parameter is invalid",
                                SectionKind::D3D12RootParameterTable);
            }
            else if (parameter.kind == RootParameterKind::ShaderResourceTable ||
                     parameter.kind == RootParameterKind::UnorderedAccessTable)
            {
                if (!parameter.staticView.IsValid() || parameter.staticView.value >= view.Views().size() ||
                    parameter.dynamicSlot.IsValid())
                    return fail(PackageErrorCode::InvalidReference, "descriptor root parameter is invalid",
                                SectionKind::D3D12RootParameterTable);
                ++descriptorCount;
            }
            else
            {
                return fail(PackageErrorCode::InvalidEnumValue, "root parameter kind is unknown",
                            SectionKind::D3D12RootParameterTable);
            }
        }
        if (layout.descriptorRange.count != descriptorCount)
            return fail(PackageErrorCode::InvalidReference, "binding descriptor count is inconsistent",
                        SectionKind::D3D12BindingLayoutTable);
    }

    for (const auto& program : view.Programs())
    {
        if (!program.bindingLayout.IsValid() || program.bindingLayout.value >= view.BindingLayouts().size())
            return fail(PackageErrorCode::InvalidReference, "program references an unknown binding layout",
                        SectionKind::D3D12ProgramTable);
        if (program.kind == ProgramKind::Raster)
        {
            if (!program.vertexShader.IsValid() || program.vertexShader.value >= view.Shaders().size() ||
                !program.pixelShader.IsValid() || program.pixelShader.value >= view.Shaders().size() ||
                program.computeShader.IsValid())
                return fail(PackageErrorCode::InvalidReference, "raster program shader references are invalid",
                            SectionKind::D3D12ProgramTable);
        }
        else if (program.kind == ProgramKind::Compute)
        {
            if (!program.computeShader.IsValid() || program.computeShader.value >= view.Shaders().size() ||
                program.vertexShader.IsValid() || program.pixelShader.IsValid())
                return fail(PackageErrorCode::InvalidReference, "compute program shader references are invalid",
                            SectionKind::D3D12ProgramTable);
        }
        else
            return fail(PackageErrorCode::InvalidEnumValue, "program kind is unknown", SectionKind::D3D12ProgramTable);
    }

    for (const auto& executable : view.Executables())
        if (!executable.program.IsValid() || executable.program.value >= view.Programs().size() ||
            !executable.bindingLayout.IsValid() || executable.bindingLayout.value >= view.BindingLayouts().size() ||
            !rangeValid(executable.vertexElementRange, view.VertexElements().size()))
            return fail(PackageErrorCode::InvalidReference, "raster executable references are invalid",
                        SectionKind::D3D12ExecutableTable);
    for (const auto& executable : view.ComputeExecutables())
        if (!executable.program.IsValid() || executable.program.value >= view.Programs().size() ||
            !executable.bindingLayout.IsValid() || executable.bindingLayout.value >= view.BindingLayouts().size())
            return fail(PackageErrorCode::InvalidReference, "compute executable references are invalid",
                        SectionKind::D3D12ComputeExecutableTable);
    for (const auto& command : view.ComputeCommands())
        if (!command.executable.IsValid() || command.executable.value >= view.ComputeExecutables().size() ||
            command.threadGroupCountX == 0 || command.threadGroupCountY == 0 || command.threadGroupCountZ == 0)
            return fail(PackageErrorCode::InvalidReference, "compute command is invalid",
                        SectionKind::D3D12ComputeCommandTable);
    for (const auto& command : view.RasterCommands())
    {
        if (!command.executable.IsValid() || command.executable.value >= view.Executables().size() ||
            !rangeValid(command.vertexViewRange, view.Views().size()) ||
            !rangeValid(command.colorAttachmentRange, view.Views().size()) || command.vertexCount == 0 ||
            (command.depthAttachment.IsValid() && command.depthAttachment.value >= view.Views().size()) ||
            !command.attachmentOperation.IsValid() ||
            command.attachmentOperation.value >= view.AttachmentOperations().size())
            return fail(PackageErrorCode::InvalidReference, "raster command is invalid",
                        SectionKind::D3D12RasterCommandTable);
    }

    for (const auto& slot : view.DynamicSlots())
        if (!slot.destinationResource.IsValid() || slot.destinationResource.value >= view.Resources().size() ||
            slot.requiredBytes == 0 || slot.requiredAlignment == 0 ||
            !std::has_single_bit(slot.requiredAlignment))
            return fail(PackageErrorCode::InvalidInvocationSchema, "dynamic slot contract is invalid",
                        SectionKind::D3D12DynamicSlotTable);
    for (const auto& slot : view.ExternalSlots())
        if (!slot.resource.IsValid() || slot.resource.value >= view.Resources().size() ||
            view.Resources()[slot.resource.value].origin != ResourceOrigin::External || slot.minimumBytes == 0)
            return fail(PackageErrorCode::InvalidInvocationSchema, "external slot contract is invalid",
                        SectionKind::D3D12ExternalSlotTable);
    for (const auto& slot : view.SurfaceSlots())
        if (!slot.imageResource.IsValid() || slot.imageResource.value >= view.Resources().size() ||
            view.Resources()[slot.imageResource.value].origin != ResourceOrigin::Surface)
            return fail(PackageErrorCode::InvalidInvocationSchema, "surface slot contract is invalid",
                        SectionKind::D3D12SurfaceSlotTable);

    const bool hasSurfaceSlot = !view.SurfaceSlots().empty();
    if (hasSurfaceSlot != (view.Profile().surfaceImageCount != 0))
        return fail(PackageErrorCode::InvalidTargetProfile,
                    "surfaceImageCount must be zero exactly when the Package has no Surface slot",
                    SectionKind::D3D12TargetProfile);
    const auto surfaceResourceCount = static_cast<std::size_t>(std::count_if(
        view.Resources().begin(), view.Resources().end(), [](const ResourceArtifact& resource) {
            return resource.origin == ResourceOrigin::Surface;
        }));
    if (surfaceResourceCount != view.SurfaceSlots().size())
        return fail(PackageErrorCode::InvalidInvocationSchema,
                    "Surface resources and Surface slots must have one-to-one correspondence",
                    SectionKind::D3D12SurfaceSlotTable);
    if (!hasSurfaceSlot && std::any_of(view.Resources().begin(), view.Resources().end(),
        [](const ResourceArtifact& resource) { return resource.extentMode == ExtentMode::SurfaceRelative; }))
        return fail(PackageErrorCode::InvalidReference,
                    "a surface-relative Package resource requires a Surface slot",
                    SectionKind::D3D12ResourceTable);

    return ValidateOperations(view);
}
}

base::Result<std::vector<std::byte>, PackageError> BuildFrozenPackage(const D3D12PackageDescription& description)
{
    if (!DenseIds(description))
        return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::InvalidIdSequence, "description IDs must be dense and ordered"));
    if (description.operationStreams.size() != 2 || description.operationStreams[0].kind != OperationStreamKind::Load ||
        description.operationStreams[1].kind != OperationStreamKind::Frame)
        return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "a Package requires exactly one load stream followed by one frame stream"));

    PackageManifest manifest;
    manifest.resourceCount = static_cast<std::uint32_t>(description.resources.size());
    manifest.allocationCount = static_cast<std::uint32_t>(description.allocations.size());
    manifest.viewCount = static_cast<std::uint32_t>(description.views.size());
    manifest.shaderCount = static_cast<std::uint32_t>(description.shaders.size());
    manifest.programCount = static_cast<std::uint32_t>(description.programs.size());
    manifest.bindingLayoutCount = static_cast<std::uint32_t>(description.bindingLayouts.size());
    manifest.executableCount = static_cast<std::uint32_t>(description.executables.size());
    manifest.rasterCommandCount = static_cast<std::uint32_t>(description.rasterCommands.size());
    manifest.computeExecutableCount = static_cast<std::uint32_t>(description.computeExecutables.size());
    manifest.computeCommandCount = static_cast<std::uint32_t>(description.computeCommands.size());
    manifest.vertexElementCount = static_cast<std::uint32_t>(description.vertexElements.size());
    manifest.attachmentOperationCount = static_cast<std::uint32_t>(description.attachmentOperations.size());
    manifest.dynamicSlotCount = static_cast<std::uint32_t>(description.dynamicSlots.size());
    manifest.externalSlotCount = static_cast<std::uint32_t>(description.externalSlots.size());
    manifest.surfaceSlotCount = static_cast<std::uint32_t>(description.surfaceSlots.size());
    manifest.loadOperationStream = 0;
    manifest.frameOperationStream = 1;

    auto resources = EncodeTable<ResourceArtifact>(description.resources, ResourceStride, [](base::BinaryWriter& writer, const ResourceArtifact& value) {
        WriteId(writer, value.id); writer.WriteU16(static_cast<std::uint16_t>(value.resourceKind)); writer.WriteU16(static_cast<std::uint16_t>(value.origin));
        writer.WriteU16(static_cast<std::uint16_t>(value.rebuildPolicy)); writer.WriteU16(static_cast<std::uint16_t>(value.extentMode)); writer.WriteU32(value.flags);
        writer.WriteU32(value.physicalInstanceCount); WriteId(writer, value.allocation); writer.WriteU32(static_cast<std::uint32_t>(value.format)); writer.WriteU32(value.usageFlags);
        WriteState(writer, value.initialState); writer.WriteU64(value.sizeBytes); writer.WriteU32(value.width); writer.WriteU32(value.height); writer.WriteU16(value.depthOrArraySize);
        writer.WriteU16(value.mipLevels); writer.WriteU16(value.sampleCount); writer.WriteU16(value.planeCount); writer.WriteU64(value.initialDataOffset); writer.WriteU64(value.initialDataSize);
        writer.WriteU32(value.firstView); writer.WriteU32(value.viewCount);
    });
    auto allocations = EncodeTable<AllocationArtifact>(description.allocations, AllocationStride, [](base::BinaryWriter& writer, const AllocationArtifact& value) {
        WriteId(writer, value.id); writer.WriteU16(static_cast<std::uint16_t>(value.kind)); writer.WriteU16(static_cast<std::uint16_t>(value.heapClass)); writer.WriteU32(value.flags);
        writer.WriteU32(value.physicalInstanceCount); writer.WriteU64(value.sizeBytes); writer.WriteU64(value.alignment); writer.WriteU32(value.aliasGroup);
    });
    auto views = EncodeTable<ResourceViewArtifact>(description.views, ViewStride, [](base::BinaryWriter& writer, const ResourceViewArtifact& value) {
        WriteId(writer, value.id); WriteId(writer, value.resource); writer.WriteU16(static_cast<std::uint16_t>(value.viewClass)); writer.WriteU16(0);
        writer.WriteU32(static_cast<std::uint32_t>(value.format)); writer.WriteU32(value.flags); writer.WriteU32(0); writer.WriteU64(value.byteOffset); writer.WriteU64(value.byteSize);
        writer.WriteU32(value.strideBytes); writer.WriteU16(value.firstMip); writer.WriteU16(value.mipCount); writer.WriteU16(value.firstArrayLayer); writer.WriteU16(value.arrayLayerCount);
        writer.WriteU16(value.firstPlane); writer.WriteU16(value.planeCount); writer.WriteU32(value.descriptorHeapClass); writer.WriteU32(value.descriptorIndex); writer.WriteU32(value.descriptorInstanceStride);
    });
    auto shaders = EncodeTable<ShaderArtifact>(description.shaders, ShaderStride, [](base::BinaryWriter& writer, const ShaderArtifact& value) {
        WriteId(writer, value.id); writer.WriteU16(static_cast<std::uint16_t>(value.stage)); writer.WriteU16(static_cast<std::uint16_t>(value.format));
        writer.WriteU16(value.shaderModelMajor); writer.WriteU16(value.shaderModelMinor); writer.WriteU32(value.flags); WriteBlobRef(writer, value.bytecode); writer.WriteBytes(value.bytecodeDigest);
    });
    auto programs = EncodeTable<ProgramArtifact>(description.programs, ProgramStride, [](base::BinaryWriter& writer, const ProgramArtifact& value) {
        WriteId(writer, value.id); writer.WriteU16(static_cast<std::uint16_t>(value.kind)); writer.WriteU16(value.flags); WriteId(writer, value.vertexShader); WriteId(writer, value.pixelShader);
        WriteId(writer, value.computeShader); WriteId(writer, value.bindingLayout); writer.WriteBytes(value.interfaceDigest);
    });
    auto bindings = EncodeTable<BindingLayoutArtifact>(description.bindingLayouts, BindingLayoutStride, [](base::BinaryWriter& writer, const BindingLayoutArtifact& value) {
        WriteId(writer, value.id); writer.WriteU16(value.rootSignatureMajor); writer.WriteU16(value.rootSignatureMinor); writer.WriteU32(value.flags);
        WriteRange(writer, value.parameterRange); WriteRange(writer, value.descriptorRange); WriteRange(writer, value.staticSamplerRange); WriteBlobRef(writer, value.serializedRootSignature);
        writer.WriteBytes(value.layoutDigest); writer.WriteU32(0);
    });
    auto rootParameters = EncodeTable<RootParameterArtifact>(description.rootParameters, RootParameterStride, [](base::BinaryWriter& writer, const RootParameterArtifact& value) {
        WriteId(writer, value.id); writer.WriteU16(static_cast<std::uint16_t>(value.kind)); writer.WriteU16(static_cast<std::uint16_t>(value.visibility)); writer.WriteU32(value.rootParameterIndex);
        writer.WriteU32(value.shaderRegister); writer.WriteU32(value.registerSpace); WriteId(writer, value.dynamicSlot); WriteId(writer, value.staticView); writer.WriteU32(value.flags); writer.WriteU32(0); writer.WriteU32(0);
    });
    auto vertexElements = EncodeTable<VertexElementArtifact>(description.vertexElements, VertexElementStride, [](base::BinaryWriter& writer, const VertexElementArtifact& value) {
        writer.WriteU32(value.id); writer.WriteU16(static_cast<std::uint16_t>(value.meaning)); writer.WriteU16(value.semanticIndex); writer.WriteU32(static_cast<std::uint32_t>(value.format));
        writer.WriteU32(value.inputSlot); writer.WriteU32(value.alignedByteOffset); writer.WriteU32(value.instanceStepRate); writer.WriteU32(value.flags); writer.WriteU32(0);
    });
    auto executables = EncodeTable<RasterExecutableArtifact>(description.executables, ExecutableStride, [](base::BinaryWriter& writer, const RasterExecutableArtifact& value) {
        WriteId(writer, value.id); WriteId(writer, value.program); WriteId(writer, value.bindingLayout); writer.WriteU32(0); WriteRange(writer, value.vertexElementRange); WriteRange(writer, value.colorFormatRange);
        writer.WriteU32(static_cast<std::uint32_t>(value.colorFormat)); writer.WriteU32(static_cast<std::uint32_t>(value.depthFormat)); writer.WriteU32(static_cast<std::uint32_t>(value.primitiveTopology));
        writer.WriteU32(static_cast<std::uint32_t>(value.primitiveTopologyType)); writer.WriteU32(value.rasterStateId); writer.WriteU32(value.blendStateId); writer.WriteU32(value.depthStateId);
        writer.WriteU32(value.sampleCount); writer.WriteU32(value.sampleQuality); writer.WriteU32(0); writer.WriteBytes(value.specializationDigest);
    });

    auto computeExecutables = EncodeTable<ComputeExecutableArtifact>(description.computeExecutables, ComputeExecutableStride, [](base::BinaryWriter& writer, const ComputeExecutableArtifact& value) {
        WriteId(writer, value.id); WriteId(writer, value.program); WriteId(writer, value.bindingLayout); writer.WriteU32(value.flags); writer.WriteBytes(value.specializationDigest);
    });
    auto computeCommands = EncodeTable<ComputeCommandArtifact>(description.computeCommands, ComputeCommandStride, [](base::BinaryWriter& writer, const ComputeCommandArtifact& value) {
        WriteId(writer, value.id); WriteId(writer, value.executable); writer.WriteU32(value.threadGroupCountX); writer.WriteU32(value.threadGroupCountY); writer.WriteU32(value.threadGroupCountZ); writer.WriteU32(value.flags);
    });
    auto attachments = EncodeTable<AttachmentOperationArtifact>(description.attachmentOperations, AttachmentOperationStride, [](base::BinaryWriter& writer, const AttachmentOperationArtifact& value) {
        WriteId(writer, value.id); writer.WriteU16(static_cast<std::uint16_t>(value.colorLoad)); writer.WriteU16(static_cast<std::uint16_t>(value.colorStore));
        for (float component : value.clearColor)
        {
            if (!std::isfinite(component)) throw std::runtime_error("clear color must be finite");
            if (component == 0.0f) component = 0.0f;
            writer.WriteU32(std::bit_cast<std::uint32_t>(component));
        }
        if (!std::isfinite(value.clearDepth) || value.clearDepth < 0.0f || value.clearDepth > 1.0f || value.clearStencil > 255u)
            throw std::runtime_error("depth clear value is invalid");
        writer.WriteU16(static_cast<std::uint16_t>(value.depthLoad));
        writer.WriteU16(static_cast<std::uint16_t>(value.depthStore));
        float clearDepth = value.clearDepth == 0.0f ? 0.0f : value.clearDepth;
        writer.WriteU32(std::bit_cast<std::uint32_t>(clearDepth));
        writer.WriteU32(value.clearStencil);
        writer.WriteU32(0); writer.WriteU32(0); writer.WriteU32(0);
    });
    auto commands = EncodeTable<RasterCommandArtifact>(description.rasterCommands, RasterCommandStride, [](base::BinaryWriter& writer, const RasterCommandArtifact& value) {
        WriteId(writer, value.id); WriteId(writer, value.executable); WriteRange(writer, value.vertexViewRange); WriteId(writer, value.indexView); WriteRange(writer, value.colorAttachmentRange);
        WriteId(writer, value.depthAttachment); writer.WriteU32(value.vertexCount); writer.WriteU32(value.instanceCount); writer.WriteU32(value.firstVertex); writer.WriteU32(value.firstInstance);
        writer.WriteU32(value.indexCount); writer.WriteU32(value.firstIndex); writer.WriteI32(value.baseVertex); writer.WriteU32(value.viewportId); writer.WriteU32(value.scissorId);
        WriteId(writer, value.attachmentOperation); writer.WriteU32(0);
    });
    auto dynamicSlots = EncodeTable<DynamicDataSlotArtifact>(description.dynamicSlots, DynamicSlotStride, [](base::BinaryWriter& writer, const DynamicDataSlotArtifact& value) {
        WriteId(writer, value.id); WriteId(writer, value.destinationResource); writer.WriteU64(value.destinationOffset); writer.WriteU64(value.requiredBytes);
        writer.WriteU32(value.requiredAlignment); writer.WriteU32(value.flags);
    });
    auto externalSlots = EncodeTable<ExternalResourceSlotArtifact>(description.externalSlots, ExternalSlotStride, [](base::BinaryWriter& writer, const ExternalResourceSlotArtifact& value) {
        WriteId(writer, value.id); WriteId(writer, value.resource); writer.WriteU32(static_cast<std::uint32_t>(value.requiredKind));
        writer.WriteU32(static_cast<std::uint32_t>(value.requiredFormat)); writer.WriteU64(value.minimumBytes);
        WriteState(writer, value.requiredIncomingState); WriteState(writer, value.guaranteedOutgoingState);
        writer.WriteU32(static_cast<std::uint32_t>(value.synchronizationContract)); writer.WriteU32(value.flags);
    });
    auto surfaces = EncodeTable<SurfaceSlotArtifact>(description.surfaceSlots, SurfaceSlotStride, [](base::BinaryWriter& writer, const SurfaceSlotArtifact& value) {
        WriteId(writer, value.id); WriteId(writer, value.imageResource); writer.WriteU32(static_cast<std::uint32_t>(value.requiredFormat)); WriteState(writer, value.acquiredState);
        WriteState(writer, value.presentedState); writer.WriteU32(value.flags);
    });
    auto streams = EncodeTable<OperationStreamArtifact>(description.operationStreams, OperationStreamStride, [](base::BinaryWriter& writer, const OperationStreamArtifact& value) {
        writer.WriteU32(static_cast<std::uint32_t>(value.kind)); writer.WriteU32(value.firstOperation); writer.WriteU32(value.operationCount); writer.WriteU32(value.flags);
    });

    base::BinaryWriter payloadWriter;
    base::BinaryWriter operationWriter;
    for (const auto& operation : description.operations)
    {
        if (!KnownOpcode(operation.opcode))
            return base::Result<std::vector<std::byte>, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "unknown operation cannot be serialized"));
        payloadWriter.Align(8);
        const auto offset = payloadWriter.Size();
        payloadWriter.WriteBytes(operation.payload);
        operationWriter.WriteU32(static_cast<std::uint32_t>(operation.opcode));
        operationWriter.WriteU16(operation.operationVersion);
        operationWriter.WriteU16(operation.flags);
        WriteId(operationWriter, operation.queue);
        operationWriter.WriteU32(static_cast<std::uint32_t>(operation.payload.size()));
        operationWriter.WriteU64(offset);
    }

    base::BinaryWriter invocation;
    invocation.WriteU32(static_cast<std::uint32_t>(description.dynamicSlots.size()));
    invocation.WriteU32(static_cast<std::uint32_t>(description.externalSlots.size()));
    invocation.WriteU32(static_cast<std::uint32_t>(description.surfaceSlots.size()));
    invocation.WriteU32(0);

    base::BinaryWriter descriptorPlan;
    descriptorPlan.WriteU32(description.profile.rtvDescriptorCount);
    descriptorPlan.WriteU32(description.profile.dsvDescriptorCount);
    descriptorPlan.WriteU32(description.profile.shaderDescriptorCount);
    descriptorPlan.WriteU32(description.profile.samplerDescriptorCount);

    PackageBuildInput input;
    input.targetKind = TargetKindD3D12;
    input.targetSchemaVersion = TargetSchemaVersion;
    input.minimumRuntimeVersion = MinimumRuntimeVersion;
    input.sections.push_back(MakeSection(SectionKind::Manifest, EncodeManifest(manifest), 1, ManifestStride));
    input.sections.push_back(MakeBlobSection(SectionKind::InvocationSchema, std::move(invocation).Take()));
    input.sections.push_back(MakeSection(SectionKind::OperationStreamTable, std::move(streams), static_cast<std::uint32_t>(description.operationStreams.size()), OperationStreamStride));
    input.sections.push_back(MakeSection(SectionKind::OperationTable, std::move(operationWriter).Take(), static_cast<std::uint32_t>(description.operations.size()), OperationStride));
    input.sections.push_back(MakeBlobSection(SectionKind::OperationPayload, std::move(payloadWriter).Take()));
    input.sections.push_back(MakeBlobSection(SectionKind::InitialData, description.initialData));
    input.sections.push_back(MakeBlobSection(SectionKind::ShaderData, description.shaderData));
    input.sections.push_back(MakeBlobSection(SectionKind::NativeObjectData, description.nativeObjectData));
    input.sections.push_back(MakeSection(SectionKind::D3D12TargetProfile, EncodeProfile(description.profile), 1, ProfileStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12ResourceTable, std::move(resources), static_cast<std::uint32_t>(description.resources.size()), ResourceStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12AllocationTable, std::move(allocations), static_cast<std::uint32_t>(description.allocations.size()), AllocationStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12ViewTable, std::move(views), static_cast<std::uint32_t>(description.views.size()), ViewStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12ShaderTable, std::move(shaders), static_cast<std::uint32_t>(description.shaders.size()), ShaderStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12ProgramTable, std::move(programs), static_cast<std::uint32_t>(description.programs.size()), ProgramStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12BindingLayoutTable, std::move(bindings), static_cast<std::uint32_t>(description.bindingLayouts.size()), BindingLayoutStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12ExecutableTable, std::move(executables), static_cast<std::uint32_t>(description.executables.size()), ExecutableStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12RasterCommandTable, std::move(commands), static_cast<std::uint32_t>(description.rasterCommands.size()), RasterCommandStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12ComputeExecutableTable, std::move(computeExecutables), static_cast<std::uint32_t>(description.computeExecutables.size()), ComputeExecutableStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12ComputeCommandTable, std::move(computeCommands), static_cast<std::uint32_t>(description.computeCommands.size()), ComputeCommandStride));
    input.sections.push_back(MakeBlobSection(SectionKind::D3D12DescriptorPlan, std::move(descriptorPlan).Take()));
    input.sections.push_back(MakeSection(SectionKind::D3D12DynamicSlotTable, std::move(dynamicSlots), static_cast<std::uint32_t>(description.dynamicSlots.size()), DynamicSlotStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12ExternalSlotTable, std::move(externalSlots), static_cast<std::uint32_t>(description.externalSlots.size()), ExternalSlotStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12SurfaceSlotTable, std::move(surfaces), static_cast<std::uint32_t>(description.surfaceSlots.size()), SurfaceSlotStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12VertexElementTable, std::move(vertexElements), static_cast<std::uint32_t>(description.vertexElements.size()), VertexElementStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12AttachmentOperationTable, std::move(attachments), static_cast<std::uint32_t>(description.attachmentOperations.size()), AttachmentOperationStride));
    input.sections.push_back(MakeSection(SectionKind::D3D12RootParameterTable, std::move(rootParameters), static_cast<std::uint32_t>(description.rootParameters.size()), RootParameterStride));
    return PackageWriter::Write(std::move(input));
}

base::Result<D3D12PackageView, PackageError> D3D12PackageView::Decode(const FrozenExecutablePackage& package)
{
    if (package.Target() != TargetKindD3D12 || package.Header().targetSchemaVersion != TargetSchemaVersion)
        return base::Result<D3D12PackageView, PackageError>::Failure(Error(PackageErrorCode::UnsupportedTargetSchema, "package is not D3D12 schema V12"));
    if (package.Header().minimumRuntimeVersion > MinimumRuntimeVersion)
        return base::Result<D3D12PackageView, PackageError>::Failure(Error(PackageErrorCode::TargetCapabilityMismatch, "package requires a newer Package Runtime"));

    D3D12PackageView output;
    output.package_ = &package;
    auto manifestSection = RequireSection(package, SectionKind::Manifest, ManifestStride);
    auto invocationSection = RequireSection(package, SectionKind::InvocationSchema);
    auto descriptorPlanSection = RequireSection(package, SectionKind::D3D12DescriptorPlan);
    auto profileSection = RequireSection(package, SectionKind::D3D12TargetProfile, ProfileStride);
    auto resourceSection = RequireSection(package, SectionKind::D3D12ResourceTable, ResourceStride);
    auto allocationSection = RequireSection(package, SectionKind::D3D12AllocationTable, AllocationStride);
    auto viewSection = RequireSection(package, SectionKind::D3D12ViewTable, ViewStride);
    auto shaderSection = RequireSection(package, SectionKind::D3D12ShaderTable, ShaderStride);
    auto programSection = RequireSection(package, SectionKind::D3D12ProgramTable, ProgramStride);
    auto bindingSection = RequireSection(package, SectionKind::D3D12BindingLayoutTable, BindingLayoutStride);
    auto rootParameterSection = RequireSection(package, SectionKind::D3D12RootParameterTable, RootParameterStride);
    auto vertexSection = RequireSection(package, SectionKind::D3D12VertexElementTable, VertexElementStride);
    auto executableSection = RequireSection(package, SectionKind::D3D12ExecutableTable, ExecutableStride);
    auto computeExecutableSection = RequireSection(package, SectionKind::D3D12ComputeExecutableTable, ComputeExecutableStride);
    auto computeCommandSection = RequireSection(package, SectionKind::D3D12ComputeCommandTable, ComputeCommandStride);
    auto attachmentSection = RequireSection(package, SectionKind::D3D12AttachmentOperationTable, AttachmentOperationStride);
    auto commandSection = RequireSection(package, SectionKind::D3D12RasterCommandTable, RasterCommandStride);
    auto dynamicSection = RequireSection(package, SectionKind::D3D12DynamicSlotTable, DynamicSlotStride);
    auto externalSection = RequireSection(package, SectionKind::D3D12ExternalSlotTable, ExternalSlotStride);
    auto surfaceSection = RequireSection(package, SectionKind::D3D12SurfaceSlotTable, SurfaceSlotStride);
    auto streamSection = RequireSection(package, SectionKind::OperationStreamTable, OperationStreamStride);
    auto operationSection = RequireSection(package, SectionKind::OperationTable, OperationStride);
    auto payloadSection = RequireSection(package, SectionKind::OperationPayload);
    auto initialSection = RequireSection(package, SectionKind::InitialData);
    auto shaderDataSection = RequireSection(package, SectionKind::ShaderData);
    auto nativeSection = RequireSection(package, SectionKind::NativeObjectData);

    const base::Result<const SectionView*, PackageError>* required[] = {
        &manifestSection, &invocationSection, &descriptorPlanSection, &profileSection, &resourceSection, &allocationSection,
        &viewSection, &shaderSection, &programSection, &bindingSection, &rootParameterSection, &vertexSection, &executableSection,
        &computeExecutableSection, &computeCommandSection, &attachmentSection, &commandSection, &dynamicSection, &externalSection, &surfaceSection, &streamSection, &operationSection, &payloadSection,
        &initialSection, &shaderDataSection, &nativeSection
    };
    for (const auto* result : required)
        if (!*result) return base::Result<D3D12PackageView, PackageError>::Failure(result->Error());

    auto manifests = DecodeTable<PackageManifest>(*manifestSection.Value(), ManifestStride, DecodeManifestRecord);
    auto profiles = DecodeTable<TargetProfile>(*profileSection.Value(), ProfileStride, DecodeProfileRecord);
    auto resources = DecodeTable<ResourceArtifact>(*resourceSection.Value(), ResourceStride, DecodeResource);
    auto allocations = DecodeTable<AllocationArtifact>(*allocationSection.Value(), AllocationStride, DecodeAllocation);
    auto views = DecodeTable<ResourceViewArtifact>(*viewSection.Value(), ViewStride, DecodeView);
    auto shaders = DecodeTable<ShaderArtifact>(*shaderSection.Value(), ShaderStride, DecodeShader);
    auto programs = DecodeTable<ProgramArtifact>(*programSection.Value(), ProgramStride, DecodeProgram);
    auto bindings = DecodeTable<BindingLayoutArtifact>(*bindingSection.Value(), BindingLayoutStride, DecodeBinding);
    auto rootParameters = DecodeTable<RootParameterArtifact>(*rootParameterSection.Value(), RootParameterStride, DecodeRootParameter);
    auto vertices = DecodeTable<VertexElementArtifact>(*vertexSection.Value(), VertexElementStride, DecodeVertexElement);
    auto executables = DecodeTable<RasterExecutableArtifact>(*executableSection.Value(), ExecutableStride, DecodeExecutable);
    auto computeExecutables = DecodeTable<ComputeExecutableArtifact>(*computeExecutableSection.Value(), ComputeExecutableStride, DecodeComputeExecutable);
    auto computeCommands = DecodeTable<ComputeCommandArtifact>(*computeCommandSection.Value(), ComputeCommandStride, DecodeComputeCommand);
    auto attachments = DecodeTable<AttachmentOperationArtifact>(*attachmentSection.Value(), AttachmentOperationStride, DecodeAttachment);
    auto commands = DecodeTable<RasterCommandArtifact>(*commandSection.Value(), RasterCommandStride, DecodeRasterCommand);
    auto dynamicSlots = DecodeTable<DynamicDataSlotArtifact>(*dynamicSection.Value(), DynamicSlotStride, DecodeDynamicSlot);
    auto externalSlots = DecodeTable<ExternalResourceSlotArtifact>(*externalSection.Value(), ExternalSlotStride, DecodeExternalSlot);
    auto surfaces = DecodeTable<SurfaceSlotArtifact>(*surfaceSection.Value(), SurfaceSlotStride, DecodeSurface);
    auto streams = DecodeTable<OperationStreamArtifact>(*streamSection.Value(), OperationStreamStride, DecodeStream);
    auto rawOperations = DecodeTable<RawOperation>(*operationSection.Value(), OperationStride, DecodeOperation);

    const PackageError* decodeError = nullptr;
    if (!manifests) decodeError = &manifests.Error();
    else if (!profiles) decodeError = &profiles.Error();
    else if (!resources) decodeError = &resources.Error();
    else if (!allocations) decodeError = &allocations.Error();
    else if (!views) decodeError = &views.Error();
    else if (!shaders) decodeError = &shaders.Error();
    else if (!programs) decodeError = &programs.Error();
    else if (!bindings) decodeError = &bindings.Error();
    else if (!rootParameters) decodeError = &rootParameters.Error();
    else if (!vertices) decodeError = &vertices.Error();
    else if (!executables) decodeError = &executables.Error();
    else if (!computeExecutables) decodeError = &computeExecutables.Error();
    else if (!computeCommands) decodeError = &computeCommands.Error();
    else if (!attachments) decodeError = &attachments.Error();
    else if (!commands) decodeError = &commands.Error();
    else if (!dynamicSlots) decodeError = &dynamicSlots.Error();
    else if (!externalSlots) decodeError = &externalSlots.Error();
    else if (!surfaces) decodeError = &surfaces.Error();
    else if (!streams) decodeError = &streams.Error();
    else if (!rawOperations) decodeError = &rawOperations.Error();
    if (decodeError) return base::Result<D3D12PackageView, PackageError>::Failure(*decodeError);

    if (manifests.Value().size() != 1 || profiles.Value().size() != 1 || streams.Value().size() != 2)
        return base::Result<D3D12PackageView, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "manifest/profile/stream cardinality is invalid"));

    if (invocationSection.Value()->bytes.size() != 16)
        return base::Result<D3D12PackageView, PackageError>::Failure(Error(PackageErrorCode::InvalidInvocationSchema, "invocation schema size is invalid", SectionKind::InvocationSchema));
    base::BinaryReader invocationReader(invocationSection.Value()->bytes);
    auto dynamicCount = invocationReader.ReadU32();
    auto externalCount = invocationReader.ReadU32();
    auto surfaceCount = invocationReader.ReadU32();
    auto invocationReserved = invocationReader.ReadU32();
    if (!dynamicCount || !externalCount || !surfaceCount || !invocationReserved ||
        dynamicCount.Value() != dynamicSlots.Value().size() || externalCount.Value() != externalSlots.Value().size() ||
        surfaceCount.Value() != surfaces.Value().size() || invocationReserved.Value() != 0)
        return base::Result<D3D12PackageView, PackageError>::Failure(Error(PackageErrorCode::InvalidInvocationSchema, "invocation schema does not match slot tables", SectionKind::InvocationSchema));

    if (descriptorPlanSection.Value()->bytes.size() != 16)
        return base::Result<D3D12PackageView, PackageError>::Failure(Error(PackageErrorCode::InvalidTargetProfile, "descriptor plan size is invalid", SectionKind::D3D12DescriptorPlan));
    base::BinaryReader descriptorReader(descriptorPlanSection.Value()->bytes);
    auto rtvCount = descriptorReader.ReadU32();
    auto dsvCount = descriptorReader.ReadU32();
    auto shaderDescriptorCount = descriptorReader.ReadU32();
    auto samplerCount = descriptorReader.ReadU32();
    if (!rtvCount || !dsvCount || !shaderDescriptorCount || !samplerCount ||
        rtvCount.Value() != profiles.Value()[0].rtvDescriptorCount || dsvCount.Value() != profiles.Value()[0].dsvDescriptorCount ||
        shaderDescriptorCount.Value() != profiles.Value()[0].shaderDescriptorCount || samplerCount.Value() != profiles.Value()[0].samplerDescriptorCount)
        return base::Result<D3D12PackageView, PackageError>::Failure(Error(PackageErrorCode::InvalidTargetProfile, "descriptor plan does not match target profile", SectionKind::D3D12DescriptorPlan));

    output.manifest_ = manifests.Value()[0];
    output.profile_ = profiles.Value()[0];
    output.resources_ = std::move(resources).Value();
    output.allocations_ = std::move(allocations).Value();
    output.views_ = std::move(views).Value();
    output.shaders_ = std::move(shaders).Value();
    output.programs_ = std::move(programs).Value();
    output.bindingLayouts_ = std::move(bindings).Value();
    output.rootParameters_ = std::move(rootParameters).Value();
    output.vertexElements_ = std::move(vertices).Value();
    output.executables_ = std::move(executables).Value();
    output.computeExecutables_ = std::move(computeExecutables).Value();
    output.computeCommands_ = std::move(computeCommands).Value();
    output.attachmentOperations_ = std::move(attachments).Value();
    output.rasterCommands_ = std::move(commands).Value();
    output.dynamicSlots_ = std::move(dynamicSlots).Value();
    output.externalSlots_ = std::move(externalSlots).Value();
    output.surfaceSlots_ = std::move(surfaces).Value();
    output.initialData_ = initialSection.Value()->bytes;
    output.shaderData_ = shaderDataSection.Value()->bytes;
    output.nativeObjectData_ = nativeSection.Value()->bytes;

    std::vector<OperationView> operations;
    operations.reserve(rawOperations.Value().size());
    for (std::size_t i = 0; i < rawOperations.Value().size(); ++i)
    {
        const auto& raw = rawOperations.Value()[i];
        if (!KnownOpcode(raw.opcode) || raw.version != ExpectedOperationVersion(raw.opcode))
        {
            auto error = Error(PackageErrorCode::InvalidOperationStream, "unknown operation or version", SectionKind::OperationTable);
            error.operationIndex = static_cast<std::uint32_t>(i);
            return base::Result<D3D12PackageView, PackageError>::Failure(error);
        }
        std::uint64_t end = 0;
        if (!base::CheckedAdd(raw.payloadOffset, raw.payloadBytes, end) || end > payloadSection.Value()->bytes.size())
        {
            auto error = Error(PackageErrorCode::InvalidOperationStream, "operation payload is out of range", SectionKind::OperationPayload);
            error.operationIndex = static_cast<std::uint32_t>(i);
            return base::Result<D3D12PackageView, PackageError>::Failure(error);
        }
        operations.push_back({raw.opcode, raw.version, raw.flags, raw.queue,
            payloadSection.Value()->bytes.subspan(static_cast<std::size_t>(raw.payloadOffset), raw.payloadBytes)});
    }

    const auto& decodedStreams = streams.Value();
    for (const auto& stream : decodedStreams)
        if (static_cast<std::uint64_t>(stream.firstOperation) + stream.operationCount > operations.size())
            return base::Result<D3D12PackageView, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "operation stream range is invalid", SectionKind::OperationStreamTable));
    if (decodedStreams[0].kind != OperationStreamKind::Load || decodedStreams[1].kind != OperationStreamKind::Frame ||
        output.manifest_.loadOperationStream != 0 || output.manifest_.frameOperationStream != 1)
        return base::Result<D3D12PackageView, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "load/frame stream ordering is invalid", SectionKind::OperationStreamTable));

    output.loadOperations_.assign(
        operations.begin() + decodedStreams[0].firstOperation,
        operations.begin() + decodedStreams[0].firstOperation + decodedStreams[0].operationCount);
    output.frameOperations_.assign(
        operations.begin() + decodedStreams[1].firstOperation,
        operations.begin() + decodedStreams[1].firstOperation + decodedStreams[1].operationCount);

    auto references = ValidateReferences(output);
    if (!references) return base::Result<D3D12PackageView, PackageError>::Failure(references.Error());
    auto operationValidation = ValidateOperations(output);
    if (!operationValidation) return base::Result<D3D12PackageView, PackageError>::Failure(operationValidation.Error());
    return base::Result<D3D12PackageView, PackageError>::Success(std::move(output));
}

base::Result<std::span<const std::byte>, PackageError> D3D12PackageView::ResolveBlob(const BlobRef& blob) const
{
    if (blob.reserved != 0)
        return base::Result<std::span<const std::byte>, PackageError>::Failure(Error(PackageErrorCode::InvalidBlobReference, "blob reserved field must be zero", blob.section));
    std::span<const std::byte> source;
    if (blob.section == SectionKind::InitialData) source = initialData_;
    else if (blob.section == SectionKind::ShaderData) source = shaderData_;
    else if (blob.section == SectionKind::NativeObjectData) source = nativeObjectData_;
    else return base::Result<std::span<const std::byte>, PackageError>::Failure(Error(PackageErrorCode::InvalidBlobReference, "blob references a forbidden section", blob.section));
    std::uint64_t end = 0;
    if (!base::CheckedAdd(blob.offset, blob.size, end) || end > source.size())
        return base::Result<std::span<const std::byte>, PackageError>::Failure(Error(PackageErrorCode::InvalidBlobReference, "blob is out of range", blob.section));
    return base::Result<std::span<const std::byte>, PackageError>::Success(source.subspan(static_cast<std::size_t>(blob.offset), static_cast<std::size_t>(blob.size)));
}

std::vector<std::byte> Encode(const CreateResourcePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.resource); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const UploadBufferPayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.resource); writer.WriteU32(0); writer.WriteU64(payload.sourceOffset); writer.WriteU64(payload.bytes); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const UploadTexturePayload& payload)
{
    base::BinaryWriter writer;
    WriteId(writer, payload.resource); writer.WriteU32(0); writer.WriteU64(payload.sourceOffset);
    writer.WriteU32(payload.sourceRowBytes); writer.WriteU32(payload.sourceSliceBytes);
    writer.WriteU16(payload.mipLevel); writer.WriteU16(payload.arrayLayer); writer.WriteU16(payload.plane); writer.WriteU16(payload.reserved);
    return std::move(writer).Take();
}
std::vector<std::byte> Encode(const CreateRootSignaturePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.layout); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const CreateGraphicsPipelinePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.executable); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const CreateComputePipelinePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.executable); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const InitializeStatePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.resource); writer.WriteU32(0); WriteState(writer, payload.before); WriteState(writer, payload.after); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const VerifyBufferContentsPayload& payload)
{
    base::BinaryWriter writer;
    WriteId(writer, payload.resource);
    writer.WriteU32(0);
    writer.WriteU64(payload.resourceOffset);
    writer.WriteU64(payload.expectedDataOffset);
    writer.WriteU64(payload.bytes);
    return std::move(writer).Take();
}
std::vector<std::byte> Encode(const VerifyTextureContentsPayload& payload)
{
    base::BinaryWriter writer;
    WriteId(writer, payload.resource); writer.WriteU32(0); writer.WriteU64(payload.expectedDataOffset);
    writer.WriteU32(payload.expectedRowBytes); writer.WriteU32(payload.width); writer.WriteU32(payload.height); writer.WriteU32(payload.reserved);
    return std::move(writer).Take();
}
std::vector<std::byte> Encode(const AcquireSurfaceImagePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.slot); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const AcquireExternalPayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.slot); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const WaitExternalPayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.slot); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const ApplyDynamicDataPayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.slot); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const TransitionPayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.view); writer.WriteU32(payload.flags); WriteState(writer, payload.before); WriteState(writer, payload.after); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const ExecuteRasterPayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.command); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const ExecuteComputePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.command); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const CopyBufferPayload& payload)
{
    base::BinaryWriter writer;
    WriteId(writer, payload.source);
    WriteId(writer, payload.destination);
    writer.WriteU64(payload.sourceOffset);
    writer.WriteU64(payload.destinationOffset);
    writer.WriteU64(payload.bytes);
    return std::move(writer).Take();
}
std::vector<std::byte> Encode(const SignalQueuePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.signalPoint); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const WaitQueuePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.signalPoint); writer.WriteU32(0); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const WaitTemporalPayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.resource); WriteId(writer, payload.producerSignalPoint); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const ActivateAliasPayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.before); WriteId(writer, payload.after); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const ReleaseExternalPayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.slot); WriteId(writer, payload.releaseSignalPoint); return std::move(writer).Take();
}
std::vector<std::byte> Encode(const PresentSurfacePayload& payload)
{
    base::BinaryWriter writer; WriteId(writer, payload.slot); writer.WriteU32(0); return std::move(writer).Take();
}

namespace
{
template<class T, class DecodeFunction>
base::Result<T, PackageError> DecodePayload(std::span<const std::byte> bytes, std::size_t expected, DecodeFunction decode)
{
    if (bytes.size() != expected)
        return base::Result<T, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "operation payload size mismatch", SectionKind::OperationPayload));
    base::BinaryReader reader(bytes);
    return decode(reader);
}

base::Result<std::uint32_t, PackageError> ReadPayloadU32(base::BinaryReader& reader)
{
    auto value = reader.ReadU32();
    return value ? base::Result<std::uint32_t, PackageError>::Success(value.Value()) :
        base::Result<std::uint32_t, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "operation payload is truncated", SectionKind::OperationPayload));
}

template<class T, class Id>
base::Result<T, PackageError> DecodeIdPayload(std::span<const std::byte> bytes, const char* name)
{
    return DecodePayload<T>(bytes, 8, [name](base::BinaryReader& reader) {
        auto id = ReadPayloadU32(reader);
        auto reserved = ReadPayloadU32(reader);
        if (!id || !reserved || reserved.Value() != 0)
            return base::Result<T, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, std::string(name) + " payload is invalid"));
        return base::Result<T, PackageError>::Success(T{Id{id.Value()}});
    });
}
}

base::Result<CreateResourcePayload, PackageError> DecodeCreateResource(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<CreateResourcePayload, ResourceId>(bytes, "CreateResource");
}
base::Result<UploadBufferPayload, PackageError> DecodeUploadBuffer(std::span<const std::byte> bytes)
{
    return DecodePayload<UploadBufferPayload>(bytes, 24, [](base::BinaryReader& reader) {
        auto id = ReadPayloadU32(reader); auto reserved = ReadPayloadU32(reader); auto offset = reader.ReadU64(); auto count = reader.ReadU64();
        if (!id || !reserved || !offset || !count || reserved.Value() != 0)
            return base::Result<UploadBufferPayload, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "UploadBuffer payload is invalid"));
        return base::Result<UploadBufferPayload, PackageError>::Success({ResourceId{id.Value()}, offset.Value(), count.Value()});
    });
}
base::Result<UploadTexturePayload, PackageError> DecodeUploadTexture(std::span<const std::byte> bytes)
{
    return DecodePayload<UploadTexturePayload>(bytes, 32, [](base::BinaryReader& reader) {
        auto id = ReadPayloadU32(reader); auto reserved32 = ReadPayloadU32(reader); auto offset = reader.ReadU64();
        auto row = reader.ReadU32(); auto slice = reader.ReadU32(); auto mip = reader.ReadU16(); auto arrayLayer = reader.ReadU16();
        auto plane = reader.ReadU16(); auto reserved16 = reader.ReadU16();
        if (!id || !reserved32 || !offset || !row || !slice || !mip || !arrayLayer || !plane || !reserved16 || reserved32.Value() != 0 || reserved16.Value() != 0)
            return base::Result<UploadTexturePayload, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "UploadTexture payload is invalid"));
        return base::Result<UploadTexturePayload, PackageError>::Success({ResourceId{id.Value()}, offset.Value(), row.Value(), slice.Value(), mip.Value(), arrayLayer.Value(), plane.Value(), 0});
    });
}
base::Result<CreateRootSignaturePayload, PackageError> DecodeCreateRootSignature(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<CreateRootSignaturePayload, BindingLayoutId>(bytes, "CreateRootSignature");
}
base::Result<CreateGraphicsPipelinePayload, PackageError> DecodeCreateGraphicsPipeline(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<CreateGraphicsPipelinePayload, ExecutableId>(bytes, "CreateGraphicsPipeline");
}
base::Result<CreateComputePipelinePayload, PackageError> DecodeCreateComputePipeline(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<CreateComputePipelinePayload, ComputeExecutableId>(bytes, "CreateComputePipeline");
}
base::Result<InitializeStatePayload, PackageError> DecodeInitializeState(std::span<const std::byte> bytes)
{
    return DecodePayload<InitializeStatePayload>(bytes, 24, [](base::BinaryReader& reader) {
        auto id = ReadPayloadU32(reader); auto reserved = ReadPayloadU32(reader); auto before = ReadState(reader, SectionKind::OperationPayload); auto after = ReadState(reader, SectionKind::OperationPayload);
        if (!id || !reserved || !before || !after || reserved.Value() != 0)
            return base::Result<InitializeStatePayload, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "InitializeState payload is invalid"));
        return base::Result<InitializeStatePayload, PackageError>::Success({ResourceId{id.Value()}, before.Value(), after.Value()});
    });
}
base::Result<VerifyBufferContentsPayload, PackageError> DecodeVerifyBufferContents(std::span<const std::byte> bytes)
{
    return DecodePayload<VerifyBufferContentsPayload>(bytes, 32, [](base::BinaryReader& reader) {
        auto id = ReadPayloadU32(reader);
        auto reserved = ReadPayloadU32(reader);
        auto resourceOffset = reader.ReadU64();
        auto expectedOffset = reader.ReadU64();
        auto count = reader.ReadU64();
        if (!id || !reserved || !resourceOffset || !expectedOffset || !count || reserved.Value() != 0)
            return base::Result<VerifyBufferContentsPayload, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "VerifyBufferContents payload is invalid"));
        return base::Result<VerifyBufferContentsPayload, PackageError>::Success(
            {ResourceId{id.Value()}, resourceOffset.Value(), expectedOffset.Value(), count.Value()});
    });
}
base::Result<VerifyTextureContentsPayload, PackageError> DecodeVerifyTextureContents(std::span<const std::byte> bytes)
{
    return DecodePayload<VerifyTextureContentsPayload>(bytes, 32, [](base::BinaryReader& reader) {
        auto id = ReadPayloadU32(reader); auto reserved0 = ReadPayloadU32(reader); auto expected = reader.ReadU64();
        auto row = reader.ReadU32(); auto width = reader.ReadU32(); auto height = reader.ReadU32(); auto reserved1 = reader.ReadU32();
        if (!id || !reserved0 || !expected || !row || !width || !height || !reserved1 || reserved0.Value() != 0 || reserved1.Value() != 0)
            return base::Result<VerifyTextureContentsPayload, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "VerifyTextureContents payload is invalid"));
        return base::Result<VerifyTextureContentsPayload, PackageError>::Success({ResourceId{id.Value()}, expected.Value(), row.Value(), width.Value(), height.Value(), 0});
    });
}
base::Result<AcquireSurfaceImagePayload, PackageError> DecodeAcquireSurfaceImage(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<AcquireSurfaceImagePayload, SurfaceSlotId>(bytes, "AcquireSurfaceImage");
}
base::Result<AcquireExternalPayload, PackageError> DecodeAcquireExternal(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<AcquireExternalPayload, ExternalSlotId>(bytes, "AcquireExternal");
}
base::Result<WaitExternalPayload, PackageError> DecodeWaitExternal(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<WaitExternalPayload, ExternalSlotId>(bytes, "WaitExternal");
}
base::Result<ApplyDynamicDataPayload, PackageError> DecodeApplyDynamicData(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<ApplyDynamicDataPayload, DynamicSlotId>(bytes, "ApplyDynamicData");
}
base::Result<TransitionPayload, PackageError> DecodeTransition(std::span<const std::byte> bytes)
{
    return DecodePayload<TransitionPayload>(bytes, 24, [](base::BinaryReader& reader) {
        auto id = ReadPayloadU32(reader); auto flags = ReadPayloadU32(reader); auto before = ReadState(reader, SectionKind::OperationPayload); auto after = ReadState(reader, SectionKind::OperationPayload);
        if (!id || !flags || !before || !after)
            return base::Result<TransitionPayload, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "Transition payload is invalid"));
        return base::Result<TransitionPayload, PackageError>::Success({ViewId{id.Value()}, flags.Value(), before.Value(), after.Value()});
    });
}
base::Result<ExecuteRasterPayload, PackageError> DecodeExecuteRaster(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<ExecuteRasterPayload, RasterCommandId>(bytes, "ExecuteRaster");
}
base::Result<ExecuteComputePayload, PackageError> DecodeExecuteCompute(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<ExecuteComputePayload, ComputeCommandId>(bytes, "ExecuteCompute");
}
base::Result<CopyBufferPayload, PackageError> DecodeCopyBuffer(std::span<const std::byte> bytes)
{
    return DecodePayload<CopyBufferPayload>(bytes, 32, [](base::BinaryReader& reader) {
        auto source = reader.ReadU32(); auto destination = reader.ReadU32();
        auto sourceOffset = reader.ReadU64(); auto destinationOffset = reader.ReadU64(); auto count = reader.ReadU64();
        if (!source || !destination || !sourceOffset || !destinationOffset || !count)
            return base::Result<CopyBufferPayload, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "ExecuteCopy payload is invalid"));
        return base::Result<CopyBufferPayload, PackageError>::Success(
            {{source.Value()}, {destination.Value()}, sourceOffset.Value(), destinationOffset.Value(), count.Value()});
    });
}
base::Result<SignalQueuePayload, PackageError> DecodeSignalQueue(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<SignalQueuePayload, SignalPointId>(bytes, "SignalQueue");
}
base::Result<WaitQueuePayload, PackageError> DecodeWaitQueue(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<WaitQueuePayload, SignalPointId>(bytes, "WaitQueue");
}
base::Result<WaitTemporalPayload, PackageError> DecodeWaitTemporal(std::span<const std::byte> bytes)
{
    return DecodePayload<WaitTemporalPayload>(bytes, 8, [](base::BinaryReader& reader) {
        auto resource = reader.ReadU32(); auto signal = reader.ReadU32();
        if (!resource || !signal) return base::Result<WaitTemporalPayload, PackageError>::Failure(
            Error(PackageErrorCode::InvalidOperationStream, "WaitTemporal payload is truncated"));
        return base::Result<WaitTemporalPayload, PackageError>::Success({{resource.Value()}, {signal.Value()}});
    });
}
base::Result<ActivateAliasPayload, PackageError> DecodeActivateAlias(std::span<const std::byte> bytes)
{
    return DecodePayload<ActivateAliasPayload>(bytes, 8, [](base::BinaryReader& reader) {
        auto before = reader.ReadU32(); auto after = reader.ReadU32();
        if (!before || !after) return base::Result<ActivateAliasPayload, PackageError>::Failure(Error(PackageErrorCode::InvalidOperationStream, "ActivateAlias payload is truncated"));
        return base::Result<ActivateAliasPayload, PackageError>::Success({{before.Value()}, {after.Value()}});
    });
}
base::Result<ReleaseExternalPayload, PackageError> DecodeReleaseExternal(std::span<const std::byte> bytes)
{
    return DecodePayload<ReleaseExternalPayload>(bytes, 8, [](base::BinaryReader& reader) {
        auto slot = reader.ReadU32(); auto signal = reader.ReadU32();
        if (!slot || !signal) return base::Result<ReleaseExternalPayload, PackageError>::Failure(
            Error(PackageErrorCode::InvalidOperationStream, "ReleaseExternal payload is truncated"));
        return base::Result<ReleaseExternalPayload, PackageError>::Success({{slot.Value()}, {signal.Value()}});
    });
}
base::Result<PresentSurfacePayload, PackageError> DecodePresentSurface(std::span<const std::byte> bytes)
{
    return DecodeIdPayload<PresentSurfacePayload, SurfaceSlotId>(bytes, "PresentSurface");
}
}
