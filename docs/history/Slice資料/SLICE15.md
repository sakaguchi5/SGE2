# Slice 15 — PGA Direct Frontend

## Goal

Prove that a projective-geometric-algebra source representation can lower directly into the existing Semantic execution boundary without introducing PGA knowledge into Semantic, Compiler, Frozen Package, Runtime, or Backend projects.

“Direct” means that the PGA frontend is an independent source path. It does not translate through the Classical or SDF frontend and does not reuse either implementation. Its normalized PGA meets are converted to the neutral experiment vertex ABI as the frontend-lowering boundary.

## PGA representation

`22_PgaFrontend` uses the 2D homogeneous model:

```text
P = x*e20 + y*e01 + w*e12
l = a*e1 + b*e2 + c*e0
```

A point lies on a line when:

```text
a*x + b*y + c*w = 0
```

For two finite points, `Join` produces their projective line. For two nonparallel lines, `Meet` produces their homogeneous intersection. A point with `e12 = 0` is ideal and cannot be lowered to the current finite vertex ABI, so it is rejected explicitly.

## Triangle construction

Each neutral isosceles-triangle layer becomes three oriented PGA lines:

```text
base
right edge
left edge
```

The vertices are derived only through line meets:

```text
left meet right -> apex
right meet base -> right base
base meet left -> left base
```

The frontend then proves locally that:

- every meet is finite
- every point is incident with both generating lines
- joining adjacent reconstructed points recovers the original line projectively
- the centroid is inside the oriented line arrangement
- all coordinates are finite

The experiment uses binary-exact parameters, so normalized PGA coordinates match the Classical and SDF outputs bit-for-bit.

## Equivalence boundary

Slice 15 requires all of the following:

1. PGA join/meet/incidence primitives satisfy their local contracts.
2. Parallel PGA lines are rejected as an ideal intersection.
3. Classical, SDF, and PGA geometry are bit-identical.
4. Each frontend independently produces deterministic Package bytes.
5. All three execution digests are identical.
6. All three complete serialized Package byte arrays are identical.
7. The Package Reader accepts the shared output.
8. The cumulative WARP, Device-loss, recovery, and fresh-process tests still pass.

This is stronger than image equivalence and stronger than semantically similar graph output. It proves one executable identity.

## Dependency boundary

Forbidden transitively:

```text
Classical -> SDF or PGA
SDF       -> Classical or PGA
PGA       -> Classical or SDF
ExperimentDomain -> any frontend
```

Allowed:

```text
Classical --\
SDF --------> ExperimentDomain -> Semantic boundary
PGA -------/
```

PGA types stop at `22_PgaFrontend`. No PGA point, line, join, meet, or multivector coordinate is added to the Frozen Package ABI.

## Non-goals

This slice does not yet make the GPU execute a general geometric-algebra algebra or retain symbolic PGA expressions in the Package. It proves that PGA can be a first-class free-domain language while the downstream executable remains representation-neutral.
