# Semantic GPU Engine 2 — Level 3 Complete

Semantic GPU Engine 2 Level 3 is frozen at D3D12 target schema 17 and minimum Runtime 17. For one semantic obligation, the Compiler can construct multiple valid schedule, Queue, and allocation Plans; an independent Verifier checks them before policy selection; and only the selected Plan is lowered into the source-independent Frozen Package. Runtime and Backend remain unaware of planning, verification, cost, policy, and profiling.

The authoritative completion command is:

```powershell
.\run_level3_final.bat
```

It first reruns the complete Level 2 freeze, including the byte-identical fixed 54-Package corpus. It then qualifies the Level 3 obligation and Plan encodings, independent Verifier, adversarial mutations, bounded candidate generation, cost/policy selection, profile-aware offline reselection, WARP observation equivalence, and same-process/fresh-process/Debug/Release manifest determinism. See the [Level 3 capability constitution](docs/current/SGE2_Level3_Capability_Constitution.md), [observation contract](docs/current/SGE2_Level3_Observation_Contract.md), and [qualification record](docs/current/SGE2_Level3_Qualification.md).

The selected Plan still uses Package schema 17. That schema represents Placed allocation only as a two-resource alias group, so `PlacedNoAlias` is a named unsupported capability and is rejected by the independent Verifier. It is not silently encoded as another strategy; supporting it requires an explicit future schema/runtime revision.

The Slice 15 experiment below remains the retained Level 1 reference input and cross-frontend equivalence proof.

## Retained Level 1 reference — Slice 15 PGA Direct Frontend

Slice 15 adds a third independent free-domain frontend based on 2D projective geometric algebra (PGA). Classical explicit geometry, an SDF half-space field, and a PGA line arrangement must all disappear at the Semantic boundary and produce exactly one Frozen Executable Package.

## Three independent inputs

The Classical frontend owns explicit vertices. The SDF frontend owns oriented half-spaces and reconstructs their intersections. The PGA frontend owns homogeneous points and lines:

```text
Point P = x*e20 + y*e01 + w*e12
Line  l = a*e1  + b*e2  + c*e0
Incidence: a*x + b*y + c*w = 0
```

The PGA path implements:

- `Join(point, point) -> line`
- `Meet(line, line) -> homogeneous point`
- finite-point normalization to `w = 1`
- projective line equivalence up to nonzero scale
- incidence validation
- ideal/parallel intersection rejection

It reconstructs each triangle from the meets of its base, right-edge, and left-edge PGA lines. It does not call Classical or SDF code.

## Slice 15 theorem exercised by the tests

```text
Classical explicit vertices ───────┐
SDF half-space intersections ─────┼→ bit-identical neutral geometry
PGA line join/meet lowering ───────┘              ↓
                                            SemanticGraph
                                                 ↓
                                      D3D12 Target Compiler
                                                 ↓
                              byte-identical Frozen Executable Package
```

The Package contains no frontend kind, SDF equation, PGA multivector, line equation, or Classical mesh object. Runtime and Backend cannot tell which frontend produced it.

## Retained Slice 1–14 behavior

The common Package still exercises the cumulative execution path:

- dynamic `FrameInvocation`
- Buffer and Texture initial upload
- depth attachment
- Compute Work
- Copy Work and Copy queue
- multiple-queue handoff
- FrameLocal and Temporal resources
- Aliasing
- External Acquire / Release
- controlled device reconstruction
- actual RemoveDevice / DRED path
- removed-LUID exclusion and `AwaitingAdapter`
- fresh-process Package rematerialization
- Classical / SDF cross-frontend equivalence

The Slice 13 lifecycle corrections remain included: process-once Debug Layer/DRED configuration and the final `PresentSurface -> SignalQueue -> ReleaseExternal` completion boundary.

## Commands

```powershell
.\run_tests.bat Debug
.\run_demo.bat Debug
.\run_demo.bat Debug --warp
.\run_demo.bat Debug --warp --force-removal
.\verify_dependencies.ps1
.\run_level2_final.bat
.\run_level3_final.bat
```

`41_PackageCompiler` accepts a frontend selector:

```powershell
41_PackageCompiler.exe CommonExperiment.sgep all
41_PackageCompiler.exe Classical.sgep classical
41_PackageCompiler.exe Sdf.sgep sdf
41_PackageCompiler.exe Pga.sgep pga
```

The default is `all`. The old Slice-14 spelling `both` is retained as an alias for `all`. In equivalence mode the compiler refuses to write a Package unless all three frontends agree on geometry, execution digest, and every Package byte.

The standard test command retains the earlier output below. The Level 3 final-freeze command ends with `SGE2 LEVEL 3 FINAL FREEZE PASSED` and `Semantic GPU Engine 2 Level 3 is complete.`

Expected retained lines include:

```text
PGA join, meet, incidence, finite normalization, and ideal-point rejection passed.
Classical explicit-mesh, SDF half-space, and PGA line-algebra frontends converged to bit-identical geometry, Semantic execution, and Frozen Package bytes.
Experiment equivalence test passed.
All Slice-15 tests passed.
```

## Project boundary

`20_ClassicalFrontend`, `21_SdfFrontend`, and `22_PgaFrontend` cannot reference one another. All three may depend on neutral `23_ExperimentDomain`. `12_Level3PlanModel` and `13_Level3PlanVerifier` are source/compiler-side libraries and cannot reference the Compiler, Runtime, Executor, or Platform projects; the Verifier also does not reuse Planner validation helpers. `10_D3D12Executor`, `35_D3D12ReadbackTests`, and `40_Launcher` remain unable to reference Semantic, Compiler, Experiment, Frontend, or Level 3 planning projects.
