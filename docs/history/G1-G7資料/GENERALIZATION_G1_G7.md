# G1-G7 generic compiler implementation

This repository now contains the Level 1 to Level 2 compiler generalization stages described below. The implementation deliberately stops before **Level 2 Qualification**: it does not rebuild Slice 1 through Slice 15 as fifteen independent inputs and does not claim that qualification.

## G1 - Generic cardinality

- Source `ResourceId`, `ResourceUseId`, `ProgramId`, and `WorkId` no longer have to equal vector positions or be dense.
- Semantic analysis creates canonical ID orders.
- Target lowering assigns fresh dense Package IDs and keeps explicit Source-to-Package maps.
- Resource, View, Program, Work, slot, artifact, and operation table sizes are derived from input collections.

## G2 - Generic dependency graph

- `ResourceUse` effects derive RAW, WAR, WAW, and Present dependencies.
- `Work::dependencies` represents semantic ordering that cannot be inferred from effects.
- Stable topological sorting is independent of vector order.
- Cycles produce semantic diagnostics.

## G3 - Generic lifetime and instances

- Analysis records first/last use intervals for every Resource.
- Persistent and Preparation Resources produce one physical instance.
- FrameLocal and Temporal Resources produce `framesInFlight` instances.
- External and Surface resources remain Runtime-owned and do not receive Package allocations.
- Temporal current and previous uses become separate state cells and descriptor-instance relations.

## G4 - Generic state planning

- `ViewRole` maps to required D3D12 states.
- The compiler tracks explicit state cells and emits `before`/`after` transitions only when states differ.
- Copy queues are restricted to Common, CopySource, and CopyDestination.
- Surface, External, Temporal, and frame-end state contracts are fixed in the operation stream.

## G5 - Generic queue planning

- Raster and Present use Direct queues.
- Compute uses a Compute queue when available and otherwise falls back to Direct.
- Copy uses a Copy queue when available and otherwise falls back to Direct.
- Cross-queue dependency edges generate Signal/Wait operations deterministically.

## G6 - Generic allocation planning

- The safe baseline is one committed allocation per Package-owned Resource.
- Allocation class, alignment, and physical instance count are derived from resource requirements.
- A compatible Preparation resource and an explicit Resource.aliasPreparation target may share one placed allocation when their load/frame lifetimes do not overlap. Stage D removed alias origin from shader-input roles.
- Alias activation order is emitted explicitly.

## G7 - Generic binding and Package lowering

- `ProgramInterface` counts are no longer fixed to the Slice 15 layout.
- Generic Builder entry points accept arbitrary ResourceUse collections.
- CBV, SRV, and UAV root parameters, shader registers, descriptors, serialized Root Signatures, Programs, Executables, Commands, invocation slots, and operation streams are generated from the graph and target profile.
- The Frozen Package schema validator now validates general dense artifact tables, ranges, references, descriptor capacity, blobs, invocation contracts, queues, batches, and operation payloads instead of one fixed Slice cardinality and operation order.

## Verification boundary

`30_SemanticTests` verifies G1-G3 analysis behavior. `31_CompilerTests` verifies G1-G7 lowering, including sparse IDs, reordered source vectors, added Resources and Work, added bindings, multiple queues, deterministic bytes, and general Package decode. `32_PackageContractTests` retains container corruption checks, and `34_ExperimentTests` retains Classical/SDF/PGA byte equivalence.

The existing D3D12 Executor still contains Level 1 fixed-profile assumptions. Generalizing that Executor and then reconstructing and executing Slice 1 through Slice 15 as independent generic inputs belongs to the explicitly excluded Level 2 Qualification step.
