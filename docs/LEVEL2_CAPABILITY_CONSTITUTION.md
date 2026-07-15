# SGE2 Level 2 D3D12 v1 Capability Constitution

## Purpose

Level 2 is not defined by the number of GPU features. It is defined by whether
resource and work cardinality and DAG shape are no longer fixed inside the
Compiler or Executor.

The Compiler must either:

1. accept a graph completely and freeze every execution decision into the
   Frozen Package, or
2. reject it before package materialization with a named analysis or
   target-feasibility diagnostic.

An unsupported shape must never first fail in D3D12 command recording.

## Accepted semantic vocabulary

### Resources

- Buffer
- Texture2D with exactly one mip level
- zero or one SurfaceImage

### Texture formats

- fixed immutable Texture2D: Bgra8Unorm
- surface-relative depth attachment: Depth32Float

### Lifetimes

- Persistent
- FrameLocal
- Temporal with one previous/current generation relation
- External Buffer
- Preparation Resource used only by an explicit alias contract

### Work

- Raster
- Compute
- Buffer Copy
- presentation boundary through the Raster `PresentSource` structural operand

### Graph shape

- any finite acyclic graph expressible by the accepted vocabulary
- independent components
- chain, fan-in, fan-out, diamond, explicit WAR ordering
- resource and work IDs need not be dense or vector ordered at Source level

### Queue profile

- exactly one Direct queue
- zero or one Compute queue
- zero or one Copy queue
- Compute and Copy may fall back to Direct

### Raster profile

- one vertex buffer
- one SurfaceImage color attachment
- zero or one Depth32Float attachment
- no index buffer, MRT, MSAA, or offscreen color target in v1

### Binding profile

ProgramInterface owns an ordered list of stable ProgramParameter identities.
Each Raster or Compute Work binds every parameter exactly once through a
WorkOperand.

Supported parameter kinds:

- ConstantBuffer
- SampledTexture
- ReadOnlyBuffer
- UnorderedBuffer (Compute stage only in v1)

Root layout order and shader register identity come from ProgramParameter, not
from ResourceUse declaration order.

## Explicit rejection requirements

The following are rejected by SemanticAnalysis or target-feasibility:

- multiple SurfaceImage resources
- Texture2D mip count other than one
- Direct queue count other than one
- more than one Compute or Copy queue
- Temporal resources with fewer than two frames in flight
- missing, duplicate, or mismatched ProgramParameter bindings
- one ResourceUse owned by more than one Work
- Resource kind / ViewRole / Effect mismatches
- Previous temporal relation on a non-Temporal Resource
- Copy ranges exceeding Buffer size
- incompatible alias Resource shapes, shared Preparation owner, or orphan Preparation Resource
- standalone Present Work in the D3D12 v1 profile
- constant-buffer parameter size/alignment mismatch

## Deferred beyond Stage C/D

The capability vocabulary does not yet claim runtime qualification for:

- surface-free Compute-only runtime execution
- standalone Present Work lowering
- WARP qualification of temporal cross-queue execution (the Stage-F ABI now represents it explicitly)
- WARP qualification of multiple simultaneous external boundary slots
- same-class multiple native queues
- generated-graph qualification

Stage E closes shader-interface reflection and Compiler static-completeness.
Stage F closes the Package ABI for exact current-frame waits, previous-frame
Temporal producer waits, and External release completion through SignalPointId.
The remaining runtime-shape qualifications are closed by later Level 2 stages.
