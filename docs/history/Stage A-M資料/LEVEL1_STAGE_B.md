# Level-1 Stage B — Slice 1–15 End-to-End Qualification

Stage A reconstructed the original Slice 1–15 ladder as 18 independent inputs to one general Compiler path.
Stage B executes every resulting Frozen Package through the same Package Runtime and D3D12 WARP Executor.

## Qualification set

- Slice 1–13: neutral progressive scenarios
- Slice 14: Classical and SDF
- Slice 15: Classical, SDF, and PGA
- Total: 18 Packages

## Per-Package checks

1. Build Semantic input.
2. Compile with the general D3D12 Target Compiler.
3. Serialize and read the Frozen Package.
4. Materialize through Package Runtime and D3D12 WARP.
5. Execute three frames.
6. Validate Package-declared Direct / Compute / Copy queue completions.
7. Validate frame-slot reuse.
8. Validate Temporal previous/current instances and dependency fence.
9. Validate External release completion when the schema contains an External slot.
10. Let load-stream Buffer and Texture verification operations compare GPU readback against Package InitialData.

Slice 13 additionally performs controlled reconstruction, rejects the stale epoch-1 External binding, rebinds an epoch-2 resource, and executes frame zero after Temporal reset.

## Executor implications

Stage B removes the remaining Level-1 shape assumptions needed by the reconstructed ladder:

- Copy queue may be absent; load work then uses the Package-declared Direct queue.
- Compute queue may be present and is materialized from the Target Profile.
- Queue waits, signals, frame-slot fences, and Temporal dependencies are tracked per Package queue.
- Compute commands no longer require a Temporal input.
- Raster commands may omit a depth attachment before Slice 5.
- Copy Work may run on Direct when no dedicated Copy queue exists.

The Executor still does not select queues or invent dependencies. It only materializes and executes the decisions fixed in the Frozen Package.
