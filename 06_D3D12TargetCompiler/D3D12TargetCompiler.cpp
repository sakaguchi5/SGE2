#include "D3D12TargetCompiler.h"

#include "../00_Base/BinaryIO.h"
#include "../00_Base/Sha256.h"
#include "../04_SemanticAnalysis/SemanticAnalysis.h"
#include "../07_FrozenPackageCore/PackageReader.h"
#include "../08_D3D12PackageSchema/D3D12Encoding.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#ifdef interface
#undef interface
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")

namespace sge::compiler::d3d12
{
using Microsoft::WRL::ComPtr;
namespace pkg = package::d3d12_v13;

namespace
{
constexpr std::uint64_t DefaultPlacementAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
constexpr std::uint64_t ConstantPlacementAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

CompileError Error(std::string stage, std::string message)
{
    return {std::move(stage), std::move(message)};
}

template<class T>
base::Result<T, CompileError> Failure(std::string stage, std::string message)
{
    return base::Result<T, CompileError>::Failure(Error(std::move(stage), std::move(message)));
}

base::Result<std::vector<std::byte>, CompileError> CompileShader(
    const semantic::ProgramSource& source,
    const std::string& entry,
    const char* profile,
    const char* stage)
{
    ComPtr<ID3DBlob> shader;
    ComPtr<ID3DBlob> errors;
    constexpr UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                           D3DCOMPILE_WARNINGS_ARE_ERRORS |
                           D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT result = D3DCompile(
        source.hlslSource.data(), source.hlslSource.size(), nullptr, nullptr, nullptr,
        entry.c_str(), profile, flags, 0, &shader, &errors);
    if (FAILED(result))
    {
        std::string message = "D3DCompile failed";
        if (errors)
            message.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        return Failure<std::vector<std::byte>>(stage, std::move(message));
    }

    ComPtr<ID3DBlob> stripped;
    constexpr UINT stripFlags = D3DCOMPILER_STRIP_REFLECTION_DATA |
                                D3DCOMPILER_STRIP_DEBUG_INFO |
                                D3DCOMPILER_STRIP_TEST_BLOBS |
                                D3DCOMPILER_STRIP_PRIVATE_DATA;
    if (FAILED(D3DStripShader(shader->GetBufferPointer(), shader->GetBufferSize(), stripFlags, &stripped)))
        return Failure<std::vector<std::byte>>(stage, "D3DStripShader failed");
    const auto* begin = static_cast<const std::byte*>(stripped->GetBufferPointer());
    return base::Result<std::vector<std::byte>, CompileError>::Success(
        std::vector<std::byte>(begin, begin + stripped->GetBufferSize()));
}

base::Result<std::vector<std::byte>, CompileError> SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC& description,
    const char* stage)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(result))
    {
        std::string message = "D3D12SerializeRootSignature failed";
        if (errors)
            message.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        return Failure<std::vector<std::byte>>(stage, std::move(message));
    }
    const auto* begin = static_cast<const std::byte*>(blob->GetBufferPointer());
    return base::Result<std::vector<std::byte>, CompileError>::Success(
        std::vector<std::byte>(begin, begin + blob->GetBufferSize()));
}

enum class NativeBindingKind { Constant, ShaderResource, UnorderedAccess };
struct NativeBinding final
{
    NativeBindingKind kind = NativeBindingKind::Constant;
    std::uint32_t shaderRegister = 0;
};

