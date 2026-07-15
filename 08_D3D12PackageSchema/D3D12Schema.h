#pragma once

#include "../00_Base/Sha256.h"
#include "../00_Base/StrongId.h"
#include "../07_FrozenPackageCore/PackageFormat.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sge::package::d3d12_v13
{
template<class Tag> using Id32 = base::Id32<Tag>;
struct ResourceTag; struct AllocationTag; struct ViewTag; struct ShaderTag; struct ProgramTag;
struct BindingLayoutTag; struct ExecutableTag; struct RasterCommandTag; struct DynamicSlotTag;
struct ExternalSlotTag; struct SurfaceSlotTag; struct QueueTag; struct AttachmentOperationTag;
struct RootParameterTag; struct ComputeExecutableTag; struct ComputeCommandTag;
using ResourceId = Id32<ResourceTag>;
using AllocationId = Id32<AllocationTag>;
using ViewId = Id32<ViewTag>;
using ShaderId = Id32<ShaderTag>;
using ProgramId = Id32<ProgramTag>;
using BindingLayoutId = Id32<BindingLayoutTag>;
using ExecutableId = Id32<ExecutableTag>;
using RasterCommandId = Id32<RasterCommandTag>;
using DynamicSlotId = Id32<DynamicSlotTag>;
using ExternalSlotId = Id32<ExternalSlotTag>;
using SurfaceSlotId = Id32<SurfaceSlotTag>;
using QueueId = Id32<QueueTag>;
using AttachmentOperationId = Id32<AttachmentOperationTag>;
using RootParameterId = Id32<RootParameterTag>;
using ComputeExecutableId = Id32<ComputeExecutableTag>;
using ComputeCommandId = Id32<ComputeCommandTag>;

struct IndexRange final { std::uint32_t first = 0; std::uint32_t count = 0; };
struct BlobRef final
{
    SectionKind section{};
    std::uint32_t reserved = 0;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

enum class BarrierModel : std::uint32_t { Legacy = 1, Enhanced = 2 };
enum class ShaderBinaryFormat : std::uint16_t { Dxbc = 1, Dxil = 2 };
enum class ResourceKind : std::uint16_t { Buffer = 1, Texture1D = 2, Texture2D = 3, Texture3D = 4, SurfaceImage = 5 };
enum class ResourceOrigin : std::uint16_t { PackageOwned = 1, External = 2, Surface = 3 };
enum class ResourceFlags : std::uint32_t { None = 0, FrameLocal = 1u << 0, Temporal = 1u << 1, Aliased = 1u << 2 };
enum class ResourceViewFlags : std::uint32_t { None = 0, TemporalCurrent = 1u << 0, TemporalPrevious = 1u << 1 };
enum class RebuildPolicy : std::uint16_t { RecreateFromPackage = 1, RequireExternalRebind = 2, RuntimeManaged = 3 };
enum class ExtentMode : std::uint16_t { Fixed = 1, SurfaceRelative = 2 };
enum class StateClass : std::uint16_t { Common = 1, Present = 2, Explicit = 3 };
enum class ExplicitStateBits : std::uint32_t
{
    None = 0,
    VertexBuffer = 1u << 0,
    ConstantBuffer = 1u << 1,
    IndexBuffer = 1u << 2,
    RenderTarget = 1u << 3,
    DepthWrite = 1u << 4,
    DepthRead = 1u << 5,
    ShaderRead = 1u << 6,
    UnorderedWrite = 1u << 7,
    CopySource = 1u << 8,
    CopyDestination = 1u << 9,
    IndirectArgument = 1u << 10
};
enum class ViewClass : std::uint16_t
{
    VertexBuffer = 1, IndexBuffer = 2, ConstantBuffer = 3, ShaderResource = 4,
    UnorderedAccess = 5, RenderTarget = 6, DepthStencil = 7, CopySource = 8,
    CopyDestination = 9, PresentSource = 10
};
enum class AllocationKind : std::uint16_t { Committed = 1, Placed = 2, RuntimeManaged = 3 };
enum class HeapClass : std::uint16_t { DefaultBuffer = 1, DefaultTexture = 2, RenderTargetOrDepth = 3, Upload = 4, Readback = 5 };
enum class ShaderStage : std::uint16_t { Vertex = 1, Pixel = 2, Compute = 3 };
enum class ProgramKind : std::uint16_t { Raster = 1, Compute = 2 };
enum class OperationStreamKind : std::uint32_t { Load = 1, Frame = 2 };
enum class Format : std::uint32_t { Unknown = 0, R32G32Float = 1, R32G32B32Float = 2, R32G32B32A32Float = 3, B8G8R8A8Unorm = 4, D32Float = 5 };
enum class PrimitiveTopology : std::uint32_t { TriangleList = 1 };
enum class PrimitiveTopologyType : std::uint32_t { Triangle = 1 };
enum class VertexMeaning : std::uint16_t { Position = 1, Color = 2, TexCoord = 3 };
enum class AttachmentLoadOp : std::uint16_t { Preserve = 1, Clear = 2, Discard = 3 };
enum class AttachmentStoreOp : std::uint16_t { Store = 1, Discard = 2 };
enum class RootParameterKind : std::uint16_t { ConstantBuffer = 1, ShaderResourceTable = 2, UnorderedAccessTable = 3 };
enum class ShaderVisibility : std::uint16_t { All = 1, Vertex = 2, Pixel = 3, Compute = 4 };
enum class ExternalSynchronizationContract : std::uint32_t { CompletionTokenRequired = 1 };
enum class ExternalSlotFlags : std::uint32_t { Required = 1u << 0 };

enum class D3D12OperationCode : std::uint32_t
{
    CreateDescriptorHeaps = 0x0002'0001,
    CreateResource = 0x0002'0002,
    UploadBuffer = 0x0002'0003,
    CreateRootSignature = 0x0002'0004,
    CreateGraphicsPipeline = 0x0002'0005,
    InitializeState = 0x0002'0006,
    VerifyBufferContents = 0x0002'0007,
    UploadTexture = 0x0002'0008,
    VerifyTextureContents = 0x0002'0009,
    CreateComputePipeline = 0x0002'000A,
    AcquireSurfaceImage = 0x0002'0100,
    AcquireExternal = 0x0002'0101,
    WaitExternal = 0x0002'0102,
    ApplyDynamicData = 0x0002'0103,
    BeginQueueBatch = 0x0002'0200,
    Transition = 0x0002'0201,
    ActivateAlias = 0x0002'0202,
    ExecuteRaster = 0x0002'0203,
    ExecuteCompute = 0x0002'0204,
    ExecuteCopy = 0x0002'0205,
    EndQueueBatch = 0x0002'0206,
    SignalQueue = 0x0002'0207,
    WaitQueue = 0x0002'0208,
    WaitTemporal = 0x0002'0209,
    ReleaseExternal = 0x0002'0300,
    PresentSurface = 0x0002'0301
};

struct ResourceState final
{
    StateClass stateClass = StateClass::Common;
    std::uint16_t reserved = 0;
    std::uint32_t explicitBits = 0;
    auto operator<=>(const ResourceState&) const = default;
};

struct TargetProfile final
{
    std::uint32_t minimumFeatureLevel = 0;
    std::uint16_t shaderModelMajor = 0;
    std::uint16_t shaderModelMinor = 0;
    std::uint16_t rootSignatureMajor = 0;
    std::uint16_t rootSignatureMinor = 0;
    BarrierModel barrierModel = BarrierModel::Legacy;
    ShaderBinaryFormat shaderBinaryFormat = ShaderBinaryFormat::Dxbc;
    std::uint32_t framesInFlight = 2;
    std::uint32_t directQueueCount = 1;
    std::uint32_t computeQueueCount = 0;
    std::uint32_t copyQueueCount = 0;
    std::uint32_t surfaceImageCount = 2;
    std::uint32_t rtvDescriptorCount = 0;
    std::uint32_t dsvDescriptorCount = 0;
    std::uint32_t shaderDescriptorCount = 0;
    std::uint32_t samplerDescriptorCount = 0;
    std::uint64_t requiredFeatureBits0 = 0;
    std::uint64_t requiredFeatureBits1 = 0;
};

struct PackageManifest final
{
    std::uint32_t resourceCount = 0;
    std::uint32_t allocationCount = 0;
    std::uint32_t viewCount = 0;
    std::uint32_t shaderCount = 0;
    std::uint32_t programCount = 0;
    std::uint32_t bindingLayoutCount = 0;
    std::uint32_t executableCount = 0;
    std::uint32_t rasterCommandCount = 0;
    std::uint32_t computeExecutableCount = 0;
    std::uint32_t computeCommandCount = 0;
    std::uint32_t vertexElementCount = 0;
    std::uint32_t attachmentOperationCount = 0;
    std::uint32_t dynamicSlotCount = 0;
    std::uint32_t externalSlotCount = 0;
    std::uint32_t surfaceSlotCount = 0;
    std::uint32_t loadOperationStream = InvalidIndex;
    std::uint32_t frameOperationStream = InvalidIndex;
    std::uint32_t flags = 0;
};

struct ResourceArtifact final
{
    ResourceId id;
    ResourceKind resourceKind = ResourceKind::Buffer;
    ResourceOrigin origin = ResourceOrigin::PackageOwned;
    RebuildPolicy rebuildPolicy = RebuildPolicy::RecreateFromPackage;
    ExtentMode extentMode = ExtentMode::Fixed;
    std::uint32_t flags = 0;
    std::uint32_t physicalInstanceCount = 1;
    AllocationId allocation;
    Format format = Format::Unknown;
    std::uint32_t usageFlags = 0;
    ResourceState initialState;
    std::uint64_t sizeBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t depthOrArraySize = 0;
    std::uint16_t mipLevels = 0;
    std::uint16_t sampleCount = 1;
    std::uint16_t planeCount = 1;
    std::uint64_t initialDataOffset = 0;
    std::uint64_t initialDataSize = 0;
    std::uint32_t firstView = 0;
    std::uint32_t viewCount = 0;
};

struct AllocationArtifact final
{
    AllocationId id;
    AllocationKind kind = AllocationKind::Committed;
    HeapClass heapClass = HeapClass::DefaultBuffer;
    std::uint32_t flags = 0;
    std::uint32_t physicalInstanceCount = 1;
    std::uint64_t sizeBytes = 0;
    std::uint64_t alignment = 0;
    std::uint32_t aliasGroup = InvalidIndex;
};

struct ResourceViewArtifact final
{
    ViewId id;
    ResourceId resource;
    ViewClass viewClass = ViewClass::VertexBuffer;
    std::uint16_t reserved = 0;
    Format format = Format::Unknown;
    std::uint32_t flags = 0;
    std::uint64_t byteOffset = 0;
    std::uint64_t byteSize = 0;
    std::uint32_t strideBytes = 0;
    std::uint16_t firstMip = 0;
    std::uint16_t mipCount = 0;
    std::uint16_t firstArrayLayer = 0;
    std::uint16_t arrayLayerCount = 0;
    std::uint16_t firstPlane = 0;
    std::uint16_t planeCount = 0;
    std::uint32_t descriptorHeapClass = 0;
    std::uint32_t descriptorIndex = InvalidIndex;
    std::uint32_t descriptorInstanceStride = 0;
};

struct ShaderArtifact final
{
    ShaderId id;
    ShaderStage stage = ShaderStage::Vertex;
    ShaderBinaryFormat format = ShaderBinaryFormat::Dxbc;
    std::uint16_t shaderModelMajor = 5;
    std::uint16_t shaderModelMinor = 1;
    std::uint32_t flags = 0;
    BlobRef bytecode;
    base::Digest256 bytecodeDigest{};
};

struct BindingLayoutArtifact final
{
    BindingLayoutId id;
    std::uint16_t rootSignatureMajor = 1;
    std::uint16_t rootSignatureMinor = 0;
    std::uint32_t flags = 0;
    IndexRange parameterRange;
    IndexRange descriptorRange;
    IndexRange staticSamplerRange;
    BlobRef serializedRootSignature;
    base::Digest256 layoutDigest{};
};

struct RootParameterArtifact final
{
    RootParameterId id;
    RootParameterKind kind = RootParameterKind::ConstantBuffer;
    ShaderVisibility visibility = ShaderVisibility::Vertex;
    std::uint32_t rootParameterIndex = 0;
    std::uint32_t shaderRegister = 0;
    std::uint32_t registerSpace = 0;
    DynamicSlotId dynamicSlot;
    ViewId staticView;
    std::uint32_t flags = 0;
};

struct ProgramArtifact final
{
    ProgramId id;
    ProgramKind kind = ProgramKind::Raster;
    std::uint16_t flags = 0;
    ShaderId vertexShader;
    ShaderId pixelShader;
    ShaderId computeShader;
    BindingLayoutId bindingLayout;
    base::Digest256 interfaceDigest{};
};

struct VertexElementArtifact final
{
    std::uint32_t id = InvalidIndex;
    VertexMeaning meaning = VertexMeaning::Position;
    std::uint16_t semanticIndex = 0;
    Format format = Format::Unknown;
    std::uint32_t inputSlot = 0;
    std::uint32_t alignedByteOffset = 0;
    std::uint32_t instanceStepRate = 0;
    std::uint32_t flags = 0;
};

struct RasterExecutableArtifact final
{
    ExecutableId id;
    ProgramId program;
    BindingLayoutId bindingLayout;
    IndexRange vertexElementRange;
    IndexRange colorFormatRange;
    Format colorFormat = Format::Unknown;
    Format depthFormat = Format::Unknown;
    PrimitiveTopology primitiveTopology = PrimitiveTopology::TriangleList;
    PrimitiveTopologyType primitiveTopologyType = PrimitiveTopologyType::Triangle;
    std::uint32_t rasterStateId = 0;
    std::uint32_t blendStateId = 0;
    std::uint32_t depthStateId = 0;
    std::uint32_t sampleCount = 1;
    std::uint32_t sampleQuality = 0;
    base::Digest256 specializationDigest{};
};

struct ComputeExecutableArtifact final
{
    ComputeExecutableId id;
    ProgramId program;
    BindingLayoutId bindingLayout;
    std::uint32_t flags = 0;
    base::Digest256 specializationDigest{};
};

struct ComputeCommandArtifact final
{
    ComputeCommandId id;
    ComputeExecutableId executable;
    std::uint32_t threadGroupCountX = 1;
    std::uint32_t threadGroupCountY = 1;
    std::uint32_t threadGroupCountZ = 1;
    std::uint32_t flags = 0;
};

struct AttachmentOperationArtifact final
{
    AttachmentOperationId id;
    AttachmentLoadOp colorLoad = AttachmentLoadOp::Clear;
    AttachmentStoreOp colorStore = AttachmentStoreOp::Store;
    std::array<float, 4> clearColor{0.04f, 0.06f, 0.1f, 1.0f};
    AttachmentLoadOp depthLoad = AttachmentLoadOp::Clear;
    AttachmentStoreOp depthStore = AttachmentStoreOp::Store;
    float clearDepth = 1.0f;
    std::uint32_t clearStencil = 0;
};

struct RasterCommandArtifact final
{
    RasterCommandId id;
    ExecutableId executable;
    IndexRange vertexViewRange;
    ViewId indexView;
    IndexRange colorAttachmentRange;
    ViewId depthAttachment;
    std::uint32_t vertexCount = 0;
    std::uint32_t instanceCount = 1;
    std::uint32_t firstVertex = 0;
    std::uint32_t firstInstance = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t baseVertex = 0;
    std::uint32_t viewportId = 0;
    std::uint32_t scissorId = 0;
    AttachmentOperationId attachmentOperation;
};

struct DynamicDataSlotArtifact final
{
    DynamicSlotId id;
    ResourceId destinationResource;
    std::uint64_t destinationOffset = 0;
    std::uint64_t requiredBytes = 0;
    std::uint32_t requiredAlignment = 1;
    std::uint32_t flags = 0;
};

struct ExternalResourceSlotArtifact final
{
    ExternalSlotId id;
    ResourceId resource;
    ResourceKind requiredKind = ResourceKind::Buffer;
    Format requiredFormat = Format::Unknown;
    std::uint64_t minimumBytes = 0;
    ResourceState requiredIncomingState;
    ResourceState guaranteedOutgoingState;
    ExternalSynchronizationContract synchronizationContract = ExternalSynchronizationContract::CompletionTokenRequired;
    std::uint32_t flags = static_cast<std::uint32_t>(ExternalSlotFlags::Required);
};

struct SurfaceSlotArtifact final
{
    SurfaceSlotId id;
    ResourceId imageResource;
    Format requiredFormat = Format::Unknown;
    ResourceState acquiredState;
    ResourceState presentedState;
    std::uint32_t flags = 0;
};

struct OperationStreamArtifact final
{
    OperationStreamKind kind = OperationStreamKind::Load;
    std::uint32_t firstOperation = 0;
    std::uint32_t operationCount = 0;
    std::uint32_t flags = 0;
};

struct OperationArtifact final
{
    D3D12OperationCode opcode{};
    std::uint16_t operationVersion = 1;
    std::uint16_t flags = 0;
    QueueId queue;
    std::vector<std::byte> payload;
};

struct OperationView final
{
    D3D12OperationCode opcode{};
    std::uint16_t operationVersion = 1;
    std::uint16_t flags = 0;
    QueueId queue;
    std::span<const std::byte> payload;
};

struct D3D12PackageDescription final
{
    TargetProfile profile;
    std::vector<ResourceArtifact> resources;
    std::vector<AllocationArtifact> allocations;
    std::vector<ResourceViewArtifact> views;
    std::vector<ShaderArtifact> shaders;
    std::vector<ProgramArtifact> programs;
    std::vector<BindingLayoutArtifact> bindingLayouts;
    std::vector<RootParameterArtifact> rootParameters;
    std::vector<VertexElementArtifact> vertexElements;
    std::vector<RasterExecutableArtifact> executables;
    std::vector<ComputeExecutableArtifact> computeExecutables;
    std::vector<ComputeCommandArtifact> computeCommands;
    std::vector<AttachmentOperationArtifact> attachmentOperations;
    std::vector<RasterCommandArtifact> rasterCommands;
    std::vector<DynamicDataSlotArtifact> dynamicSlots;
    std::vector<ExternalResourceSlotArtifact> externalSlots;
    std::vector<SurfaceSlotArtifact> surfaceSlots;
    std::vector<OperationStreamArtifact> operationStreams;
    std::vector<OperationArtifact> operations;
    std::vector<std::byte> initialData;
    std::vector<std::byte> shaderData;
    std::vector<std::byte> nativeObjectData;
};

struct CreateResourcePayload final { ResourceId resource; };
struct UploadBufferPayload final { ResourceId resource; std::uint64_t sourceOffset = 0; std::uint64_t bytes = 0; };
struct UploadTexturePayload final
{
    ResourceId resource;
    std::uint64_t sourceOffset = 0;
    std::uint32_t sourceRowBytes = 0;
    std::uint32_t sourceSliceBytes = 0;
    std::uint16_t mipLevel = 0;
    std::uint16_t arrayLayer = 0;
    std::uint16_t plane = 0;
    std::uint16_t reserved = 0;
};
struct CreateRootSignaturePayload final { BindingLayoutId layout; };
struct CreateGraphicsPipelinePayload final { ExecutableId executable; };
struct CreateComputePipelinePayload final { ComputeExecutableId executable; };
struct InitializeStatePayload final { ResourceId resource; ResourceState before; ResourceState after; };
struct VerifyBufferContentsPayload final
{
    ResourceId resource;
    std::uint64_t resourceOffset = 0;
    std::uint64_t expectedDataOffset = 0;
    std::uint64_t bytes = 0;
};
struct VerifyTextureContentsPayload final
{
    ResourceId resource;
    std::uint64_t expectedDataOffset = 0;
    std::uint32_t expectedRowBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t reserved = 0;
};
struct AcquireSurfaceImagePayload final { SurfaceSlotId slot; };
struct AcquireExternalPayload final { ExternalSlotId slot; };
struct WaitExternalPayload final { ExternalSlotId slot; };
struct ApplyDynamicDataPayload final { DynamicSlotId slot; };
struct TransitionPayload final { ViewId view; std::uint32_t flags = 0; ResourceState before; ResourceState after; };
struct ExecuteRasterPayload final { RasterCommandId command; };
struct ExecuteComputePayload final { ComputeCommandId command; };
struct CopyBufferPayload final
{
    ResourceId source;
    ResourceId destination;
    std::uint64_t sourceOffset = 0;
    std::uint64_t destinationOffset = 0;
    std::uint64_t bytes = 0;
};
struct WaitQueuePayload final { QueueId producerQueue; };
struct WaitTemporalPayload final { ResourceId resource; };
struct ActivateAliasPayload final { ResourceId before; ResourceId after; };
struct ReleaseExternalPayload final { ExternalSlotId slot; };
struct PresentSurfacePayload final { SurfaceSlotId slot; };
}
