# Level 2 Stage E: static completeness, shader reflection, typed compiler pipeline

## Purpose

Stage E closes the gap between a valid Semantic Graph and the actual compiled
shader interface. A graph is no longer accepted merely because its declared
ProgramInterface looks internally consistent. The Target Compiler compiles each
used shader entry, reflects the unstripped DXBC, verifies the reflected contract,
and only then strips reflection/debug data for the Frozen Package.

An accepted graph must fail before Package lowering when any of the following is
inconsistent:

- shader stage and Program kind
- CBV/SRV/UAV register identity
- reflected resource dimension and ProgramParameter kind
- constant-buffer byte size
- vertex input semantic or component count
- the Level 2 static sampler contract
- shader binding arrays, unsupported UAV textures, or unsupported reflection types

The Executor still performs no shader reflection.

## Explicit pipeline stages

The compiler now exposes and internally uses typed stage values:

1. `ValidatedSourceStage`
   - SemanticAnalysis result
   - Level 2 target-capability result
   - stable link to the source SemanticGraph
2. `ProgramCompilationStage`
   - used Programs only
   - compiled and stripped stage bytecode
   - reflected binding and vertex-input facts
3. internal `LoweredPackageStage`
   - complete D3D12 Package description before serialization
4. package serialization and validation
   - container digest
   - Package Reader validation
   - D3D12 schema validation

`CompileOutput.completedStages` records the four completed boundaries for
qualification tests. These stage types are Compiler planning values, not Runtime
or persistent Package ABI types.

## Static-completeness additions

SemanticAnalysis now rejects additional errors before D3D12 lowering:

- overlapping or out-of-stride vertex elements
- invalid Program source-entry combinations
- unused Programs
- unsupported parameter alignment/size contracts
- incompatible states for the same Resource generation inside one Work
- invalid raster vertex-buffer stride or byte range
- Temporal Resources without exactly one current writer and at least one previous reader
- malformed immutable Buffer stride/size contracts
- invalid immutable Texture row pitch
- non-Bgra8 presentation surfaces

## Reflection contract for Level 2 D3D12 v1

- `ConstantBuffer` maps to one reflected cbuffer at the declared `b#`.
- `SampledTexture` maps to one reflected Texture2D at the declared `t#`.
- `ReadOnlyBuffer` maps to one reflected Buffer/StructuredBuffer at the declared `t#`.
- `UnorderedBuffer` maps to one reflected Buffer/RWStructuredBuffer at the declared `u#`.
- sampled textures use the single canonical Pixel-stage static sampler `s0`.
- binding arrays are rejected in v1.
- Raster vertex semantics are POSITION, COLOR, and TEXCOORD with semantic index 0.

Reflection names are diagnostic-only and never enter `.sgep` bytes.

## Regression requirement

After applying Stage E:

- 30_SemanticTests must pass the new static negative tests.
- 31_CompilerTests must prove the typed stages and reject deliberate
  ProgramInterface/HLSL mismatches at `shader-reflection`.
- all 18 Stage-A inputs must still compile deterministically.
- all 18 Stage-B Packages must still execute on WARP.
- fresh-process rematerialization and device-recovery tests must remain green.
