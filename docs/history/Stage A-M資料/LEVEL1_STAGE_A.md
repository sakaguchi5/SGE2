# Level-1 Stage A: Slice 1-15 as general Compiler inputs

Stage A reconstructs the original Slice ladder as data. It does not add a Slice-specific compiler or executor path.

The only compilation entry remains:

```text
SemanticGraph + D3D12TargetProfile
    -> D3D12TargetCompiler::Compile
    -> Frozen Package
```

`24_Level1Scenarios` owns the input scenarios:

- Slice 1-13: progressive neutral Semantic Graphs.
- Slice 14: independent Classical and SDF inputs.
- Slice 15: independent Classical, SDF, and PGA inputs.

This produces 18 Stage-A inputs. `36_Level1ScenarioTests` verifies that every input:

- is accepted by the same SemanticAnalysis path;
- is accepted by the same general D3D12 Target Compiler path;
- produces deterministic Package bytes;
- produces a valid, decodable Frozen Package;
- contains the operations and artifacts required by that Slice contract.

It also verifies byte identity between the two Slice-14 frontends and among the three Slice-15 frontends.

Slice 13 is represented by the Slice-12 execution graph plus the complete recovery-facing Package contract: Package-owned resources are reconstructible, Temporal resources are marked, and External resources require rebind. Actual removal and reconstruction remain runtime qualification work and are not duplicated inside the scenario library.

Stage A intentionally does not execute all 18 Packages on D3D12. That is Stage B.
