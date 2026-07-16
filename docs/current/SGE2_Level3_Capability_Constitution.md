# SGE2 Level 3 D3D12 v1 Capability Constitution

## 1. Status and baseline

This document freezes the accepted scope of the first Level 3 implementation.

```text
Level 2 code baseline: fc6b883b20d5428a4bf4f82b072fab15e8cb844a
Level 3 documentation baseline: 0df58f81b69d7d1ca333250c74b157391c6cbe90
Target Package schema: 17
Minimum Runtime: 17
Level 2 regression corpus: 54 Packages
```

Level 3 does not expand the Level 2 Semantic vocabulary. It changes the Compiler trust structure: planners propose execution plans, an independent verifier proves contract satisfaction, and policy selects only among verified plans.

## 2. Accepted candidate axes

Level 3 D3D12 v1 accepts bounded, deterministic alternatives on exactly these axes:

1. Work schedule within the Semantic partial order.
2. Direct, Compute, and Copy Queue assignment with explicit cross-Queue synchronization.
3. Committed and legal two-Resource alias allocation strategies.

The internal Plan vocabulary reserves `PlacedNoAlias`, but Schema 17 validation defines every Placed allocation as a two-Resource alias allocation. Consequently `PlacedNoAlias` is generated only as an adversarial/capability candidate and is rejected by the independent verifier for D3D12 v1. Enabling it requires an explicit future Schema/Runtime contract revision; Schema 17 is not silently widened.

Frames in flight, binding layout, executable specialization, new Semantic vocabulary, Package composition, streaming, and Runtime replanning remain outside this version.

## 3. Required trust boundary

```text
SemanticGraph
  -> SemanticObligation + D3D12PlanningContract
  -> Candidate Planner
  -> ExecutionPlanIR[]
  -> Independent Plan Verifier
  -> CostVector + CompilerPolicy
  -> Selected Verified Plan
  -> Schema 17 Frozen Package
```

The Plan Verifier must not call Planner implementation helpers. The Package lowerer must not add a missing Semantic decision. Runtime and Backend must not observe candidate lists, cost vectors, policy, profiles, or rejected plans.

## 4. Canonical safe plan

The Level 2 plan is retained as `CanonicalSafePlan`. For the fixed Level 2 corpus it must reproduce every Package byte, table identity, descriptor index, operation order, and execution digest.

The safe plan is not trusted specially: it must pass the same independent verifier as every other candidate. Failure of the safe plan is a Compiler invariant violation and compilation fails.

## 5. Candidate budget

Candidate generation is always finite and deterministic. The policy contract contains explicit limits for proposed candidates, verified candidates, candidates per axis, and compile work units. Reaching a limit stops enumeration at a canonical boundary; it never changes verification rules.

## 6. Sidecars

Candidate manifests and profile records are Compiler-side sidecars. They are not sections of `.sgep`, do not change Package schema or Runtime version, and may not contain timestamps, process identity, pointers, paths, thread order, or unordered-container iteration order.

## 7. Completion boundary

Level 3 D3D12 v1 is complete only when the Stage Z corpus proves all of the following:

- Level 2 CanonicalSafe Package bytes remain unchanged.
- One obligation can produce multiple distinct verified plans and Packages.
- All candidate acceptance and rejection passes through the independent verifier.
- Valid alternatives have the same declared Semantic observation.
- Different policies select different plans where the corpus exposes a tradeoff.
- Candidate manifests are identical in repeated, fresh-process, Debug, and Release builds.
- Profile-aware reselection creates a new Package and never mutates an existing Package.
- Device reconstruction rematerializes the selected Package without planning.
