# Semantic GPU Engine 2 — G1-G7 Generic Compiler

The compiler path has been generalized through G1-G7. See [docs/GENERALIZATION_G1_G7.md](docs/GENERALIZATION_G1_G7.md) for the implemented contracts and the explicit boundary before Level 2 Qualification.

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
```

`41_PackageCompiler` accepts a frontend selector:

```powershell
41_PackageCompiler.exe CommonExperiment.sgep all
41_PackageCompiler.exe Classical.sgep classical
41_PackageCompiler.exe Sdf.sgep sdf
41_PackageCompiler.exe Pga.sgep pga
```

The default is `all`. The old Slice-14 spelling `both` is retained as an alias for `all`. In equivalence mode the compiler refuses to write a Package unless all three frontends agree on geometry, execution digest, and every Package byte.

Expected final lines include:

```text
PGA join, meet, incidence, finite normalization, and ideal-point rejection passed.
Classical explicit-mesh, SDF half-space, and PGA line-algebra frontends converged to bit-identical geometry, Semantic execution, and Frozen Package bytes.
Experiment equivalence test passed.
All Slice-15 tests passed.
```

## Project boundary

`20_ClassicalFrontend`, `21_SdfFrontend`, and `22_PgaFrontend` cannot reference one another. All three may depend on neutral `23_ExperimentDomain`. `10_D3D12Executor`, `35_D3D12ReadbackTests`, and `40_Launcher` remain unable to reference Semantic, Compiler, Experiment, or Frontend projects.
