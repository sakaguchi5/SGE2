# SGE2 Level 2 D3D12 v1 — Final Capability Constitution

## Status

Semantic GPU Engine 2 Level 2 is frozen at D3D12 target schema 17 and minimum Runtime 17. Level 2 is defined by cardinality and graph generality inside a declared finite vocabulary, not by the number of graphics features.

For every input the Compiler must do exactly one of two things:

1. accept the complete Semantic Graph and freeze every target execution decision into a Frozen Executable Package; or
2. reject it before Package materialization with a named semantic-analysis or target-feasibility diagnostic.

The Runtime and Executor must not recover missing Compiler decisions from Source objects, debug names, historical Slice shape, or D3D12 convenience assumptions.

## Accepted semantic vocabulary

### Resources

- Buffer;
- fixed-size `Texture2D`, exactly one mip level, `Bgra8Unorm`;
- zero or one presentation `SurfaceImage`, `Bgra8Unorm`;
- zero or one surface-relative `Depth32Float` attachment per Raster Work.

### Lifetimes and origins

- Persistent Package-owned;
- FrameLocal Package-owned;
- Temporal Package-owned with Current/Previous physical generations;
- External Buffer with explicit incoming/outgoing state and completion contract;
- Preparation Resource used only by an explicit alias contract;
- Runtime-managed Surface image.

### Work

- Raster;
- Compute;
- Buffer Copy;
- presentation through the Raster `PresentSource` structural operand.

### Graph shape

- any finite acyclic graph expressible by this vocabulary;
- resource and Work cardinalities are not fixed;
- chain, fan-in, fan-out, diamond, layered, dense-transitive, disconnected components, queue zig-zag, WAR and WAW ordering;
- Source vectors need not be execution ordered;
- Source IDs may be sparse;
- one source Program may be used by multiple Works;
- non-semantic vector ordering and transitively redundant dependency edges do not change Package bytes.

### Queue profile

- exactly one Direct queue;
- zero or one Compute queue;
- zero or one Copy queue;
- Compute and Copy may fall back to Direct;
- a queue may be entered, left, and entered again;
- cross-queue waits reference explicit producer `SignalPointId` values.

### Raster profile

- one vertex buffer per Raster Work;
- one Surface color attachment per Raster Work;
- zero or one `Depth32Float` attachment;
- triangle-list topology;
- one final Present per Surface frame.

### Program and binding profile

`ProgramInterface` owns stable `ProgramParameterId` identities. Every Raster or Compute Work binds each required parameter exactly once through a `WorkOperand`.

Supported parameter kinds:

- ConstantBuffer;
- SampledTexture;
- ReadOnlyBuffer;
- UnorderedBuffer for Compute.

Shader register identity comes from `ProgramParameter`, not ResourceUse registration order. Shader Reflection must agree exactly with the declared interface.

### Runtime-qualified interactions

- Surface and surface-free/headless Packages;
- multiple Dynamic slots;
- multiple External Buffer slots;
- FrameLocal physical-instance reuse;
- Temporal Previous/Current across frames and queues;
- Compute/Copy dedicated queues and Direct fallback;
- exact GPU result Readback against CPU reference values;
- Package-owned reconstruction after controlled and actual device removal;
- Temporal history reset and External rebind after recovery;
- fresh-process Package rematerialization without Source or Compiler dependencies.

## Explicitly rejected in Level 2 D3D12 v1

- cyclic graphs or dangling identities;
- more than one presentation Surface;
- Texture2D mip count other than one;
- unsupported fixed or surface-relative Texture formats;
- more than one native Compute or Copy queue;
- Direct queue count other than one;
- standalone Present Work;
- index buffers;
- multiple render targets;
- offscreen color attachments;
- MSAA;
- Texture arrays, cube maps, or mip chains;
- External Texture resources;
- missing, duplicate, or mismatched ProgramParameter bindings;
- one ResourceUse owned by more than one Work;
- invalid Resource/ViewRole/Effect combinations;
- Previous relation on a non-Temporal Resource;
- out-of-range Copy operations;
- overlapping or incompatible explicit alias contracts;
- unsupported root-signature cost or target profile;
- malformed, contradictory, unknown-version, or internally inconsistent Frozen Packages.

Every rejected shape must fail before D3D12 command recording. Device removal is never a Package validator.

## Deferred beyond Level 2

The following are not required for Level 2 and belong to later levels or a later target profile:

- generation and comparison of multiple valid execution plans;
- cost models, latency/memory objectives, profiling, and plan selection;
- same-class multiple native queues;
- new resource or rendering vocabulary such as MRT, index buffers, MSAA, mip chains, and offscreen render graphs;
- dynamic recompilation or profile-guided Package replacement.

Level 2 generates one complete, deterministic, correct plan. Choosing the best plan is a later problem.
