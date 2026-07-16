# Level 3 decisive authority tests

Base commit: `c42b0a5d198ec8ad05daa3754dc98d7418b88465`

This project is deliberately separate from `run_level3_final.bat` until the failures are repaired.
It tests the trust boundary that the provisional qualification did not prove:

1. `CompileSelectedPlan` must reject a Plan rejected by the independent verifier.
2. A changed accepted Binding root index must either be rejected or change Package bytes.
3. A changed accepted descriptor index must either be rejected or change Package bytes.
4. A changed accepted SignalPoint must either be rejected or change Package bytes.
5. A changed Resource-to-Allocation reference must either be rejected or change Package bytes.
6. Existing valid Queue and Allocation decisions remain positive controls.
7. Profile provenance is checked for contract, Plan, Package, adapter, scenario, and sample count.

The central rule is:

```text
Plan mutation
  -> verifier rejects it
or
Plan mutation
  -> verifier accepts it
  -> Package bytes freeze the changed decision
```

An accepted Plan mutation that changes `PlanIdentity` but leaves Package bytes unchanged is a failure.

Run from the repository root:

```powershell
cmd /c .\run_level3_decisive_tests.bat
```

On the base commit, failures for the verifier gate, Binding authority, SignalPoint authority, and
Resource Allocation-reference authority are expected. After those gaps are repaired, both Debug and
Release executions must end with:

```text
SGE2 LEVEL 3 DECISIVE AUTHORITY TESTS PASSED
```

Only after this project passes should it be added to `run_level3_final.bat` and the freeze identity be regenerated.


## Harness fix v2

The authority-test executable output directory is based on `$(MSBuildProjectDirectory)` rather than `$(SolutionDir)`. The runner also verifies that the executable exists before attempting to run it. A missing executable is a harness failure, not a Level 3 authority-test result.
