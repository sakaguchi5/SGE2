# Slice 15 validation

## Pure and structural checks

`34_ExperimentTests` verifies:

- SDF inside, boundary, and outside sign convention
- stable SDF half-space intersections
- PGA finite line meets and point normalization
- PGA point-line incidence
- PGA point joins recovering the original lines up to projective scale
- rejection of an ideal intersection from parallel lines
- bit-identical Classical, SDF, and PGA geometry
- per-frontend byte determinism
- three-way execution-digest identity
- three-way Package-byte identity
- Package Reader acceptance

`verify_dependencies.ps1` verifies:

- the full ProjectReference graph is acyclic
- Executor, readback integration test, and Launcher cannot reach Semantic, Compiler, Experiment, or Frontend projects
- Classical, SDF, and PGA frontends cannot transitively reference sibling frontends
- ExperimentDomain cannot transitively reference a frontend

## Windows/WARP cumulative sequence

`run_tests.bat Debug` performs:

1. Build the full solution.
2. Ask `41_PackageCompiler` to compile all three frontends.
3. Refuse Package output unless Classical, SDF, and PGA converge byte-for-byte.
4. Run Semantic, Compiler, Package, Backend conformance, and cross-frontend experiment tests.
5. Load the shared Package on WARP.
6. Exercise controlled reconstruction, Temporal reset, stale External rejection, and External rebind.
7. Exercise actual RemoveDevice, DRED capture, removed-LUID exclusion, and `AwaitingAdapter`.
8. Launch a child process and rematerialize the same Package on a fresh WARP Device.

The expected last line is:

```text
All Slice-15 tests passed.
```

Actual Visual Studio 2026/v145, Windows SDK, Debug Layer, and WARP execution must be confirmed on Windows.
