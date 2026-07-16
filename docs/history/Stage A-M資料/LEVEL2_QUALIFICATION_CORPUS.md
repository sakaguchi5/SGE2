# SGE2 Level 2 — Frozen Qualification Corpus

## Freeze identity

```text
SGE2-Level2-D3D12-v1-FinalFreeze-Corpus1
Baseline before Stage M: fc6b883b20d5428a4bf4f82b072fab15e8cb844a
Target schema: 17
Minimum Runtime: 17
Semantic corpus digest: 4b07e0725dc87f30f495794ea2ef5e245dd0d6415397761c441a9a1ca4f83c49
```

The semantic corpus digest is computed before D3DCompile. It covers the canonicalized Semantic resources, uses, programs, shader source, Works, reduced dependency graph, and Target profile of every final accepted case. Debug names and non-semantic storage order do not affect it.

## Final accepted Package corpus: 54

- 18 retained Slice 1–15 inputs;
- 1 Stage-G headless Compute input;
- 3 Stage-H non-historical graphs;
- 25 Stage-J representative generated graphs;
- 4 Stage-J deterministic large graphs with 10, 25, 50, and 100 Works;
- 3 Stage-K interaction graphs: dedicated Compute/Copy, Direct fallback, and cross-queue Temporal.

`44_Level2FinalFreezeTests` compiles every case twice in one process, validates schema/runtime v17, decodes the complete Package, and writes one deterministic manifest row containing semantic, file, execution, and target-profile digests plus artifact and operation cardinalities.

It then starts a new process of itself. The child recompiles all 54 cases and emits the same manifest. Parent and child manifests must be byte-identical.

`run_level2_final.bat` repeats the entire qualification in Debug and Release. The two 54-Package manifests must also be byte-identical.

## Earlier retained evidence

### Stage I

- 7 semantic-equivalence transformations preserve Package bytes;
- 7 semantic changes alter execution identity;
- 6 invalid transformations are rejected.

### Stage J

- 33,867 forward-edge DAGs with one through six Works match an independent topology oracle;
- 25 representative Packages cover chain, fan, diamond, layered, dense, disconnected, and mixed-queue forms;
- fixed 10/25/50/100-Work seeds qualify larger cardinalities;
- 9 Compiler-boundary and 2 generator-contract invalid mutations are rejected.

### Stage K

- GPU Compute and Copy values match CPU reference values;
- dedicated queues and Direct fallback produce the same value;
- multiple Dynamic and External slots are validated;
- FrameLocal reuse and cross-queue Temporal Previous/Current values are observed;
- reconstruction restores Package-owned state and resets Temporal history as specified.

### Stage L

- 9 corrupt core-container mutations;
- 4 invalid writer inputs;
- 40 correctly re-digested malicious D3D12 Packages;
- all 53 are rejected before D3D12 materialization.

## Runtime and recovery corpus

The final command also retains:

- Stage-G headless load and reconstruction;
- Stage-H WARP execution and reconstruction;
- Slice 1–15 WARP execution;
- Stage-K semantic Readback;
- controlled reconstruction;
- actual `RemoveDevice` and DRED collection;
- `AwaitingAdapter` and removed-LUID exclusion;
- Compiler-free fresh-process Package rematerialization.
