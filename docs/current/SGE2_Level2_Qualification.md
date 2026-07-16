# SGE2 Level 2 Qualification

## 1. Qualificationの目的

Level 2の完成は、単一デモの成功ではなく、Compilerの一般性、Packageの決定性、Runtime/Executor境界、物質化前拒否、Recoveryを独立した証拠群で固定する。

## 2. Freeze identity

```text
SGE2-Level2-D3D12-v1-FinalFreeze-Corpus1
Baseline commit: fc6b883b20d5428a4bf4f82b072fab15e8cb844a
Target schema: 17
Minimum Runtime: 17
Semantic corpus digest: 4b07e0725dc87f30f495794ea2ef5e245dd0d6415397761c441a9a1ca4f83c49
Accepted Package count: 54
```

Semantic corpus digestはD3DCompile前に計算し、Canonical Semantic resources、uses、programs、shader source、works、reduced dependency graph、Target profileを覆う。Debug nameと非意味的格納順は影響しない。

## 3. Final accepted Package corpus

- retained Slice 1-15 inputs: 18
- Stage G headless Compute: 1
- Stage H non-historical graphs: 3
- Stage J representative generated graphs: 25
- deterministic large graphs（10/25/50/100 Works）: 4
- Stage K interaction graphs: 3

合計54。

## 4. Stage別証拠

### Stage I - Metamorphic semantics

- 7 semantic-equivalence transformationsがPackage bytesを保存
- 7 semantic changesがexecution identityを変更
- 6 invalid transformationsをreject

### Stage J - Graph generality

- 1-6 Worksのforward-edge DAG 33,867件をindependent topology oracleと照合
- chain、fan、diamond、layered、dense、disconnected、mixed-queueの25 representative Packages
- 10/25/50/100 Worksの固定seed
- Compiler boundary invalid mutation 9件、generator contract invalid mutation 2件をreject

### Stage K - Runtime semantics

- GPU Compute/Copy値がCPU referenceと一致
- dedicated queuesとDirect fallbackが同じ値
- multiple Dynamic/External slots
- FrameLocal reuse
- cross-queue Temporal Previous/Current
- reconstruction後のPackage-owned state復旧とTemporal reset

### Stage L - Adversarial validation

- core container corruption 9件
- invalid writer input 4件
- digestを正しく再計算したmalicious D3D12 Package 40件
- 合計53件をD3D12 materialization前にreject

### Stage M - Final freeze

`44_Level2FinalFreezeTests`は54 caseを同一Process内で2回compileし、schema/runtime 17を検証し、semantic/file/execution/target-profile digestとartifact/operation cardinalityを含むmanifestを生成する。

続いてfresh child processが54 caseを再Compileし、parent/child manifestのbyte一致を要求する。

`run_level2_final.bat`はDebugとReleaseで全Qualificationを繰り返し、両manifestのbyte一致を要求する。

## 5. Runtime / Recovery corpus

- Stage G headless load/reconstruction
- Stage H WARP execution/reconstruction
- Slice 1-15 WARP execution
- Stage K semantic Readback
- controlled reconstruction
- actual RemoveDeviceとDRED collection
- AwaitingAdapter
- removed-LUID exclusion
- Compiler-free fresh-process Package rematerialization

## 6. Architectural gate

最終dependency gateは次を要求する。

- FrozenPackageCore / D3D12PackageSchemaはSemantic、Compiler、Runtime、Executor、Platform、Frontend、Scenarioへ依存しない
- TargetCompilerはRuntime、Executor、Platformへ依存しない
- Runtime、Executor、Readback test、LauncherはSemantic、Compiler、Frontend、Scenarioへ依存しない
- Classical/SDF/PGA frontendは相互に依存しない

## 7. Authoritative completion procedure

```powershell
.\run_level2_final.bat
```

成功時の最終行:

```text
SGE2 LEVEL 2 FINAL FREEZE PASSED
Semantic GPU Engine 2 Level 2 is complete.
```

この手続き中にCompiler、Schema、Runtime、Executorを修正してはならない。欠陥が見つかった場合は責任Stageへ戻り、修正後にStage Mを最初から再実行する。

## 8. Qualificationの限界

Qualificationは宣言されたLevel 2 vocabularyとWARP/利用可能Adapter条件に対する証拠である。未宣言のGPU機能、すべてのVendor/Driver、性能最適性、物理時刻決定性を主張しない。
