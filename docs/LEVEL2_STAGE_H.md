# Level 2 Stage H — Non-Historical Graph Qualification

## Purpose

Stages A through G proved that the original Slice 1–15 capability set can pass through one general Compiler, Frozen Package, Runtime, and D3D12 Executor.  That history is valuable, but it can hide a different fixed assumption: the implementation may still work only for graph shapes descended from the cumulative Slice experiment.

Stage H therefore adds Semantic Graphs that were designed independently of the Slice sequence.  They do not extend the Slice-15 graph and they do not preserve its resource count, Work order, queue pattern, or presentation structure.

No Package ABI meaning changes in this stage.  The target schema remains version 16.  Stage H is a qualification expansion over the Stage-G implementation.

## Shared scenario boundary

`25_Level2Scenarios` is a test-input library.  It owns only Semantic Graphs, target profiles, and expected high-level topology.  It does not create Package artifacts or D3D12 objects.

Both `31_CompilerTests` and `37_Level1ExecutionTests` consume exactly the same scenarios:

1. Compiler tests inspect analysis order, deterministic Package bytes, artifact cardinalities, operation order, queue waits, and signal identity.
2. Execution tests load those Packages, submit three frames, exercise frame-slot reuse, perform controlled device reconstruction, and submit again.

This prevents the static test and the WARP test from silently using different representations of the intended graph.

## Scenario H1 — Compute / Copy zig-zag

The graph is registered consumer-first but must execute as:

```text
Compute Seed
    ↓  cross-queue wait
Copy Middle
    ↓  cross-queue wait
Compute Finalize
```

It contains four resources, six ResourceUses, two source Programs, three Works, and no Surface.  It uses one Compute queue and one Copy queue.  The expected Frame stream contains two `WaitQueue` operations referencing explicit producer signal points.

This scenario rejects assumptions such as:

- vector order is execution order;
- a Package uses each queue at most once;
- a queue handoff always moves away from Direct;
- Compute cannot resume after Copy.

## Scenario H2 — Compute fan-out / fan-in diamond

The graph is:

```text
             ┌→ Left branch ─┐
Input ───────┤                ├→ Merge
             └→ Right branch ┘
```

The two branches share one source Program but own distinct Work bindings and Package executable artifacts.  All Works execute on the Compute queue, so dependency order must be preserved without producing cross-queue waits.

This scenario rejects assumptions such as:

- one source Program corresponds to one Work;
- every dependency requires a Fence wait;
- a valid graph is a linear chain;
- Package Program cardinality is copied directly from source Program cardinality.

## Scenario H3 — Two-pass Raster Surface graph

Two Raster Works use one shared Raster Program and the same Surface:

```text
Raster pass 1
    ↓ explicit dependency
Raster pass 2
    ↓
Present
```

The top-level Resource, ResourceUse, Program, and Work vectors are deliberately reversed after construction.  Each pass has its own Vertex Buffer and RenderTarget View.  The Package must emit two Raster executable/command artifacts and exactly one Present operation.

This scenario rejects assumptions such as:

- a frame contains only one Raster Work;
- one Surface implies one RenderTarget View;
- source storage order determines Package IDs or command order;
- presentation occurs immediately after the first Surface writer.

## Compiler qualification

For each Stage-H graph, `31_CompilerTests` verifies:

- target-independent analysis derives the expected canonical Work order;
- repeated compilation is byte deterministic;
- permutation of top-level source tables leaves Package bytes unchanged;
- Resource, View, Shader, Program, Binding Layout, executable, and command counts are derived from the graph;
- Execute operation order and Queue IDs match the dependency-derived plan;
- `WaitQueue`, `SignalQueue`, and `PresentSurface` cardinalities match the graph;
- Stage-F signal identity remains explicit and internally consistent.

Operand-order metamorphism and generated graph families are intentionally deferred to Stage I.

## Runtime / Executor qualification

For each Stage-H Package, `37_Level1ExecutionTests` verifies:

- headless Packages load without a Surface host;
- the Raster Package derives its Surface requirement from the Package Surface slot;
- only queues actually used by the Package produce completion records;
- three submissions exercise frame-slot reuse;
- no Temporal or External outputs are invented;
- controlled device reconstruction rematerializes Package-owned objects;
- the same Frozen Package submits successfully in the new device epoch.

## Completion condition

Stage H is complete when all three non-historical scenarios pass:

```text
Semantic construction
→ analysis
→ D3D12 target compilation
→ Frozen Package validation
→ Package-driven load
→ three WARP submissions
→ controlled reconstruction
→ post-recovery submission
```

while all Stage A–G and Slice 1–15 tests remain unchanged and passing.
