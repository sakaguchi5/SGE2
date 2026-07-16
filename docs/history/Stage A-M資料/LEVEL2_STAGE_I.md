# Level 2 Stage I — Canonicalization and Metamorphic Laws

## Purpose

Stage H proved that three graph shapes designed independently of Slice 1–15 can pass through the general Compiler, Frozen Package, Runtime, and Executor.  Stage I changes the form of the proof: instead of adding another hand-written graph, it fixes laws that every accepted Level-2 graph representation must obey.

For one accepted Semantic Graph and one Target Profile, the Compiler must distinguish three categories:

1. an equivalent representation must produce byte-identical Package output;
2. a valid semantic change must change both Package bytes and execution digest;
3. an invalid change must be rejected at a named pre-materialization stage.

Stage I changes no Frozen Package ABI meaning.  The D3D12 target schema and minimum runtime version remain 16.

## Equivalent-representation laws

`38_Level2MetamorphicTests` compiles the Stage-H scenarios and applies the following transformations:

- permutation of top-level Resource, ResourceUse, Program, and Work storage;
- permutation of WorkOperand storage;
- permutation of explicit dependency storage;
- permutation of ProgramParameter storage while preserving `ProgramParameterId`;
- permutation of Raster vertex-input storage while preserving semantic/offset identity;
- mutation of Resource, Program, ProgramParameter, and Work diagnostic names;
- addition of an explicit dependency already implied by a transitive path;
- restatement of derived hazard edges as explicit dependencies.

Every transformed graph must produce exactly the same Package bytes and execution digest as its baseline.

### Canonicalization changes

Stage I therefore makes these implementation rules explicit:

- ProgramParameter identity is `ProgramParameterId`, not vector position;
- ProgramParameter IDs remain a unique dense set, but their vector order is not semantic;
- Root-parameter order and interface digest are canonicalized by ProgramParameter identity;
- vertex elements and interface digest are canonicalized by byte offset and semantic identity;
- frame-operation emission traverses WorkOperands in canonical `(kind, parameter, use)` order;
- the analyzed DAG exposes its unique transitive reduction, so redundant explicit edges do not create additional queue waits or Package differences.

## Semantic-sensitivity laws

Stage I also verifies the opposite direction.  Each of the following accepted changes must change both Package bytes and execution digest:

- Compute dispatch dimensions;
- Copy byte count;
- shader program semantics and resulting shader binary;
- ProgramParameter shader register, together with the matching HLSL register;
- Resource shape;
- Queue profile and fallback assignment;
- Raster vertex count.

This prevents a superficially deterministic Compiler from silently ignoring execution-affecting input.

## Rejection-boundary laws

The following mutations must fail before Package materialization:

- dependency cycle — `semantic-analysis`;
- duplicate ProgramParameter binding — `semantic-analysis`;
- unknown ProgramParameter identity — `semantic-analysis`;
- duplicate ProgramParameter declaration identity — `semantic-analysis`;
- Copy range overflow — `semantic-analysis`;
- unsupported queue topology — `target-feasibility`.

## Source-ID boundary

Source IDs are stable identities inside one Semantic Graph.  Stage I requires that vector storage order and ID density do not affect lowering; the existing Compiler qualification retains the sparse, order-preserving Source-ID remap test.

An arbitrary permutation of all stable Source IDs is a graph-isomorphism canonical-labeling problem and is not declared an equivalent source representation in Level 2 v1.  Requiring it would conflict with stable diagnostic identity and future incremental compilation without adding GPU-execution correctness.  Package IDs remain newly assigned dense IDs and never copy Source IDs into the ABI.

## Qualification project

Stage I adds:

```text
38_Level2MetamorphicTests
```

The test prints one line for each equivalent, sensitive, and rejected law.  Its success line is:

```text
Stage-I canonicalization and metamorphic laws passed.
```

## Completion condition

Stage I is complete when:

```text
equivalent representation
    -> byte-identical Package and execution digest

valid semantic change
    -> different Package and execution digest

invalid change
    -> named Compiler rejection stage
```

and all Stage A–H, Slice 1–15, WARP execution, fresh-process rematerialization, and device-recovery tests remain passing.
