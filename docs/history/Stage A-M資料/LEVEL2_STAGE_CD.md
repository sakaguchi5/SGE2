# Level 2 Stage C/D

## Stage C: capability closure

Stage C adds an explicit Level 2 D3D12 v1 capability boundary.

`ValidateLevel2Capability(graph, profile)` rejects unsupported target topology,
Texture2D shapes, multiple surfaces, standalone Present Work, and invalid Temporal frame policy before
Package lowering.

SemanticAnalysis now rejects invalid ResourceUse and operand combinations at the
Source boundary instead of allowing them to become D3D12 command-list errors.

## Stage D: semantic operand model

Stage D replaces the Slice-derived binding vocabulary:

- ComputedBuffer
- CopiedBuffer
- AliasedBuffer
- ExternalBuffer
- TemporalPreviousBuffer
- fixed RasterPayload binding fields
- fixed ComputePayload previous/output fields

with orthogonal facts:

- Resource contract: lifetime, update, aliasPreparation
- ResourceUse: Resource, Effect, generic ViewRole, TemporalRelation
- ProgramParameter: stable identity, kind, stage, shader register, size contract
- WorkOperand: ProgramParameter binding or structural Raster/Copy/Present operand

The Compiler sorts bindings by ProgramParameter identity and freezes that
identity into Root Parameters. Resource declaration order no longer defines
shader register order.

Aliasing is now an explicit Resource-to-Preparation contract. Temporal previous
is a TemporalRelation on a generic ShaderBuffer use.

## Regression requirement

All 18 Stage-A inputs must pass SemanticAnalysis through the new operand model.
Stage A/B must then be rebuilt and run on Windows/WARP to verify package and
executor compatibility.
