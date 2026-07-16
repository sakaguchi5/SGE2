# SGE2 Level 3 Qualification

## 1. Freeze identity

```text
Identity: SGE2-Level3-D3D12-v1-FinalFreeze-Corpus1
Level 2 baseline: fc6b883b20d5428a4bf4f82b072fab15e8cb844a
Level 3 docs baseline: 0df58f81b69d7d1ca333250c74b157391c6cbe90
Target schema: 17
Minimum Runtime: 17
Level 2 regression corpus: 54 Packages
Plan contract version: 1
```

## 2. Qualification corpora

### A. Level 2 regression

The fixed 54-Package corpus is compiled through both the retained Level 2 path and `CanonicalSafePlan -> independent verifier -> selected Plan lowering`. Every Package byte and execution digest must match.

### B. Multi-plan corpus

Finite schedule, Queue, synchronization, and Schema-17-compatible allocation candidates are generated with stable identities and explicit budgets. Distinct verified Plans lower to distinct Packages where the selected execution differs. WARP execution compares Published/External observations rather than Package bytes.

### C. Invalid Plan corpus

Mutations cover missing/duplicate/reordered Work, missing or impossible Queue assignment, missing cross-Queue synchronization, invalid physical instance count, incorrect lifetime, incomplete allocation coverage, illegal alias, missing state plan, missing binding, invalid External/Present boundary, and corrupted PlanIdentity. Rejection must occur before Package lowering.

### D. Profile corpus

Fixed Profile records cover successful offline reselection and rejection of stale target contract, PlanIdentity, Package execution digest, adapter/driver fingerprint, measurement scenario, and insufficient sample count. Reselection recompiles a new Package; Runtime and Backend never read Profile records.

## 3. Determinism

The freeze manifest contains Obligation digest, PlanningContract digest, PlanIdentity, Package file/execution digest, policy-selected identity, and Candidate Manifest digest. It is emitted twice in fresh Debug processes and once in Release; all bytes must match. Repeated compilation inside one process is also checked.

## 4. Runtime and recovery

Verified schedule and Queue alternatives execute on WARP with identical External Buffer observations. A selected Package is reconstructed from unchanged Package bytes, requires External rebinding, and repeats the observation successfully.

## 5. Schema 17 boundary

Schema 17 represents Committed allocations and two-Resource Placed alias allocations. The internal `PlacedNoAlias` vocabulary is deliberately rejected by the D3D12 v1 PlanningContract because widening Schema 17 validation would mutate the Level 2 ABI. It remains a named future capability requiring an explicit Schema/Runtime revision.

## 6. Authoritative command

```powershell
.\run_level3_final.bat
```

Successful final line:

```text
SGE2 LEVEL 3 FINAL FREEZE PASSED
Semantic GPU Engine 2 Level 3 is complete.
```
