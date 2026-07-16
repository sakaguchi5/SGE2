# Slice 14 — Classical / SDF Common Experiment

## Goal

Prove that a free-domain representation is not part of the executable identity.

A Classical mesh and an SDF field must be able to express one experiment independently, converge at the Semantic boundary, and produce exactly one Frozen Executable Package.

## Neutral experiment

`23_ExperimentDomain` owns only the shared target:

- two isosceles triangle layers
- exact target-space dimensions
- depth and color values
- the canonical experiment vertex contract
- the common Semantic recipe used after frontend-specific geometry lowering

The triangle coordinates use binary-exact fractions. This lets the SDF intersection path and Classical direct-vertex path be compared bit-for-bit rather than with a visual tolerance.

## Classical path

`20_ClassicalFrontend` reads the neutral layer parameters and directly emits:

```text
apex
right base
left base
```

for the near and far triangles.

## SDF path

`21_SdfFrontend` converts each layer into three interior half-spaces:

```text
base       : y >= baseY
right edge : rise*(x-centerX) + halfWidth*(y-apexY) <= 0
left edge  : -rise*(x-centerX) + halfWidth*(y-apexY) <= 0
```

Its signed field is the maximum normalized boundary distance. The frontend independently solves:

```text
left ∩ right  → apex
right ∩ base  → right base
base ∩ left   → left base
```

It canonicalizes signed zero and rejects parallel, non-finite, or outward-facing results.

## Equivalence boundary

The frontends meet only at `experiment::TriangleGeometry`.

The following are required:

1. Classical and SDF geometry are bit-identical.
2. Each frontend is deterministic by itself.
3. Both graphs pass Semantic analysis and Target compilation.
4. Both execution digests are identical.
5. Both serialized Package byte arrays are identical.
6. The common Package passes the Package Reader.

This is stronger than “the images look similar.” It proves that the Compiler and Backend receive the same executable meaning.

## Dependency rule

Forbidden:

```text
20_ClassicalFrontend → 21_SdfFrontend
21_SdfFrontend       → 20_ClassicalFrontend
23_ExperimentDomain  → either frontend
```

Allowed:

```text
20_ClassicalFrontend ─┐
                      ├→ 23_ExperimentDomain → Semantic boundary
21_SdfFrontend ───────┘
```

`verify_dependencies.ps1` checks the frontend isolation transitively.

## Non-goals

Slice 14 does not yet execute an SDF directly in a pixel or compute shader. It proves the earlier and more fundamental boundary: an SDF free-domain description can choose a mesh realization during frontend lowering without contaminating Semantic, Package, Runtime, or Backend types.

Direct mathematical frontend execution is reserved for later slices, beginning with the PGA direct frontend experiment.
