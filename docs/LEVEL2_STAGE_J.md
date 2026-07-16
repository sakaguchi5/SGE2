# Level 2 Stage J — Generated DAG Model Qualification

## Purpose

Stage H proved three independently designed, non-historical Graphs. Stage I fixed canonicalization and metamorphic laws. Those are necessary, but a small hand-written corpus can still miss a hidden cardinality limit, a fixed queue-handoff pattern, or a dependency-shape assumption.

Stage J therefore replaces individual examples with a deterministic graph model and an independent reference oracle. It does not add a GPU feature and it does not change Package ABI meaning. The D3D12 target schema and minimum runtime version remain 16.

Stage J asks one question:

> Within the declared Compute / Buffer-Copy DAG vocabulary, does the Compiler derive the same canonical order, transitive reduction, Queue assignment, wait relation, and Signal identity as an independent finite-graph model when Work count and connectivity are generated rather than hand-written?

Runtime value correctness is intentionally deferred to Stage K. Stage J qualifies graph structure, compilation, Package validation, and frozen operation topology.

## Generator boundary

`26_Level2GeneratedGraphs` is a source-side test-input library. It depends on Base, SemanticModel, and TargetModel only. It does not reference SemanticAnalysis, D3D12TargetCompiler, FrozenPackage, Runtime, or Executor.

A generated node is one of:

- Compute Work with one private UAV output Buffer;
- Buffer Copy Work with one shared immutable source and one private destination Buffer.

All resource hazards are intentionally private to each node. Graph edges are explicit semantic dependencies. This makes the expected DAG independent from Compiler hazard discovery and allows the oracle to know the exact source relation without calling any Compiler or Analysis function.

Logical edges must go from a lower logical node index to a higher logical node index. This is a generator construction rule, not a Source-ID ordering rule. Work IDs are deterministically shuffled, and Source vectors, operand vectors, and dependency vectors are independently shuffled before analysis.

## Independent oracle

The generator computes expected results without calling SGE2 analysis code:

1. deterministic topological order using the Level-2 Work-kind / Work-ID tie break;
2. graph transitive reduction by edge removal and independent reachability search;
3. Queue assignment from the Target profile;
4. canonical Work position for every producer;
5. one cross-queue wait per producer Queue;
6. latest producer position when several dependencies originate from the same Queue;
7. dense frame-local SignalPoint identity equal to canonical Work position.

The test compares SemanticAnalysis and Frozen Package operations against this oracle.

## J1 — exhaustive small-DAG analysis

For node counts one through six, every subset of the forward edge set is enumerated:

```text
n = 1:      1 graph
n = 2:      2 graphs
n = 3:      8 graphs
n = 4:     64 graphs
n = 5:  1,024 graphs
n = 6: 32,768 graphs
-------------------
total: 33,867 graphs
```

Every graph is constructed as a valid SemanticGraph and passed through SemanticAnalysis. The following must exactly match the oracle:

- canonical Work order;
- cover relations after transitive reduction.

This exhaustive layer performs no shader compilation. Its purpose is high-volume validation of finite DAG analysis.

## J2 — representative Package corpus

Twenty-five representative generated Graphs are compiled to Frozen Packages. The corpus includes:

- single Work;
- independent mixed Compute / Copy components;
- mixed Queue chain;
- fan-out;
- fan-in with several producers on one Queue;
- diamond;
- disconnected components;
- layered DAG;
- dense transitively redundant DAG;
- sixteen deterministic six-Work edge masks with mixed Work kinds.

For each case the test verifies:

- repeated Compile produces identical bytes and execution digest;
- a second independent Source-storage permutation produces identical bytes;
- PackageReader and D3D12 schema accept the result;
- Resource, View, Shader, Program, Binding Layout, executable, and command counts are graph-derived;
- Execute kind and Queue match the oracle at every canonical position;
- WaitQueue references the latest required producer SignalPoint for each producer Queue;
- SignalPoint identities are dense, unique, and Queue-correct;
- no Surface, Temporal, or External boundary is invented.

## J3 — deterministic large-seed corpus

Four fixed seeds generate mixed Compute / Copy DAGs with:

```text
10 Works  at 28% forward-edge density
25 Works  at 18% forward-edge density
50 Works  at 12% forward-edge density
100 Works at  8% forward-edge density
```

The seed, Work count, and density are printed on failure. Every case passes the same analysis, determinism, permutation, Package, Queue, Wait, and Signal checks as the representative corpus.

These are deterministic qualification inputs, not nondeterministic fuzz tests. A failure can be reproduced exactly from the logged seed.

## J4 — generated invalid boundary

Nine generated SemanticGraph mutations must be rejected before Package materialization:

- dependency cycle;
- dangling dependency;
- one ResourceUse owned by two Works;
- missing ProgramParameter binding;
- Copy range overflow;
- Previous relation on a non-Temporal Resource;
- unsupported fixed Texture format;
- multiple presentation Surfaces;
- duplicate Work ID.

The generator itself also rejects:

- backward logical edge;
- duplicate logical edge.

The expected rejection stage is checked (`semantic-analysis` or `target-feasibility`).

## What Stage J proves

Stage J proves that the current Level-2 Compiler is not limited to a fixed Resource count, Work count, linear chain, one Queue handoff, one producer per Queue, or a short list of hand-written DAGs within the generated Compute / Copy model.

It also proves that the frozen operation stream agrees with an independently calculated model for:

- topological scheduling;
- transitive reduction;
- Queue placement;
- cross-queue synchronization;
- exact SignalPoint identity.

## What Stage J does not prove

Stage J does not yet claim:

- GPU Readback equality with a CPU reference model;
- interaction coverage for Temporal, External, Dynamic, Alias, and Surface features;
- Raster output correctness;
- adversarial mutation of serialized Package bytes;
- every graph in the entire accepted Semantic vocabulary.

Those belong to Stage K and Stage L. Stage J is the generated finite-DAG structural qualification layer.

## Completion condition

Stage J is complete when:

```text
33,867 exhaustive small DAGs
+ 25 representative compiled Packages
+ 4 deterministic large-seed Packages
+ 9 invalid Compiler-boundary mutations
+ 2 invalid generator contracts
```

all pass while every Stage A through I test remains unchanged and passing.
