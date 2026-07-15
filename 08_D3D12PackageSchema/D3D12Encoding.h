#pragma once

#include "../00_Base/Result.h"
#include "../07_FrozenPackageCore/FrozenExecutablePackage.h"
#include "D3D12Schema.h"

#include <string>

namespace sge::package::d3d12_v13
{
[[nodiscard]] base::Result<std::vector<std::byte>, PackageError> BuildFrozenPackage(
    const D3D12PackageDescription& description);

class D3D12PackageView final
{
public:
    [[nodiscard]] static base::Result<D3D12PackageView, PackageError> Decode(
        const FrozenExecutablePackage& package);

    [[nodiscard]] const TargetProfile& Profile() const noexcept { return profile_; }
    [[nodiscard]] const PackageManifest& Manifest() const noexcept { return manifest_; }
    [[nodiscard]] std::span<const ResourceArtifact> Resources() const noexcept { return resources_; }
    [[nodiscard]] std::span<const AllocationArtifact> Allocations() const noexcept { return allocations_; }
    [[nodiscard]] std::span<const ResourceViewArtifact> Views() const noexcept { return views_; }
    [[nodiscard]] std::span<const ShaderArtifact> Shaders() const noexcept { return shaders_; }
    [[nodiscard]] std::span<const ProgramArtifact> Programs() const noexcept { return programs_; }
    [[nodiscard]] std::span<const BindingLayoutArtifact> BindingLayouts() const noexcept { return bindingLayouts_; }
    [[nodiscard]] std::span<const RootParameterArtifact> RootParameters() const noexcept { return rootParameters_; }
    [[nodiscard]] std::span<const VertexElementArtifact> VertexElements() const noexcept { return vertexElements_; }
    [[nodiscard]] std::span<const RasterExecutableArtifact> Executables() const noexcept { return executables_; }
    [[nodiscard]] std::span<const ComputeExecutableArtifact> ComputeExecutables() const noexcept { return computeExecutables_; }
    [[nodiscard]] std::span<const ComputeCommandArtifact> ComputeCommands() const noexcept { return computeCommands_; }
    [[nodiscard]] std::span<const AttachmentOperationArtifact> AttachmentOperations() const noexcept { return attachmentOperations_; }
    [[nodiscard]] std::span<const RasterCommandArtifact> RasterCommands() const noexcept { return rasterCommands_; }
    [[nodiscard]] std::span<const DynamicDataSlotArtifact> DynamicSlots() const noexcept { return dynamicSlots_; }
    [[nodiscard]] std::span<const ExternalResourceSlotArtifact> ExternalSlots() const noexcept { return externalSlots_; }
    [[nodiscard]] std::span<const SurfaceSlotArtifact> SurfaceSlots() const noexcept { return surfaceSlots_; }
    [[nodiscard]] std::span<const OperationView> LoadOperations() const noexcept { return loadOperations_; }
    [[nodiscard]] std::span<const OperationView> FrameOperations() const noexcept { return frameOperations_; }
    [[nodiscard]] std::span<const std::byte> InitialData() const noexcept { return initialData_; }
    [[nodiscard]] std::span<const std::byte> ShaderData() const noexcept { return shaderData_; }
    [[nodiscard]] std::span<const std::byte> NativeObjectData() const noexcept { return nativeObjectData_; }
    [[nodiscard]] base::Result<std::span<const std::byte>, PackageError> ResolveBlob(const BlobRef& blob) const;

private:
    const FrozenExecutablePackage* package_ = nullptr;
    TargetProfile profile_;
    PackageManifest manifest_;
    std::vector<ResourceArtifact> resources_;
    std::vector<AllocationArtifact> allocations_;
    std::vector<ResourceViewArtifact> views_;
    std::vector<ShaderArtifact> shaders_;
    std::vector<ProgramArtifact> programs_;
    std::vector<BindingLayoutArtifact> bindingLayouts_;
    std::vector<RootParameterArtifact> rootParameters_;
    std::vector<VertexElementArtifact> vertexElements_;
    std::vector<RasterExecutableArtifact> executables_;
    std::vector<ComputeExecutableArtifact> computeExecutables_;
    std::vector<ComputeCommandArtifact> computeCommands_;
    std::vector<AttachmentOperationArtifact> attachmentOperations_;
    std::vector<RasterCommandArtifact> rasterCommands_;
    std::vector<DynamicDataSlotArtifact> dynamicSlots_;
    std::vector<ExternalResourceSlotArtifact> externalSlots_;
    std::vector<SurfaceSlotArtifact> surfaceSlots_;
    std::vector<OperationView> loadOperations_;
    std::vector<OperationView> frameOperations_;
    std::span<const std::byte> initialData_;
    std::span<const std::byte> shaderData_;
    std::span<const std::byte> nativeObjectData_;
};

[[nodiscard]] std::vector<std::byte> Encode(const CreateResourcePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const UploadBufferPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const UploadTexturePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const CreateRootSignaturePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const CreateGraphicsPipelinePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const CreateComputePipelinePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const InitializeStatePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const VerifyBufferContentsPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const VerifyTextureContentsPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const AcquireSurfaceImagePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const AcquireExternalPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const WaitExternalPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const ApplyDynamicDataPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const TransitionPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const ExecuteRasterPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const ExecuteComputePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const CopyBufferPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const SignalQueuePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const WaitQueuePayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const WaitTemporalPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const ActivateAliasPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const ReleaseExternalPayload& payload);
[[nodiscard]] std::vector<std::byte> Encode(const PresentSurfacePayload& payload);

[[nodiscard]] base::Result<CreateResourcePayload, PackageError> DecodeCreateResource(std::span<const std::byte> payload);
[[nodiscard]] base::Result<UploadBufferPayload, PackageError> DecodeUploadBuffer(std::span<const std::byte> payload);
[[nodiscard]] base::Result<UploadTexturePayload, PackageError> DecodeUploadTexture(std::span<const std::byte> payload);
[[nodiscard]] base::Result<CreateRootSignaturePayload, PackageError> DecodeCreateRootSignature(std::span<const std::byte> payload);
[[nodiscard]] base::Result<CreateGraphicsPipelinePayload, PackageError> DecodeCreateGraphicsPipeline(std::span<const std::byte> payload);
[[nodiscard]] base::Result<CreateComputePipelinePayload, PackageError> DecodeCreateComputePipeline(std::span<const std::byte> payload);
[[nodiscard]] base::Result<InitializeStatePayload, PackageError> DecodeInitializeState(std::span<const std::byte> payload);
[[nodiscard]] base::Result<VerifyBufferContentsPayload, PackageError> DecodeVerifyBufferContents(std::span<const std::byte> payload);
[[nodiscard]] base::Result<VerifyTextureContentsPayload, PackageError> DecodeVerifyTextureContents(std::span<const std::byte> payload);
[[nodiscard]] base::Result<AcquireSurfaceImagePayload, PackageError> DecodeAcquireSurfaceImage(std::span<const std::byte> payload);
[[nodiscard]] base::Result<AcquireExternalPayload, PackageError> DecodeAcquireExternal(std::span<const std::byte> payload);
[[nodiscard]] base::Result<WaitExternalPayload, PackageError> DecodeWaitExternal(std::span<const std::byte> payload);
[[nodiscard]] base::Result<ApplyDynamicDataPayload, PackageError> DecodeApplyDynamicData(std::span<const std::byte> payload);
[[nodiscard]] base::Result<TransitionPayload, PackageError> DecodeTransition(std::span<const std::byte> payload);
[[nodiscard]] base::Result<ExecuteRasterPayload, PackageError> DecodeExecuteRaster(std::span<const std::byte> payload);
[[nodiscard]] base::Result<ExecuteComputePayload, PackageError> DecodeExecuteCompute(std::span<const std::byte> payload);
[[nodiscard]] base::Result<CopyBufferPayload, PackageError> DecodeCopyBuffer(std::span<const std::byte> payload);
[[nodiscard]] base::Result<SignalQueuePayload, PackageError> DecodeSignalQueue(std::span<const std::byte> payload);
[[nodiscard]] base::Result<WaitQueuePayload, PackageError> DecodeWaitQueue(std::span<const std::byte> payload);
[[nodiscard]] base::Result<WaitTemporalPayload, PackageError> DecodeWaitTemporal(std::span<const std::byte> payload);
[[nodiscard]] base::Result<ActivateAliasPayload, PackageError> DecodeActivateAlias(std::span<const std::byte> payload);
[[nodiscard]] base::Result<ReleaseExternalPayload, PackageError> DecodeReleaseExternal(std::span<const std::byte> payload);
[[nodiscard]] base::Result<PresentSurfacePayload, PackageError> DecodePresentSurface(std::span<const std::byte> payload);
}
