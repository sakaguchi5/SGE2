# D3D12 Package Schema v17

## 1. Status and scope

本書はLevel 2 Finalのas-built D3D12 Target Package Schemaを記述する。

```text
Target kind: 1（D3D12）
Target schema: 17
Minimum Runtime: 17
Container: 1.0
```

歴史的理由によりC++ namespaceは`d3d12_v13`のままだが、wire contractはSchema 17である。namespace名を理由にLevel 2 frozen codeを変更しない。

As-built source:

```text
08_D3D12PackageSchema/D3D12Schema.h
08_D3D12PackageSchema/D3D12Encoding.cpp
```

## 2. Required sections and record strides

全PackageはLoad streamとFrame streamを各1個持つ。Schema 17 decoderは次をrequiredとして扱う。

| Section | Stride |
|---|---:|
| Manifest | 72 |
| D3D12TargetProfile | 80 |
| D3D12ResourceTable | 96 |
| D3D12AllocationTable | 40 |
| D3D12ViewTable | 72 |
| D3D12ShaderTable | 72 |
| D3D12ProgramTable | 56 |
| D3D12BindingLayoutTable | 104 |
| D3D12RootParameterTable | 40 |
| D3D12VertexElementTable | 32 |
| D3D12ExecutableTable | 104 |
| D3D12ComputeExecutableTable | 48 |
| D3D12ComputeCommandTable | 24 |
| D3D12AttachmentOperationTable | 48 |
| D3D12RasterCommandTable | 80 |
| D3D12DynamicSlotTable | 32 |
| D3D12ExternalSlotTable | 48 |
| D3D12SurfaceSlotTable | 32 |
| OperationStreamTable | 16 |
| OperationTable | 24 |

InvocationSchema、DescriptorPlan、OperationPayload、InitialData、ShaderData、NativeObjectDataはblob sectionである。

## 3. Stable identity rules

- Table identityは32-bit dense ID
- 各Table内で0から連続
- recordはID順
- InvalidIndexは`0xffffffff`
- Source IDとPackage IDは別identity
- SignalPointIdはQueue間およびTemporal/External release契約を参照

## 4. Target Profile

Fields:

- minimumFeatureLevel
- shader model major/minor
- root signature major/minor
- BarrierModel: Legacy / Enhanced
- ShaderBinaryFormat: DXBC / DXIL
- framesInFlight
- direct/compute/copy queue count
- surfaceImageCount
- RTV/DSV/shader/sampler descriptor count
- requiredFeatureBits0/1

Level 2 capabilityではDirect=1、Compute=0/1、Copy=0/1、Surface=0/1 profileを受理する。Profileと実際のSection/slot/descriptor planは一致しなければならない。

## 5. Main artifact tables

### ResourceArtifact

固定する情報:

- ResourceKind: Buffer / Texture1D / Texture2D / Texture3D / SurfaceImage
- ResourceOrigin: PackageOwned / External / Surface
- RebuildPolicy: RecreateFromPackage / RequireExternalRebind / RuntimeManaged
- ExtentMode: Fixed / SurfaceRelative
- ResourceFlags: FrameLocal / Temporal / Aliased
- physicalInstanceCount
- AllocationId
- Format、usage flags
- initial ResourceState
- Buffer sizeまたはTexture extent/mip/sample/plane
- initial data range
- owned View range

Level 2 accepted vocabularyはBuffer、Texture2D、SurfaceImageに限定される。Schema enumに存在してもLevel 2 Compilerが受理するとは限らない。

### AllocationArtifact

- AllocationKind: Committed / Placed / RuntimeManaged
- HeapClass
- physicalInstanceCount
- size / alignment
- aliasGroup

### ResourceViewArtifact

- ResourceId
- ViewClass
- Format
- TemporalCurrent / TemporalPrevious flags
- Buffer range/stride
- mip/array/plane range
- descriptor heap class/index/instance stride

Copy operationはResourceIdではなくexact ViewIdを参照し、Temporal generation等の物理instance意味を固定する。

### ShaderArtifact

- Shader stage
- binary format/model
- bytecode BlobRef
- bytecode digest

HLSL source、path、entry point、include pathはPackageに含めない。

### Program / Binding

ProgramArtifact:

- Raster / Compute
- shader IDs
- BindingLayoutId
- interface digest

BindingLayoutArtifact:

- root signature version
- parameter/descriptor/static-sampler ranges
- serialized Root Signature BlobRef
- layout digest

RootParameterArtifact:

- ConstantBuffer / ShaderResourceTable / UnorderedAccessTable
- visibility
- root parameter index
- shader register/space
- DynamicSlotIdまたはstatic ViewId

### Executable / Command

RasterExecutableArtifact:

