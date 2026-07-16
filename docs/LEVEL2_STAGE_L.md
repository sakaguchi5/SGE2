# Level 2 Stage L — Adversarial Boundary / Backend Conformance

## Purpose

Stages C through K established the positive path: the Compiler accepts the declared Level-2 vocabulary, freezes its decisions, and the Runtime/Executor reproduces the intended GPU result. Stage L establishes the complementary negative path.

A Frozen Package is an executable authority. Therefore a Package that has valid outer digests but contradictory internal meaning must be rejected before D3D12 resource, descriptor, pipeline, or command-list materialization. Device removal is not an acceptable Package validator.

Stage L does not add a GPU feature and does not change Package meaning. Target schema and minimum Runtime remain version 17.

## One operation contract registry

`D3D12Encoding` now exposes one operation contract table containing every supported opcode and its exact operation version.

The table is used by:

1. the D3D12 Target Compiler when serializing operations;
2. the Package decoder and operation validator;
3. `D3D12Executor::SupportsOperation`;
4. the Stage-L conformance test.

The table contains 26 unique operations. `ExecuteCopy`, `SignalQueue`, `WaitQueue`, `WaitTemporal`, and `ReleaseExternal` use version 2; all other current operations use version 1. Unknown opcodes map to version zero and are unsupported.

This removes the possibility that Compiler, Schema, and Executor maintain independent opcode/version switches that silently drift apart.

## Core container hardening

`PackageReader` rejects:

- non-zero Package header flags;
- zero or file-size-impossible section counts;
- unknown section flag bits;
- contradictory `DebugOnly | ExecutionAffecting` sections;
- unsupported schema versions on known sections.

The existing magic, size, alignment, overlap, section digest, file digest, execution digest, profile digest, required-section, and reserved-byte checks remain in force.

## D3D12 record graph hardening

The decoded tables are validated as one closed object graph rather than as unrelated records.

The validation now includes:

- Manifest flags and exact table cardinalities;
- canonical Resource-to-View table partitioning;
- supported Resource flags, shapes, origins, rebuild policies, and physical instances;
- Allocation ownership, heap class, alignment, alias-group, and Resource compatibility;
- View/Resource class compatibility and exact Buffer or Texture ranges;
- non-overlapping descriptor ranges and canonical descriptor instance stride;
- Temporal Current/Previous View identity;
- Shader stage/profile/flags and inner bytecode digest;
- canonical BindingLayout/RootParameter partitions;
- RootParameter-to-DynamicSlot or View-class compatibility;
- Program-to-Shader stage compatibility;
- Raster/Compute executable and command contracts;
- one-to-one Dynamic, External, and Surface Resource slot ownership;
- exact External synchronization and state contracts.

## Operation-stream hardening

Operation payloads must use one canonical eight-byte packing with no gaps, overlaps, or trailing unreferenced bytes. Load and Frame streams must exactly partition the operation table.

The validator additionally enforces:

- one unique first `CreateDescriptorHeaps` load operation;
- one canonical load queue;
- Resource creation before upload, initialization, or verification;
- BindingLayout creation before pipeline creation;
- one materialization operation for every Package-owned object;
- queue-class legality for Raster, Compute, Copy, and states;
- batch begin/end balance;
- symbolic Transition before-state consistency;
- valid placed-allocation alias activation;
- dense, unique SignalPoint identities;
- waits that reference an earlier producer on another queue;
- exact one-time Dynamic slot application;
- exact `Acquire → Wait → Release` sequence for every External slot;
- exact `Acquire → Present` sequence for every Surface slot.

## Adversarial corpus

`43_Level2AdversarialBoundaryTests` first compiles and decodes three valid baseline Packages:

- Stage-K Dynamic/External Compute-Copy pipeline;
- Stage-K cross-queue Temporal pipeline;
- Stage-H two-pass Raster Surface graph.

It then performs three classes of mutations.

### Core Reader: 9 mutations

These include bad magic, unsupported header flags, impossible section count, reserved header data, file-size mismatch, known-section schema mismatch, unsupported section flags, file digest corruption, and truncation.

### Core Writer: 4 mutations

These include duplicate sections, non-power-of-two alignment, contradictory table stride, and missing D3D12 target profile.

### Re-digested D3D12 Package: 40 mutations

For these tests the affected section and complete Package are reserialized with correct section, execution, profile, and file digests. Rejection therefore cannot be attributed to accidental checksum damage.

The corpus mutates:

- Manifest counts and flags;
- target queue/profile capability;
- Resource ranges, flags, and physical instances;
- Allocation flags, alignment, and alias contracts;
- View classes, Temporal flags, descriptor ranges, and descriptor overlap;
- Shader inner digest;
- Program, BindingLayout, RootParameter, and ComputeCommand contracts;
- Dynamic and External slot identity and shape;
- opcode, operation version, flags, queue, and payload packing;
- load/frame stream partition and flags;
- Transition state bits;
- ExecuteCopy View identity;
- WaitQueue SignalPoint identity;
- repeated Dynamic application and missing External release;
- Surface profile, slot, Present, and Raster attachment identity.

Every re-digested mutation must fail in D3D12 schema validation, before the Package can reach Executor materialization.

## Completion condition

Stage L is complete when:

1. all 26 operation contracts are unique and every baseline Package operation matches the registry;
2. all 9 raw container mutations are rejected by `PackageReader`;
3. all 4 invalid construction inputs are rejected by `PackageWriter`;
4. all 40 re-digested semantic mutations are rejected by D3D12 Package decode;
5. all Stage C–K, Slice 1–15, WARP semantic-readback, reconstruction, actual RemoveDevice/DRED, and fresh-process rematerialization tests remain passing.
