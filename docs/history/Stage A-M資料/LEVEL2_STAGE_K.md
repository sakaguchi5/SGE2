# Level 2 Stage K — Runtime Semantic Correctness / Interaction Matrix

## Purpose

Stages H through J established that the Compiler can canonicalize and lower non-historical and generated finite DAGs.  A successful D3D12 submission, however, proves only that the recorded commands were accepted.  It does not by itself prove that the Package bound the intended physical resource instance or produced the intended values.

Stage K closes that distinction for the Level-2 D3D12 v1 runtime boundary.  It observes Package outputs through explicit external-resource completion tokens, compares them with an independent CPU calculation, repeats the observation across frame-slot reuse, and repeats it after controlled device reconstruction.

Stage K does not add an Executor-side inference path.  Every queue, transition, binding, external boundary state, temporal relation, and release signal remains frozen in the Package.  The D3D12-specific observation helper runs only after a Package-declared external release and restores the exact required incoming state before returning a new completion token.

## ABI v17: Copy identifies exact views

Stage K exposed an ABI ambiguity that previous tests could not observe.  `ExecuteCopy` version 1 named only source and destination `ResourceId`s.  A Temporal resource has multiple physical instances, so a Resource ID alone cannot distinguish `Previous` from `Current`.  The old Executor consequently had no Package-frozen identity from which to select the previous physical generation for a Copy.

Target schema 17 therefore defines `ExecuteCopy` operation version 2:

```text
source ViewId
destination ViewId
source offset
destination offset
byte count
```

The View artifacts carry Temporal Previous/Current flags and descriptor-instance meaning.  Package validation checks that the views are CopySource/CopyDestination Buffer views and that the ranges fit those exact views.  The Executor resolves resources and tracked states from those views; it does not reconstruct temporal meaning from the source graph.

## K1 — Dynamic / External / FrameLocal / multi-queue value pipeline

The first qualification graph contains:

- two Dynamic slots;
- three simultaneous External Buffer slots;
- two FrameLocal intermediate Buffers;
- two Compute Works;
- two Copy Works;
- Compute → Copy → Compute → Copy queue handoffs.

Its value equation is:

```text
OutputA = DynamicA + ExternalInput + 2 * DynamicB
OutputB = Copy(OutputA)
```

Both outputs are read back after their Package-declared release tokens and compared with the CPU equation for three frames.  The same graph is also compiled with zero dedicated Compute and Copy queues; every Work then executes on Direct, emits no cross-queue WaitQueue operation, and must produce the same CPU-checked value.  The third submission reuses frame slot zero, proving that the observed values follow the selected FrameLocal physical instances rather than stale resources.

External slots exercise different first/last states:

```text
input    ShaderResource → ShaderResource
outputA  UnorderedAccess → CopySource
outputB  CopyDestination → CopyDestination
```

The test also rejects missing, duplicate, unknown, wrong-sized, resource-slot-mismatched, and completion-token-slot-mismatched invocation bindings before Frame operations execute.

After controlled reconstruction, old external resources and tokens are rejected by device epoch.  New resources are bound and the same Frozen Package reproduces the expected value.

## K2 — Cross-queue Temporal previous/current value qualification

The second graph contains a two-instance Temporal Buffer initialized with a known seed:

```text
Copy queue:    Temporal Previous → External output
Compute queue: Dynamic value → Temporal Current
```

For frame zero, the observed Previous value is the Package initial seed.  For later frames, the observed value is exactly the preceding frame's Dynamic value.  The test checks:

- current and previous physical-instance indices;
- no previous-frame fence on frame zero;
- a non-zero previous-frame dependency on later frames;
- exact external readback values;
- rejection of a non-consecutive Temporal frame number.

Controlled reconstruction resets Temporal history.  The first post-recovery frame again observes the Package seed, proving that reconstruction rematerializes all Temporal physical instances and does not preserve stale history accidentally.

## External observation boundary

`D3D12Executor::CreateExternalBuffer` creates a D3D12 implementation for one exact Package External slot.  It validates slot shape, allocates required UAV capability when the Package views require it, initializes the Buffer, transitions it to `requiredIncomingState`, and returns a completion token.

`D3D12Executor::ReadExternalBuffer` requires:

- a resource created for the exact Package slot by the same Package instance;
- a completion token for that same slot, instance, and device epoch;
- the Package-declared `guaranteedOutgoingState`.

It waits for the release token, copies through an explicit Direct-queue observation command, restores `requiredIncomingState`, and returns the token that must precede the next Package acquisition.  This is an application/test interop operation outside the Frozen Package execution stream; it does not invent any Package execution decision.

## Completion condition

Stage K is complete when both qualification Packages pass:

```text
Compile
→ Frozen Package validation (schema 17)
→ WARP load
→ invocation-boundary rejection tests
→ three frame submissions
→ completion-token readback
→ CPU value comparison
→ frame-slot reuse
→ controlled reconstruction
→ stale-epoch rejection / external rebind
→ post-recovery value comparison
```

while all Stage A–J, Slice 1–15, fresh-process rematerialization, controlled reconstruction, and actual RemoveDevice/DRED tests remain passing.