base::Result<std::vector<std::byte>, CompileError> SerializeBindingLayout(
    std::span<const NativeBinding> bindings,
    bool raster,
    bool staticSampler)
{
    std::size_t descriptorCount = 0;
    for (const auto& binding : bindings)
        if (binding.kind != NativeBindingKind::Constant) ++descriptorCount;

    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(descriptorCount);
    std::vector<D3D12_ROOT_PARAMETER> parameters(bindings.size());
    std::size_t rangeIndex = 0;
    for (std::size_t index = 0; index < bindings.size(); ++index)
    {
        const auto& binding = bindings[index];
        auto& parameter = parameters[index];
        parameter.ShaderVisibility = raster ?
            (binding.kind == NativeBindingKind::Constant ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL) :
            D3D12_SHADER_VISIBILITY_ALL;
        if (binding.kind == NativeBindingKind::Constant)
        {
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameter.Descriptor.ShaderRegister = binding.shaderRegister;
            parameter.Descriptor.RegisterSpace = 0;
            continue;
        }
        auto& range = ranges[rangeIndex++];
        range.RangeType = binding.kind == NativeBindingKind::ShaderResource ?
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = binding.shaderRegister;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = raster ? D3D12_SHADER_VISIBILITY_PIXEL : D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.empty() ? nullptr : parameters.data();
    description.NumStaticSamplers = staticSampler ? 1u : 0u;
    description.pStaticSamplers = staticSampler ? &sampler : nullptr;
    description.Flags = raster ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT :
                                 D3D12_ROOT_SIGNATURE_FLAG_NONE;
    return SerializeRootSignature(description, raster ? "raster-root-signature" : "compute-root-signature");
}

pkg::ResourceState Common() noexcept { return {pkg::StateClass::Common, 0, 0}; }
pkg::ResourceState Present() noexcept { return {pkg::StateClass::Present, 0, 0}; }
pkg::ResourceState Explicit(pkg::ExplicitStateBits bits) noexcept
{
    return {pkg::StateClass::Explicit, 0, static_cast<std::uint32_t>(bits)};
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

std::uint64_t AppendAligned(std::vector<std::byte>& destination,
                            std::span<const std::byte> source,
                            std::size_t alignment = 16)
{
    while (destination.size() % alignment != 0) destination.push_back(std::byte{0});
    const auto offset = static_cast<std::uint64_t>(destination.size());
    destination.insert(destination.end(), source.begin(), source.end());
    return offset;
}

base::Digest256 InterfaceDigest(const semantic::ProgramInterface& value)
{
    base::BinaryWriter writer;
    writer.WriteU32(value.vertexStrideBytes);
    writer.WriteU32(static_cast<std::uint32_t>(value.vertexInputs.size()));
    for (const auto& input : value.vertexInputs)
    {
        writer.WriteU16(static_cast<std::uint16_t>(input.meaning));
        writer.WriteU16(input.componentCount);
        writer.WriteU32(input.byteOffset);
    }
    writer.WriteU64(value.constantDataBytes);
    writer.WriteU32(value.constantDataAlignment);
    writer.WriteU32(value.sampledTextureCount);
    writer.WriteU32(value.sampledBufferCount);
    writer.WriteU32(value.unorderedBufferCount);
    return base::Sha256(writer.Bytes());
}

base::Digest256 RasterSpecializationDigest(const pkg::RasterExecutableArtifact& executable)
{
    base::BinaryWriter writer;
    writer.WriteU32(executable.program.value);
    writer.WriteU32(executable.bindingLayout.value);
    writer.WriteU32(executable.vertexElementRange.first);
    writer.WriteU32(executable.vertexElementRange.count);
    writer.WriteU32(static_cast<std::uint32_t>(executable.colorFormat));
    writer.WriteU32(static_cast<std::uint32_t>(executable.depthFormat));
    writer.WriteU32(static_cast<std::uint32_t>(executable.primitiveTopology));
    writer.WriteU32(executable.rasterStateId);
    writer.WriteU32(executable.blendStateId);
    writer.WriteU32(executable.depthStateId);
    writer.WriteU32(executable.sampleCount);
    return base::Sha256(writer.Bytes());
}

base::Digest256 ComputeSpecializationDigest(const pkg::ComputeExecutableArtifact& executable)
{
    base::BinaryWriter writer;
    writer.WriteU32(executable.program.value);
    writer.WriteU32(executable.bindingLayout.value);
    writer.WriteU32(executable.flags);
    return base::Sha256(writer.Bytes());
}

pkg::ResourceKind ResourceKind(semantic::ResourceKind kind)
{
    switch (kind)
    {
    case semantic::ResourceKind::Buffer: return pkg::ResourceKind::Buffer;
    case semantic::ResourceKind::Texture2D: return pkg::ResourceKind::Texture2D;
    case semantic::ResourceKind::SurfaceImage: return pkg::ResourceKind::SurfaceImage;
    default: return pkg::ResourceKind::Buffer;
    }
}

pkg::Format Format(semantic::FormatMeaning format)
{
    switch (format)
    {
    case semantic::FormatMeaning::Bgra8Unorm: return pkg::Format::B8G8R8A8Unorm;
    case semantic::FormatMeaning::Depth32Float: return pkg::Format::D32Float;
    default: return pkg::Format::Unknown;
    }
}

pkg::Format ResourceFormat(const semantic::Resource& resource)
{
    if (resource.kind == semantic::ResourceKind::Texture2D) return Format(resource.texture2D.formatMeaning);
    if (resource.kind == semantic::ResourceKind::SurfaceImage) return Format(resource.surface.formatMeaning);
    return pkg::Format::Unknown;
}

pkg::ViewClass ViewClass(semantic::ViewRole role)
{
    using semantic::ViewRole;
    switch (role)
    {
    case ViewRole::VertexData: return pkg::ViewClass::VertexBuffer;
    case ViewRole::ConstantData: return pkg::ViewClass::ConstantBuffer;
    case ViewRole::ColorAttachment: return pkg::ViewClass::RenderTarget;
    case ViewRole::DepthAttachment: return pkg::ViewClass::DepthStencil;
    case ViewRole::StorageBuffer: return pkg::ViewClass::UnorderedAccess;
    case ViewRole::CopySource: return pkg::ViewClass::CopySource;
    case ViewRole::CopyDestination: return pkg::ViewClass::CopyDestination;
    case ViewRole::PresentSource: return pkg::ViewClass::PresentSource;
    default: return pkg::ViewClass::ShaderResource;
    }
}

pkg::ResourceState RequiredState(semantic::ViewRole role)
{
    using semantic::ViewRole;
    switch (role)
    {
    case ViewRole::VertexData: return Explicit(pkg::ExplicitStateBits::VertexBuffer);
    case ViewRole::ConstantData: return Explicit(pkg::ExplicitStateBits::ConstantBuffer);
    case ViewRole::ColorAttachment: return Explicit(pkg::ExplicitStateBits::RenderTarget);
    case ViewRole::DepthAttachment: return Explicit(pkg::ExplicitStateBits::DepthWrite);
    case ViewRole::StorageBuffer: return Explicit(pkg::ExplicitStateBits::UnorderedWrite);
    case ViewRole::CopySource: return Explicit(pkg::ExplicitStateBits::CopySource);
    case ViewRole::CopyDestination: return Explicit(pkg::ExplicitStateBits::CopyDestination);
    case ViewRole::PresentSource: return Present();
    default: return Explicit(pkg::ExplicitStateBits::ShaderRead);
    }
}

bool IsShaderResource(semantic::ViewRole role)
{
    using semantic::ViewRole;
    switch (role)
    {
    case ViewRole::SampledTexture:
    case ViewRole::ComputedBuffer:
    case ViewRole::CopiedBuffer:
    case ViewRole::TemporalPreviousBuffer:
    case ViewRole::AliasedBuffer:
    case ViewRole::ExternalBuffer:
        return true;
    default:
        return false;
    }
}

bool DescriptorBacked(pkg::ViewClass viewClass)
{
    return viewClass == pkg::ViewClass::ShaderResource ||
           viewClass == pkg::ViewClass::UnorderedAccess ||
           viewClass == pkg::ViewClass::RenderTarget ||
           viewClass == pkg::ViewClass::DepthStencil;
}

pkg::HeapClass HeapClassFor(const semantic::Resource& resource,
                            std::span<const semantic::ResourceUse* const> uses)
{
    if (resource.update == semantic::UpdateIntent::DynamicPerFrame) return pkg::HeapClass::Upload;
    if (resource.kind == semantic::ResourceKind::Buffer) return pkg::HeapClass::DefaultBuffer;
    const bool attachment = std::any_of(uses.begin(), uses.end(), [](const semantic::ResourceUse* use) {
        return use->role == semantic::ViewRole::ColorAttachment ||
               use->role == semantic::ViewRole::DepthAttachment;
    });
    return attachment ? pkg::HeapClass::RenderTargetOrDepth : pkg::HeapClass::DefaultTexture;
}

struct ShaderIds final
{
    pkg::ShaderId vertex;
    pkg::ShaderId pixel;
    pkg::ShaderId compute;
};

struct WorkArtifacts final
{
    pkg::ProgramId program;
    pkg::BindingLayoutId layout;
    pkg::ExecutableId rasterExecutable;
    pkg::RasterCommandId rasterCommand;
    pkg::ComputeExecutableId computeExecutable;
    pkg::ComputeCommandId computeCommand;
};

struct StateCell final
{
    std::uint32_t resource = 0;
    std::uint32_t temporalRelation = 0; // 0 normal, 1 current, 2 previous
    auto operator<=>(const StateCell&) const = default;
};
}

base::Result<CompileOutput, CompileError> Compile(
    const semantic::SemanticGraph& graph,
    const target::D3D12TargetProfile& targetProfile)
{
    auto analyzedResult = analysis::Analyze(graph);
    if (!analyzedResult)
    {
        std::string message;
        for (const auto& diagnostic : analyzedResult.Error())
        {
            if (!message.empty()) message += "; ";
            message += diagnostic.message;
        }
        return Failure<CompileOutput>("semantic-analysis", std::move(message));
    }
    const auto& analyzed = analyzedResult.Value();

    if (targetProfile.framesInFlight == 0 || targetProfile.directQueueCount == 0 ||
        targetProfile.shaderBinaryFormat != target::ShaderBinaryFormat::Dxbc ||
        targetProfile.shaderModelMajor != 5 || targetProfile.shaderModelMinor > 1 ||
        targetProfile.rootSignatureMajor != 1 || targetProfile.rootSignatureMinor > 1 ||
        targetProfile.barrierModel != target::BarrierModel::Legacy || targetProfile.surfaceImageCount == 0)
        return Failure<CompileOutput>("target-feasibility", "target profile is outside the generalized D3D12 v13 capability");

    std::map<std::uint32_t, const semantic::Resource*> resources;
    std::map<std::uint32_t, const semantic::ResourceUse*> uses;
    std::map<std::uint32_t, const semantic::Program*> programs;
    std::map<std::uint32_t, const semantic::Work*> works;
    for (const auto& value : graph.resources) resources[value.id.value] = &value;
    for (const auto& value : graph.resourceUses) uses[value.id.value] = &value;
    for (const auto& value : graph.programs) programs[value.id.value] = &value;
    for (const auto& value : graph.works) works[value.id.value] = &value;

    pkg::D3D12PackageDescription description;
    description.profile.minimumFeatureLevel = targetProfile.minimumFeatureLevel;
    description.profile.shaderModelMajor = targetProfile.shaderModelMajor;
    description.profile.shaderModelMinor = targetProfile.shaderModelMinor;
    description.profile.rootSignatureMajor = targetProfile.rootSignatureMajor;
    description.profile.rootSignatureMinor = targetProfile.rootSignatureMinor;
    description.profile.barrierModel = pkg::BarrierModel::Legacy;
    description.profile.shaderBinaryFormat = pkg::ShaderBinaryFormat::Dxbc;
    description.profile.framesInFlight = targetProfile.framesInFlight;
    description.profile.directQueueCount = targetProfile.directQueueCount;
    description.profile.computeQueueCount = targetProfile.computeQueueCount;
    description.profile.copyQueueCount = targetProfile.copyQueueCount;
    description.profile.surfaceImageCount = targetProfile.surfaceImageCount;
    description.profile.rtvDescriptorCount = targetProfile.rtvDescriptorCount;
    description.profile.dsvDescriptorCount = targetProfile.dsvDescriptorCount;
    description.profile.shaderDescriptorCount = targetProfile.shaderDescriptorCount;
    description.profile.samplerDescriptorCount = targetProfile.samplerDescriptorCount;

    std::map<std::uint32_t, pkg::ResourceId> resourceMap;
    std::map<std::uint32_t, std::vector<const semantic::ResourceUse*>> resourceUses;
    for (const auto& use : graph.resourceUses) resourceUses[use.resource.value].push_back(&use);
    for (auto& [resource, resourceUseList] : resourceUses)
        std::sort(resourceUseList.begin(), resourceUseList.end(), [](const auto* left, const auto* right) {
            return left->id.value < right->id.value;
        });

    std::map<std::uint32_t, std::uint32_t> physicalInstances;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
    {
        const auto& source = *resources.at(resourceId.value);
        const pkg::ResourceId packageId{static_cast<std::uint32_t>(description.resources.size())};
        resourceMap[source.id.value] = packageId;

        pkg::ResourceArtifact artifact;
        artifact.id = packageId;
        artifact.resourceKind = ResourceKind(source.kind);
        artifact.format = ResourceFormat(source);
        artifact.sampleCount = 1;
        artifact.planeCount = 1;
        if (source.kind == semantic::ResourceKind::SurfaceImage)
        {
            artifact.origin = pkg::ResourceOrigin::Surface;
            artifact.rebuildPolicy = pkg::RebuildPolicy::RuntimeManaged;
            artifact.extentMode = pkg::ExtentMode::SurfaceRelative;
            artifact.physicalInstanceCount = 0;
            artifact.initialState = Present();
        }
        else if (source.lifetime == semantic::LifetimeIntent::External)
        {
            artifact.origin = pkg::ResourceOrigin::External;
            artifact.rebuildPolicy = pkg::RebuildPolicy::RequireExternalRebind;
            artifact.physicalInstanceCount = 1;
            artifact.initialState = Common();
        }
        else
        {
            artifact.origin = pkg::ResourceOrigin::PackageOwned;
            artifact.rebuildPolicy = pkg::RebuildPolicy::RecreateFromPackage;
            artifact.physicalInstanceCount =
                (source.lifetime == semantic::LifetimeIntent::FrameLocal ||
                 source.lifetime == semantic::LifetimeIntent::Temporal) ? targetProfile.framesInFlight : 1;
            artifact.initialState = source.update == semantic::UpdateIntent::DynamicPerFrame ?
                Explicit(pkg::ExplicitStateBits::ConstantBuffer) : Common();
        }
        physicalInstances[source.id.value] = artifact.physicalInstanceCount;
        if (source.lifetime == semantic::LifetimeIntent::FrameLocal)
            artifact.flags |= static_cast<std::uint32_t>(pkg::ResourceFlags::FrameLocal);
        if (source.lifetime == semantic::LifetimeIntent::Temporal)
            artifact.flags |= static_cast<std::uint32_t>(pkg::ResourceFlags::Temporal);

        if (source.kind == semantic::ResourceKind::Buffer)
        {
            artifact.sizeBytes = source.update == semantic::UpdateIntent::DynamicPerFrame ?
                AlignUp(source.buffer.sizeBytes, ConstantPlacementAlignment) : source.buffer.sizeBytes;
        }
        else if (source.kind == semantic::ResourceKind::Texture2D)
        {
            artifact.extentMode = source.texture2D.extentMeaning == semantic::TextureExtentMeaning::Fixed ?
                pkg::ExtentMode::Fixed : pkg::ExtentMode::SurfaceRelative;
            artifact.width = source.texture2D.width;
            artifact.height = source.texture2D.height;
            artifact.depthOrArraySize = 1;
            artifact.mipLevels = source.texture2D.mipLevels;
        }
        if (!source.initialContent.empty())
        {
            artifact.initialDataOffset = AppendAligned(description.initialData, source.initialContent);
            artifact.initialDataSize = source.initialContent.size();
        }
        description.resources.push_back(artifact);
    }

    std::map<std::uint32_t, pkg::ViewId> viewMap;
    std::uint32_t rtvDescriptors = 0;
    std::uint32_t dsvDescriptors = 0;
    std::uint32_t shaderDescriptors = 0;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
    {
        const auto packageResource = resourceMap.at(resourceId.value);
        auto& resourceArtifact = description.resources[packageResource.value];
        resourceArtifact.firstView = static_cast<std::uint32_t>(description.views.size());
        const auto foundUses = resourceUses.find(resourceId.value);
        if (foundUses != resourceUses.end())
        {
            for (const auto* sourceUse : foundUses->second)
            {
                const auto& sourceResource = *resources.at(resourceId.value);
                pkg::ResourceViewArtifact view;
                view.id = {static_cast<std::uint32_t>(description.views.size())};
                view.resource = packageResource;
                view.viewClass = ViewClass(sourceUse->role);
                view.format = ResourceFormat(sourceResource);
                if (sourceResource.kind == semantic::ResourceKind::Buffer)
                {
                    view.byteSize = sourceResource.buffer.sizeBytes;
                    view.strideBytes = sourceResource.buffer.strideBytes;
                }
                else
                {
                    view.mipCount = sourceResource.kind == semantic::ResourceKind::Texture2D ?
                        sourceResource.texture2D.mipLevels : 1;
                    view.arrayLayerCount = 1;
                    view.planeCount = 1;
                }
                if (sourceResource.lifetime == semantic::LifetimeIntent::Temporal)
                    view.flags = static_cast<std::uint32_t>(
                        sourceUse->role == semantic::ViewRole::TemporalPreviousBuffer ?
                        pkg::ResourceViewFlags::TemporalPrevious : pkg::ResourceViewFlags::TemporalCurrent);

                if (DescriptorBacked(view.viewClass))
                {
                    const auto instances = sourceResource.kind == semantic::ResourceKind::SurfaceImage ?
                        targetProfile.surfaceImageCount : std::max(1u, physicalInstances[sourceResource.id.value]);
                    view.descriptorInstanceStride = instances > 1 ? 1u : 0u;
                    if (view.viewClass == pkg::ViewClass::RenderTarget)
                    {
                        view.descriptorHeapClass = 1;
                        view.descriptorIndex = rtvDescriptors;
                        rtvDescriptors += instances;
                    }
                    else if (view.viewClass == pkg::ViewClass::DepthStencil)
                    {
                        view.descriptorHeapClass = 3;
                        view.descriptorIndex = dsvDescriptors;
                        dsvDescriptors += instances;
                    }
                    else
                    {
                        view.descriptorHeapClass = 2;
                        view.descriptorIndex = shaderDescriptors;
                        shaderDescriptors += instances;
                    }
                }
                viewMap[sourceUse->id.value] = view.id;
                description.views.push_back(view);
            }
        }
        resourceArtifact.viewCount = static_cast<std::uint32_t>(description.views.size()) - resourceArtifact.firstView;
    }

    if (rtvDescriptors > targetProfile.rtvDescriptorCount ||
        dsvDescriptors > targetProfile.dsvDescriptorCount ||
        shaderDescriptors > targetProfile.shaderDescriptorCount)
        return Failure<CompileOutput>("descriptor-planning", "generated descriptor plan exceeds the target profile");

    std::map<std::uint32_t, pkg::DynamicSlotId> dynamicSlots;
    std::map<std::uint32_t, pkg::ExternalSlotId> externalSlots;
    std::map<std::uint32_t, pkg::SurfaceSlotId> surfaceSlots;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
    {
        const auto& source = *resources.at(resourceId.value);
        const auto packageResource = resourceMap.at(resourceId.value);
        if (source.update == semantic::UpdateIntent::DynamicPerFrame)
        {
            pkg::DynamicDataSlotArtifact slot;
            slot.id = {static_cast<std::uint32_t>(description.dynamicSlots.size())};
            slot.destinationResource = packageResource;
            slot.requiredBytes = source.dynamicData.requiredBytes;
            slot.requiredAlignment = source.dynamicData.requiredAlignment;
            dynamicSlots[source.id.value] = slot.id;
            description.dynamicSlots.push_back(slot);
        }
        if (source.lifetime == semantic::LifetimeIntent::External &&
            source.kind != semantic::ResourceKind::SurfaceImage)
        {
            pkg::ExternalResourceSlotArtifact slot;
            slot.id = {static_cast<std::uint32_t>(description.externalSlots.size())};
            slot.resource = packageResource;
            slot.requiredKind = ResourceKind(source.kind);
            slot.requiredFormat = ResourceFormat(source);
            slot.minimumBytes = source.kind == semantic::ResourceKind::Buffer ? source.buffer.sizeBytes : 0;
            const auto& sourceUses = resourceUses[source.id.value];
            const auto state = sourceUses.empty() ? Common() : RequiredState(sourceUses.front()->role);
            slot.requiredIncomingState = state;
            slot.guaranteedOutgoingState = state;
            description.resources[packageResource.value].initialState = state;
            externalSlots[source.id.value] = slot.id;
            description.externalSlots.push_back(slot);
        }
        if (source.kind == semantic::ResourceKind::SurfaceImage)
        {
            pkg::SurfaceSlotArtifact slot;
            slot.id = {static_cast<std::uint32_t>(description.surfaceSlots.size())};
            slot.imageResource = packageResource;
            slot.requiredFormat = ResourceFormat(source);
            slot.acquiredState = Present();
            slot.presentedState = Present();
            surfaceSlots[source.id.value] = slot.id;
            description.surfaceSlots.push_back(slot);
        }
    }

    // G6: start from committed allocations, then safely coalesce each explicit
    // AliasedBuffer target with one compatible, frame-unused Preparation resource.
    std::map<std::uint32_t, analysis::ResourceLifetime> lifetimes;
    for (const auto& lifetime : analyzed.resourceLifetimes)
        lifetimes[lifetime.resource.value] = lifetime;
    std::map<std::uint32_t, std::uint32_t> aliasPairs;
    std::set<std::uint32_t> usedPreparations;
    for (const auto& use : graph.resourceUses)
    {
        if (use.role != semantic::ViewRole::AliasedBuffer) continue;
        const auto& targetResource = *resources.at(use.resource.value);
        for (const auto preparationId : analyzed.canonicalResourceOrder)
        {
            const auto& preparation = *resources.at(preparationId.value);
            if (usedPreparations.contains(preparation.id.value) ||
                preparation.lifetime != semantic::LifetimeIntent::Preparation ||
                preparation.kind != targetResource.kind ||
                lifetimes.at(preparation.id.value).usedByWork)
                continue;
            const auto& targetLifetime = lifetimes.at(targetResource.id.value);
            const auto& preparationLifetime = lifetimes.at(preparation.id.value);
            const bool nonOverlapping = !preparationLifetime.usedByWork || !targetLifetime.usedByWork ||
                preparationLifetime.lastUse < targetLifetime.firstUse ||
                targetLifetime.lastUse < preparationLifetime.firstUse;
            if (!nonOverlapping) continue;
            const bool compatible = preparation.kind == semantic::ResourceKind::Buffer &&
                preparation.buffer.sizeBytes == targetResource.buffer.sizeBytes &&
                preparation.buffer.strideBytes == targetResource.buffer.strideBytes;
            if (!compatible) continue;
            aliasPairs[preparation.id.value] = targetResource.id.value;
            usedPreparations.insert(preparation.id.value);
            break;
        }
    }

    std::map<std::uint32_t, pkg::AllocationId> allocationForResource;
    std::set<std::uint32_t> allocatedResources;
    std::uint32_t aliasGroup = 0;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
    {
        const auto packageResource = resourceMap.at(resourceId.value);
        const auto& source = *resources.at(resourceId.value);
        if (source.lifetime == semantic::LifetimeIntent::External || allocatedResources.contains(resourceId.value))
            continue;

        const auto pairAsPreparation = aliasPairs.find(resourceId.value);
        auto pairAsTarget = std::find_if(aliasPairs.begin(), aliasPairs.end(), [&](const auto& pair) {
            return pair.second == resourceId.value;
        });
        const bool aliased = pairAsPreparation != aliasPairs.end() || pairAsTarget != aliasPairs.end();
        std::uint32_t partnerId = package::InvalidIndex;
        if (pairAsPreparation != aliasPairs.end()) partnerId = pairAsPreparation->second;
        if (pairAsTarget != aliasPairs.end()) partnerId = pairAsTarget->first;

        const auto& sourceUseList = resourceUses[source.id.value];
        pkg::AllocationArtifact allocation;
        allocation.id = {static_cast<std::uint32_t>(description.allocations.size())};
        allocation.kind = aliased ? pkg::AllocationKind::Placed : pkg::AllocationKind::Committed;
        allocation.heapClass = HeapClassFor(source, sourceUseList);
        allocation.physicalInstanceCount = description.resources[packageResource.value].physicalInstanceCount;
        allocation.alignment = source.update == semantic::UpdateIntent::DynamicPerFrame ?
            ConstantPlacementAlignment : DefaultPlacementAlignment;
        allocation.sizeBytes = source.kind == semantic::ResourceKind::Buffer ?
            (aliased ? AlignUp(source.buffer.sizeBytes, DefaultPlacementAlignment) :
             description.resources[packageResource.value].sizeBytes) : 0;
        allocation.aliasGroup = aliased ? aliasGroup++ : package::InvalidIndex;
        description.allocations.push_back(allocation);

        allocationForResource[source.id.value] = allocation.id;
        description.resources[packageResource.value].allocation = allocation.id;
        allocatedResources.insert(source.id.value);
        if (aliased)
        {
            const auto partnerPackage = resourceMap.at(partnerId);
            if (description.resources[partnerPackage.value].physicalInstanceCount != allocation.physicalInstanceCount)
                return Failure<CompileOutput>("allocation-planning", "alias candidates have incompatible physical instance counts");
            allocationForResource[partnerId] = allocation.id;
            description.resources[partnerPackage.value].allocation = allocation.id;
            description.resources[packageResource.value].flags |= static_cast<std::uint32_t>(pkg::ResourceFlags::Aliased);
            description.resources[partnerPackage.value].flags |= static_cast<std::uint32_t>(pkg::ResourceFlags::Aliased);
            allocatedResources.insert(partnerId);
        }
    }

    std::set<std::uint32_t> usedProgramIds;
    for (const auto& work : graph.works)
    {
        if (work.kind == semantic::WorkKind::Raster) usedProgramIds.insert(work.raster.program.value);
        if (work.kind == semantic::WorkKind::Compute) usedProgramIds.insert(work.compute.program.value);
    }
    std::map<std::uint32_t, ShaderIds> shaderMap;
    for (const auto programId : analyzed.canonicalProgramOrder)
    {
        if (!usedProgramIds.contains(programId.value)) continue;
        const auto& source = *programs.at(programId.value);
        ShaderIds ids;
        if (source.kind == semantic::ProgramKind::Raster)
        {
            auto vertex = CompileShader(source.source, source.source.vertexEntry, "vs_5_1", "vertex-shader");
            if (!vertex) return base::Result<CompileOutput, CompileError>::Failure(vertex.Error());
            auto pixel = CompileShader(source.source, source.source.pixelEntry, "ps_5_1", "pixel-shader");
            if (!pixel) return base::Result<CompileOutput, CompileError>::Failure(pixel.Error());
            ids.vertex = {static_cast<std::uint32_t>(description.shaders.size())};
            const auto vertexOffset = AppendAligned(description.shaderData, vertex.Value());
            description.shaders.push_back({ids.vertex, pkg::ShaderStage::Vertex, pkg::ShaderBinaryFormat::Dxbc,
                targetProfile.shaderModelMajor, targetProfile.shaderModelMinor, 0,
                {package::SectionKind::ShaderData, 0, vertexOffset, vertex.Value().size()}, base::Sha256(vertex.Value())});
            ids.pixel = {static_cast<std::uint32_t>(description.shaders.size())};
            const auto pixelOffset = AppendAligned(description.shaderData, pixel.Value());
            description.shaders.push_back({ids.pixel, pkg::ShaderStage::Pixel, pkg::ShaderBinaryFormat::Dxbc,
                targetProfile.shaderModelMajor, targetProfile.shaderModelMinor, 0,
                {package::SectionKind::ShaderData, 0, pixelOffset, pixel.Value().size()}, base::Sha256(pixel.Value())});
        }
        else
        {
            auto compute = CompileShader(source.source, source.source.computeEntry, "cs_5_1", "compute-shader");
            if (!compute) return base::Result<CompileOutput, CompileError>::Failure(compute.Error());
            ids.compute = {static_cast<std::uint32_t>(description.shaders.size())};
            const auto computeOffset = AppendAligned(description.shaderData, compute.Value());
            description.shaders.push_back({ids.compute, pkg::ShaderStage::Compute, pkg::ShaderBinaryFormat::Dxbc,
                targetProfile.shaderModelMajor, targetProfile.shaderModelMinor, 0,
                {package::SectionKind::ShaderData, 0, computeOffset, compute.Value().size()}, base::Sha256(compute.Value())});
        }
        shaderMap[source.id.value] = ids;
    }

    std::map<std::uint32_t, WorkArtifacts> workArtifacts;
    std::uint32_t descriptorBindingOffset = 0;
    std::uint32_t staticSamplerOffset = 0;
    for (const auto workId : analyzed.canonicalWorkOrder)
    {
        const auto& work = *works.at(workId.value);
        if (work.kind != semantic::WorkKind::Raster && work.kind != semantic::WorkKind::Compute) continue;
        const auto sourceProgramId = work.kind == semantic::WorkKind::Raster ?
            work.raster.program : work.compute.program;
        const auto& sourceProgram = *programs.at(sourceProgramId.value);

        std::vector<const semantic::ResourceUse*> bindingUses;
        for (const auto useId : work.uses)
        {
            const auto* use = uses.at(useId.value);
            if (use->role == semantic::ViewRole::ConstantData || IsShaderResource(use->role) ||
                use->role == semantic::ViewRole::StorageBuffer)
                bindingUses.push_back(use);
        }

        const std::uint32_t rootCost = static_cast<std::uint32_t>(std::count_if(
            bindingUses.begin(), bindingUses.end(), [](const auto* use) {
                return use->role == semantic::ViewRole::ConstantData;
            })) * 2u + static_cast<std::uint32_t>(std::count_if(
            bindingUses.begin(), bindingUses.end(), [](const auto* use) {
                return use->role != semantic::ViewRole::ConstantData;
            }));
        if (rootCost > 64)
            return Failure<CompileOutput>("binding-layout", "root signature exceeds the D3D12 64-DWORD limit");

        const pkg::BindingLayoutId layoutId{static_cast<std::uint32_t>(description.bindingLayouts.size())};
        const auto parameterFirst = static_cast<std::uint32_t>(description.rootParameters.size());
        std::uint32_t srvRegister = 0;
        std::uint32_t uavRegister = 0;
        std::uint32_t descriptorCount = 0;
        std::vector<NativeBinding> nativeBindings;
        for (std::uint32_t index = 0; index < bindingUses.size(); ++index)
        {
            const auto* use = bindingUses[index];
            pkg::RootParameterArtifact parameter;
            parameter.id = {static_cast<std::uint32_t>(description.rootParameters.size())};
            parameter.rootParameterIndex = index;
            parameter.visibility = work.kind == semantic::WorkKind::Raster ?
                (use->role == semantic::ViewRole::ConstantData ? pkg::ShaderVisibility::Vertex : pkg::ShaderVisibility::Pixel) :
                pkg::ShaderVisibility::All;
            if (use->role == semantic::ViewRole::ConstantData)
            {
                parameter.kind = pkg::RootParameterKind::ConstantBuffer;
                parameter.shaderRegister = 0;
                parameter.dynamicSlot = dynamicSlots.at(use->resource.value);
                nativeBindings.push_back({NativeBindingKind::Constant, 0});
            }
            else if (use->role == semantic::ViewRole::StorageBuffer)
            {
                parameter.kind = pkg::RootParameterKind::UnorderedAccessTable;
                parameter.shaderRegister = uavRegister++;
                parameter.staticView = viewMap.at(use->id.value);
                nativeBindings.push_back({NativeBindingKind::UnorderedAccess, parameter.shaderRegister});
                ++descriptorCount;
            }
            else
            {
                parameter.kind = pkg::RootParameterKind::ShaderResourceTable;
                parameter.shaderRegister = srvRegister++;
                parameter.staticView = viewMap.at(use->id.value);
                nativeBindings.push_back({NativeBindingKind::ShaderResource, parameter.shaderRegister});
                ++descriptorCount;
            }
            description.rootParameters.push_back(parameter);
        }

        const bool hasStaticSampler = sourceProgram.interface.sampledTextureCount != 0;
        auto rootBytes = SerializeBindingLayout(nativeBindings,
            work.kind == semantic::WorkKind::Raster, hasStaticSampler);
        if (!rootBytes) return base::Result<CompileOutput, CompileError>::Failure(rootBytes.Error());
        const auto rootOffset = AppendAligned(description.nativeObjectData, rootBytes.Value());
        pkg::BindingLayoutArtifact layout;
        layout.id = layoutId;
        layout.rootSignatureMajor = targetProfile.rootSignatureMajor;
        layout.rootSignatureMinor = targetProfile.rootSignatureMinor;
        layout.parameterRange = {parameterFirst, static_cast<std::uint32_t>(bindingUses.size())};
        layout.descriptorRange = {descriptorBindingOffset, descriptorCount};
        layout.staticSamplerRange = {staticSamplerOffset, hasStaticSampler ? 1u : 0u};
        layout.serializedRootSignature = {package::SectionKind::NativeObjectData, 0,
                                          rootOffset, rootBytes.Value().size()};
        layout.layoutDigest = base::Sha256(rootBytes.Value());
        description.bindingLayouts.push_back(layout);
        descriptorBindingOffset += descriptorCount;
        if (hasStaticSampler) ++staticSamplerOffset;

        WorkArtifacts artifacts;
        artifacts.layout = layoutId;
        artifacts.program = {static_cast<std::uint32_t>(description.programs.size())};
        pkg::ProgramArtifact programArtifact;
        programArtifact.id = artifacts.program;
        programArtifact.bindingLayout = layoutId;
        programArtifact.interfaceDigest = InterfaceDigest(sourceProgram.interface);
        const auto shaderIds = shaderMap.at(sourceProgram.id.value);
        if (work.kind == semantic::WorkKind::Raster)
        {
            programArtifact.kind = pkg::ProgramKind::Raster;
            programArtifact.vertexShader = shaderIds.vertex;
            programArtifact.pixelShader = shaderIds.pixel;
        }
        else
        {
            programArtifact.kind = pkg::ProgramKind::Compute;
            programArtifact.computeShader = shaderIds.compute;
        }
        description.programs.push_back(programArtifact);

        if (work.kind == semantic::WorkKind::Raster)
        {
            const auto vertexFirst = static_cast<std::uint32_t>(description.vertexElements.size());
            for (const auto& input : sourceProgram.interface.vertexInputs)
            {
                pkg::VertexElementArtifact element;
                element.id = static_cast<std::uint32_t>(description.vertexElements.size());
                element.meaning = static_cast<pkg::VertexMeaning>(input.meaning);
                element.format = input.componentCount == 2 ? pkg::Format::R32G32Float :
                                 input.componentCount == 3 ? pkg::Format::R32G32B32Float :
                                                           pkg::Format::R32G32B32A32Float;
                element.alignedByteOffset = input.byteOffset;
                description.vertexElements.push_back(element);
            }

            const semantic::ResourceUse* vertex = nullptr;
            const semantic::ResourceUse* color = nullptr;
            const semantic::ResourceUse* depth = nullptr;
            for (const auto useId : work.uses)
            {
                const auto* use = uses.at(useId.value);
                if (use->role == semantic::ViewRole::VertexData) vertex = use;
                if (use->role == semantic::ViewRole::ColorAttachment) color = use;
                if (use->role == semantic::ViewRole::DepthAttachment) depth = use;
            }
            artifacts.rasterExecutable = {static_cast<std::uint32_t>(description.executables.size())};
            pkg::RasterExecutableArtifact executable;
            executable.id = artifacts.rasterExecutable;
            executable.program = artifacts.program;
            executable.bindingLayout = layoutId;
            executable.vertexElementRange = {vertexFirst, static_cast<std::uint32_t>(sourceProgram.interface.vertexInputs.size())};
            executable.colorFormatRange = {0, 1};
            executable.colorFormat = ResourceFormat(*resources.at(color->resource.value));
            executable.depthFormat = depth ? ResourceFormat(*resources.at(depth->resource.value)) : pkg::Format::Unknown;
            executable.primitiveTopology = pkg::PrimitiveTopology::TriangleList;
            executable.primitiveTopologyType = pkg::PrimitiveTopologyType::Triangle;
            executable.depthStateId = depth ? 1u : 0u;
            executable.sampleCount = 1;
            executable.specializationDigest = RasterSpecializationDigest(executable);
            description.executables.push_back(executable);

            pkg::AttachmentOperationArtifact attachment;
            attachment.id = {static_cast<std::uint32_t>(description.attachmentOperations.size())};
            attachment.depthLoad = depth ? pkg::AttachmentLoadOp::Clear : pkg::AttachmentLoadOp::Discard;
            attachment.depthStore = depth ? pkg::AttachmentStoreOp::Store : pkg::AttachmentStoreOp::Discard;
            description.attachmentOperations.push_back(attachment);

            artifacts.rasterCommand = {static_cast<std::uint32_t>(description.rasterCommands.size())};
            pkg::RasterCommandArtifact command;
            command.id = artifacts.rasterCommand;
            command.executable = artifacts.rasterExecutable;
            command.vertexViewRange = {viewMap.at(vertex->id.value).value, 1};
            command.colorAttachmentRange = {viewMap.at(color->id.value).value, 1};
            if (depth) command.depthAttachment = viewMap.at(depth->id.value);
            command.vertexCount = work.raster.vertexCount;
            command.instanceCount = 1;
            command.viewportId = 0;
            command.scissorId = 0;
            command.attachmentOperation = attachment.id;
            description.rasterCommands.push_back(command);
        }
        else
        {
            artifacts.computeExecutable = {static_cast<std::uint32_t>(description.computeExecutables.size())};
            pkg::ComputeExecutableArtifact executable;
            executable.id = artifacts.computeExecutable;
            executable.program = artifacts.program;
            executable.bindingLayout = layoutId;
            executable.specializationDigest = ComputeSpecializationDigest(executable);
            description.computeExecutables.push_back(executable);

            artifacts.computeCommand = {static_cast<std::uint32_t>(description.computeCommands.size())};
            description.computeCommands.push_back({artifacts.computeCommand, artifacts.computeExecutable,
                work.compute.threadGroupCountX, work.compute.threadGroupCountY,
                work.compute.threadGroupCountZ, 0});
        }
        workArtifacts[work.id.value] = artifacts;
    }

    const pkg::QueueId noQueue{};
    const pkg::QueueId directQueue{0};
    const pkg::QueueId computeQueue = targetProfile.computeQueueCount != 0 ?
        pkg::QueueId{targetProfile.directQueueCount} : directQueue;
    const pkg::QueueId copyQueue = targetProfile.copyQueueCount != 0 ?
        pkg::QueueId{targetProfile.directQueueCount + targetProfile.computeQueueCount} : directQueue;
    std::map<std::uint32_t, pkg::QueueId> workQueues;
    for (const auto workId : analyzed.canonicalWorkOrder)
    {
        const auto kind = works.at(workId.value)->kind;
        workQueues[workId.value] = kind == semantic::WorkKind::Copy ? copyQueue :
                                   kind == semantic::WorkKind::Compute ? computeQueue : directQueue;
    }

    const auto addOperation = [&](pkg::D3D12OperationCode code, pkg::QueueId queue,
                                  std::vector<std::byte> payload = {})
    {
        description.operations.push_back({code, 1, 0, queue, std::move(payload)});
    };

    addOperation(pkg::D3D12OperationCode::CreateDescriptorHeaps, noQueue);
    for (const auto& resource : description.resources)
        if (resource.origin == pkg::ResourceOrigin::PackageOwned)
            addOperation(pkg::D3D12OperationCode::CreateResource, noQueue,
                         pkg::Encode(pkg::CreateResourcePayload{resource.id}));

    std::vector<std::uint32_t> loadResources;
    for (const auto resourceId : analyzed.canonicalResourceOrder)
        if (resources.at(resourceId.value)->lifetime == semantic::LifetimeIntent::Preparation)
            loadResources.push_back(resourceId.value);
    for (const auto resourceId : analyzed.canonicalResourceOrder)
        if (resources.at(resourceId.value)->lifetime != semantic::LifetimeIntent::Preparation &&
            resources.at(resourceId.value)->lifetime != semantic::LifetimeIntent::External)
            loadResources.push_back(resourceId.value);

    const bool hasLoadWork = std::any_of(loadResources.begin(), loadResources.end(), [&](std::uint32_t id) {
        const auto packageResource = resourceMap.at(id);
        return description.resources[packageResource.value].initialDataSize != 0 ||
               (description.resources[packageResource.value].flags & static_cast<std::uint32_t>(pkg::ResourceFlags::Aliased)) != 0;
    });
    if (hasLoadWork)
    {
        addOperation(pkg::D3D12OperationCode::BeginQueueBatch, copyQueue);
        std::map<std::uint32_t, pkg::ResourceId> activeAlias;
        for (const auto sourceId : loadResources)
        {
            const auto packageResource = resourceMap.at(sourceId);
            const auto& source = *resources.at(sourceId);
            const auto& artifact = description.resources[packageResource.value];
            const bool aliased = (artifact.flags & static_cast<std::uint32_t>(pkg::ResourceFlags::Aliased)) != 0;
            if (aliased)
            {
                const auto allocation = description.allocations[artifact.allocation.value];
                auto before = activeAlias.find(allocation.aliasGroup);
                addOperation(pkg::D3D12OperationCode::ActivateAlias, copyQueue,
                    pkg::Encode(pkg::ActivateAliasPayload{
                        before == activeAlias.end() ? pkg::ResourceId{} : before->second, packageResource}));
                activeAlias[allocation.aliasGroup] = packageResource;
            }
            if (artifact.initialDataSize == 0) continue;
            addOperation(pkg::D3D12OperationCode::InitializeState, copyQueue,
                pkg::Encode(pkg::InitializeStatePayload{packageResource, artifact.initialState,
                    Explicit(pkg::ExplicitStateBits::CopyDestination)}));
            if (source.kind == semantic::ResourceKind::Buffer)
            {
                addOperation(pkg::D3D12OperationCode::UploadBuffer, copyQueue,
                    pkg::Encode(pkg::UploadBufferPayload{packageResource, artifact.initialDataOffset,
                                                         artifact.initialDataSize}));
                addOperation(pkg::D3D12OperationCode::InitializeState, copyQueue,
                    pkg::Encode(pkg::InitializeStatePayload{packageResource,
                        Explicit(pkg::ExplicitStateBits::CopyDestination),
                        Explicit(pkg::ExplicitStateBits::CopySource)}));
                addOperation(pkg::D3D12OperationCode::VerifyBufferContents, copyQueue,
                    pkg::Encode(pkg::VerifyBufferContentsPayload{packageResource, 0,
                        artifact.initialDataOffset, artifact.initialDataSize}));
            }
            else
            {
                addOperation(pkg::D3D12OperationCode::UploadTexture, copyQueue,
                    pkg::Encode(pkg::UploadTexturePayload{packageResource, artifact.initialDataOffset,
                        source.texture2D.rowBytes, static_cast<std::uint32_t>(artifact.initialDataSize), 0, 0, 0, 0}));
                addOperation(pkg::D3D12OperationCode::InitializeState, copyQueue,
                    pkg::Encode(pkg::InitializeStatePayload{packageResource,
                        Explicit(pkg::ExplicitStateBits::CopyDestination),
                        Explicit(pkg::ExplicitStateBits::CopySource)}));
                addOperation(pkg::D3D12OperationCode::VerifyTextureContents, copyQueue,
                    pkg::Encode(pkg::VerifyTextureContentsPayload{packageResource, artifact.initialDataOffset,
                        source.texture2D.rowBytes, source.texture2D.width, source.texture2D.height, 0}));
            }
            addOperation(pkg::D3D12OperationCode::InitializeState, copyQueue,
                pkg::Encode(pkg::InitializeStatePayload{packageResource,
                    Explicit(pkg::ExplicitStateBits::CopySource), Common()}));
        }
        addOperation(pkg::D3D12OperationCode::EndQueueBatch, copyQueue);
        addOperation(pkg::D3D12OperationCode::SignalQueue, copyQueue);
    }

    for (const auto& layout : description.bindingLayouts)
        addOperation(pkg::D3D12OperationCode::CreateRootSignature, noQueue,
                     pkg::Encode(pkg::CreateRootSignaturePayload{layout.id}));
    for (const auto& executable : description.executables)
        addOperation(pkg::D3D12OperationCode::CreateGraphicsPipeline, noQueue,
                     pkg::Encode(pkg::CreateGraphicsPipelinePayload{executable.id}));
    for (const auto& executable : description.computeExecutables)
        addOperation(pkg::D3D12OperationCode::CreateComputePipeline, noQueue,
                     pkg::Encode(pkg::CreateComputePipelinePayload{executable.id}));

    const auto loadCount = static_cast<std::uint32_t>(description.operations.size());
    for (const auto& slot : description.dynamicSlots)
        addOperation(pkg::D3D12OperationCode::ApplyDynamicData, noQueue,
                     pkg::Encode(pkg::ApplyDynamicDataPayload{slot.id}));
    for (const auto& slot : description.externalSlots)
        addOperation(pkg::D3D12OperationCode::AcquireExternal, noQueue,
                     pkg::Encode(pkg::AcquireExternalPayload{slot.id}));
    for (const auto& slot : description.surfaceSlots)
        addOperation(pkg::D3D12OperationCode::AcquireSurfaceImage, noQueue,
                     pkg::Encode(pkg::AcquireSurfaceImagePayload{slot.id}));

    std::map<std::uint32_t, std::uint32_t> workPosition;
    for (std::uint32_t index = 0; index < analyzed.canonicalWorkOrder.size(); ++index)
        workPosition[analyzed.canonicalWorkOrder[index].value] = index;
    std::map<std::uint32_t, std::uint32_t> firstExternalUse;
    for (std::uint32_t position = 0; position < analyzed.canonicalWorkOrder.size(); ++position)
    {
        const auto& work = *works.at(analyzed.canonicalWorkOrder[position].value);
        for (const auto useId : work.uses)
        {
            const auto* use = uses.at(useId.value);
            if (externalSlots.contains(use->resource.value) && !firstExternalUse.contains(use->resource.value))
                firstExternalUse[use->resource.value] = position;
        }
    }

    const auto cellFor = [&](const semantic::ResourceUse& use) {
        const auto& source = *resources.at(use.resource.value);
        std::uint32_t relation = 0;
        if (source.lifetime == semantic::LifetimeIntent::Temporal)
            relation = use.role == semantic::ViewRole::TemporalPreviousBuffer ? 2u : 1u;
        return StateCell{resourceMap.at(use.resource.value).value, relation};
    };
    const auto baselineState = [&](StateCell cell) {
        const auto& artifact = description.resources[cell.resource];
        if (artifact.initialDataSize != 0) return Common();
        return artifact.initialState;
    };
    std::map<StateCell, pkg::ResourceState> states;
    const auto currentState = [&](StateCell cell) {
        const auto found = states.find(cell);
        return found == states.end() ? baselineState(cell) : found->second;
    };
    const auto emitTransition = [&](pkg::QueueId queue, pkg::ViewId view, StateCell cell,
                                    pkg::ResourceState after)
    {
        const auto before = currentState(cell);
        if (before == after) return;
        addOperation(pkg::D3D12OperationCode::Transition, queue,
            pkg::Encode(pkg::TransitionPayload{view, 0, before, after}));
        states[cell] = after;
    };

    for (std::uint32_t position = 0; position < analyzed.canonicalWorkOrder.size(); ++position)
    {
        const auto workId = analyzed.canonicalWorkOrder[position];
        const auto& work = *works.at(workId.value);
        const auto queue = workQueues.at(work.id.value);

        std::set<std::uint32_t> waitedQueues;
        for (const auto& edge : analyzed.dependencies)
        {
            if (edge.consumer != work.id) continue;
            const auto producerQueue = workQueues.at(edge.producer.value);
            if (producerQueue != queue && waitedQueues.insert(producerQueue.value).second)
                addOperation(pkg::D3D12OperationCode::WaitQueue, queue,
                             pkg::Encode(pkg::WaitQueuePayload{producerQueue}));
        }
        for (const auto useId : work.uses)
        {
            const auto* use = uses.at(useId.value);
            const auto first = firstExternalUse.find(use->resource.value);
            if (first != firstExternalUse.end() && first->second == position)
                addOperation(pkg::D3D12OperationCode::WaitExternal, queue,
                             pkg::Encode(pkg::WaitExternalPayload{externalSlots.at(use->resource.value)}));
            if (use->role == semantic::ViewRole::TemporalPreviousBuffer)
                addOperation(pkg::D3D12OperationCode::WaitTemporal, queue,
                             pkg::Encode(pkg::WaitTemporalPayload{resourceMap.at(use->resource.value)}));
        }

        addOperation(pkg::D3D12OperationCode::BeginQueueBatch, queue);
        std::map<StateCell, const semantic::ResourceUse*> activeUses;
        std::set<std::uint32_t> rasterPresentationResources;
        if (work.kind == semantic::WorkKind::Raster)
            for (const auto useId : work.uses)
            {
                const auto* use = uses.at(useId.value);
                if (use->role == semantic::ViewRole::PresentSource)
                    rasterPresentationResources.insert(use->resource.value);
            }

        for (const auto useId : work.uses)
        {
            const auto* use = uses.at(useId.value);
            if (work.kind == semantic::WorkKind::Raster && use->role == semantic::ViewRole::PresentSource)
                continue;
            const auto cell = cellFor(*use);
            const auto required = RequiredState(use->role);
            const auto existing = activeUses.find(cell);
            if (existing != activeUses.end() && RequiredState(existing->second->role) != required)
                return Failure<CompileOutput>("state-planning", "one Work requires incompatible states for the same state cell");
            activeUses[cell] = use;
            emitTransition(queue, viewMap.at(use->id.value), cell, required);
        }

        if (work.kind == semantic::WorkKind::Copy)
        {
            const auto* sourceUse = uses.at(work.copy.source.value);
            const auto* destinationUse = uses.at(work.copy.destination.value);
            addOperation(pkg::D3D12OperationCode::ExecuteCopy, queue,
                pkg::Encode(pkg::CopyBufferPayload{resourceMap.at(sourceUse->resource.value),
                    resourceMap.at(destinationUse->resource.value), 0, 0, work.copy.bytes}));
        }
        else if (work.kind == semantic::WorkKind::Compute)
        {
            addOperation(pkg::D3D12OperationCode::ExecuteCompute, queue,
                pkg::Encode(pkg::ExecuteComputePayload{workArtifacts.at(work.id.value).computeCommand}));
        }
        else if (work.kind == semantic::WorkKind::Raster)
        {
            addOperation(pkg::D3D12OperationCode::ExecuteRaster, queue,
                pkg::Encode(pkg::ExecuteRasterPayload{workArtifacts.at(work.id.value).rasterCommand}));
            for (const auto useId : work.uses)
            {
                const auto* use = uses.at(useId.value);
                if (use->role != semantic::ViewRole::PresentSource) continue;
                const auto cell = cellFor(*use);
                emitTransition(queue, viewMap.at(use->id.value), cell, Present());
                activeUses[cell] = use;
            }
        }

        for (const auto& [cell, use] : activeUses)
        {
            if (rasterPresentationResources.contains(use->resource.value)) continue;
            pkg::ResourceState desired = baselineState(cell);
            bool nextFound = false;
            for (std::uint32_t next = position + 1; next < analyzed.canonicalWorkOrder.size() && !nextFound; ++next)
            {
                const auto& nextWork = *works.at(analyzed.canonicalWorkOrder[next].value);
                for (const auto nextUseId : nextWork.uses)
                {
                    const auto* nextUse = uses.at(nextUseId.value);
                    if (cellFor(*nextUse) != cell) continue;
                    desired = workQueues.at(nextWork.id.value) == queue ? RequiredState(nextUse->role) : Common();
                    nextFound = true;
                    break;
                }
            }
            if (!nextFound && externalSlots.contains(use->resource.value))
                desired = description.externalSlots[externalSlots.at(use->resource.value).value].guaranteedOutgoingState;
            emitTransition(queue, viewMap.at(use->id.value), cell, desired);
        }
        addOperation(pkg::D3D12OperationCode::EndQueueBatch, queue);
        addOperation(pkg::D3D12OperationCode::SignalQueue, queue);
    }

    for (const auto& slot : description.surfaceSlots)
        addOperation(pkg::D3D12OperationCode::PresentSurface, noQueue,
                     pkg::Encode(pkg::PresentSurfacePayload{slot.id}));
    if (!description.surfaceSlots.empty())
        addOperation(pkg::D3D12OperationCode::SignalQueue, directQueue);
    for (const auto& slot : description.externalSlots)
        addOperation(pkg::D3D12OperationCode::ReleaseExternal, noQueue,
                     pkg::Encode(pkg::ReleaseExternalPayload{slot.id}));

    description.operationStreams.push_back({pkg::OperationStreamKind::Load, 0, loadCount, 0});
    description.operationStreams.push_back({pkg::OperationStreamKind::Frame, loadCount,
        static_cast<std::uint32_t>(description.operations.size()) - loadCount, 0});

    auto packageBytes = pkg::BuildFrozenPackage(description);
    if (!packageBytes)
        return Failure<CompileOutput>("package-serialization", packageBytes.Error().message);
    auto verified = package::PackageReader::Read(packageBytes.Value());
    if (!verified)
        return Failure<CompileOutput>("package-validation", verified.Error().message);
    auto decoded = pkg::D3D12PackageView::Decode(verified.Value());
    if (!decoded)
        return Failure<CompileOutput>("package-schema-validation", decoded.Error().message);

    CompileOutput output;
    output.executionDigestHex = base::ToHex(verified.Value().ExecutionDigest());
    output.packageBytes = std::move(packageBytes).Value();
    return base::Result<CompileOutput, CompileError>::Success(std::move(output));
}
}