- Program / BindingLayout
- VertexElement range
- color/depth format
- topology
- raster/blend/depth state identity
- sample settings
- specialization digest

ComputeExecutableArtifact:

- Program / BindingLayout
- specialization digest

RasterCommandArtifact:

- Executable
- vertex/index/color/depth View
- draw counts/offsets
- viewport/scissor identity
- AttachmentOperationId

ComputeCommandArtifact:

- ComputeExecutable
- thread group counts

### AttachmentOperationArtifact

- color/depth load/store
- clear color/depth/stencil

## 6. Resource state contract

StateClass:

- Common
- Present
- Explicit

Explicit bits:

- VertexBuffer
- ConstantBuffer
- IndexBuffer
- RenderTarget
- DepthWrite / DepthRead
- legacy ShaderRead
- UnorderedWrite
- CopySource / CopyDestination
- IndirectArgument
- PixelShaderRead / NonPixelShaderRead

Non-Explicit stateはexplicit bitsを持ってはならない。Explicit stateは既知bitを1個以上持つ。

Transition payloadはbefore/afterを両方保持する。Executorは現在stateを推測してTransitionを作ってはならない。

## 7. Invocation schema

### DynamicDataSlotArtifact

- destination ResourceId
- destination offset
- exact required bytes
- required alignment

### ExternalResourceSlotArtifact

- ResourceId
- required ResourceKind / Format
- minimum bytes
- required incoming state
- guaranteed outgoing state
- CompletionTokenRequired
- Required flag

### SurfaceSlotArtifact

- image ResourceId
- required format
- acquired state
- presented state

Level 2ではSurface slot/resourceは1対1で、Surface有無とTargetProfile.surfaceImageCountが一致する。

## 8. Operation streams

Exactly two streams:

1. Load
2. Frame

Operation tableをcanonicalに分割し、range overlapやgapを許さない。Queue非依存OperationはInvalid QueueIdを使う。

## 9. Operation registry and versions

| Operation | Version |
|---|---:|
| CreateDescriptorHeaps | 1 |
| CreateResource | 1 |
| UploadBuffer | 1 |
| CreateRootSignature | 1 |
| CreateGraphicsPipeline | 1 |
| InitializeState | 1 |
| VerifyBufferContents | 1 |
| UploadTexture | 1 |
| VerifyTextureContents | 1 |
| CreateComputePipeline | 1 |
| AcquireSurfaceImage | 1 |
| AcquireExternal | 1 |
| WaitExternal | 1 |
| ApplyDynamicData | 1 |
| BeginQueueBatch | 1 |
| Transition | 1 |
| ActivateAlias | 1 |
| ExecuteRaster | 1 |
| ExecuteCompute | 1 |
| ExecuteCopy | 2 |
| EndQueueBatch | 1 |
| SignalQueue | 2 |
| WaitQueue | 2 |
| WaitTemporal | 2 |
| ReleaseExternal | 2 |
| PresentSurface | 1 |

Unknown opcodeまたはversion mismatchは常にrejectする。

Version 2の意味:

- ExecuteCopyはexact source/destination ViewIdを参照
- Signal/WaitはSignalPointIdを参照
- WaitTemporalはproducer SignalPointIdを参照
- ReleaseExternalはrelease SignalPointIdを参照

## 10. Load stream contract

Load streamはPackage-owned objectの物質化と再物質化に使用する。

代表Operation:

- CreateDescriptorHeaps
- CreateResource
- UploadBuffer / UploadTexture
- VerifyBufferContents / VerifyTextureContents
- InitializeState
- CreateRootSignature
- CreateGraphicsPipeline / CreateComputePipeline

Device recovery時、同じFrozen PackageとLoad streamから新しいPackageInstanceを構築する。

## 11. Frame stream contract

代表的な順序要素:

- AcquireSurfaceImage / AcquireExternal / WaitExternal
- ApplyDynamicData
- BeginQueueBatch
- WaitQueue / WaitTemporal
- Transition / ActivateAlias
- ExecuteRaster / ExecuteCompute / ExecuteCopy
- EndQueueBatch
- SignalQueue
- PresentSurface
- ReleaseExternal

実際のQueue-local orderと必要happens-beforeはPackage operation streamとSignalPoint contractが固定する。依存しないQueue間の物理interleavingは固定しない。

## 12. Schema validation principles

Decoderは少なくとも次を確認する。

- schema/runtime version
- required Sectionとexact stride
- dense IDとreference bounds
- enum/reserved field
- BlobRef範囲
- Target Profileとdescriptor/invocation schema一致
- Resource/Allocation/View整合性
- Program/Binding/Executable整合性
- Surface/External/Dynamic slot整合性
- Operation opcode/version/payload size
- Load/Frame stream partition
- state and queue capability constraints

すべてD3D12 object生成前に完了する。
