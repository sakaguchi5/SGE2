# SGE2 Level Model v0.2

## 1. Levelの評価軸

SGE2のLevelは、機能数ではなく、次の一般化の深さで定義する。

- 入力Resource/Workの個数をどこまで固定しないか
- Graph形状をどこまで固定しないか
- 一つの意味に対して何個の合法Planを扱えるか
- 動的変化を値、Plan、有限構造、任意構造へ分類できるか
- Domain固有の数学表現をどこまで保持できるか
- 規模、運用、回復性をどこまで成立させるか

各Levelは下位Levelを捨てない。下位Levelは、上位一般化の比較基準、safe fallback、regression corpusとして残す。

## 2. Level 1 - Fixed Vertical Contract

### 定義

特定の複雑なGPU実験について、正しい責務境界を保ったまま、SemanticからFrozen Package、Runtime、D3D12 Executorまでの垂直経路を成立させる。

### 完成事実

Slice 1-15で次を通過した。

- Raster / Compute / Copy
- multiple queues / handoff
- Buffer / Texture uploadとReadback
- Depth
- FrameLocal / Temporal
- Aliasing
- External Acquire / Release
- controlled/actual device lossとreconstruction
- Classical / SDF / PGA frontend equivalence

### 限界

Compilerが既知の実験形状を知っている。Resource/Work cardinalityとGraph形状は一般化されていない。

## 3. Level 2 - Finite-Vocabulary General Graph Compiler

### 定義

宣言済みの有限Semantic vocabulary内で、任意個数のResource/Workと任意の有限DAGをaccept/rejectし、一つの完全で正しいCanonical Planを生成する。

### 完成事実

- D3D12 Target Schema 17 / Runtime 17
- 固定accepted corpus 54 Packages
- Semantic corpus digest: `4b07e0725dc87f30f495794ea2ef5e245dd0d6415397761c441a9a1ca4f83c49`
- same-process / fresh-process / Debug / Release manifest byte identity
- generated DAG 33,867件をindependent oracleと照合
- adversarial Package 53件を物質化前にreject
- WARP semantic Readback、controlled/actual removal、Compiler-free rematerialization

### Level 2が証明したもの

高水準のResource/Work/Effect/Temporal/External意味を保持すれば、人間が手書きしていたGPU実行判断の相当部分を、有限一般Graphに対するCompiler処理として導出できる。

### Level 2が証明していないもの

- 複数合法Planの生成
- 最適Planの選択
- 任意GPU機能の受理
- 任意の動的Graph変化を再Compileなしで処理
- 物理実行時刻の決定論化

## 4. Level 3 - Verified Multi-Plan Selection Compiler

### 定義

同じSemantic obligationを満たす複数の合法Execution Planを生成し、Plannerと独立したVerifierで正しさを確認し、Policy/Costに基づいて一つを選ぶ。

### 必須要素

- Semantic Obligation IR
- Candidate Execution Plan IR
- Independent Plan Verifier
- Canonical Safe Plan
- deterministic candidate enumeration
- Cost vectorとPolicy
- Candidate manifest
- Profile recordと再Compile

### 完成条件

- 同じGraphから2個以上の異なる合法Plan
- 全PlanがVerifierを通過
- Policy変更で選択Planが変化
- 全Planの観測結果が同じSemantic contractを満たす
- Safe Planへ常にfallback可能
- fresh-process / Debug / ReleaseでCandidate manifest一致

## 5. Level 4 - Package Families and Bounded Dynamism

### 定義

単一Packageを超え、契約付きPackage composition、variant、incremental compile、streaming、bounded conditional execution、GPU-generated workを扱う。

### 変化の分類

```text
値の変化                  -> FrameInvocation
Planの変化                -> Level 3 Package variant
有限に列挙できる構造変化  -> Conditional region / indirect work
任意の構造変化            -> Incremental recompile / Package composition
```

### 主課題

- import/export slot
- Package linker
- Standalone / Linked Package
- Library Package
- stable semantic identityとdependency digest
- residency contract
- bounded conditional region
- ExecuteIndirect / DispatchIndirect

## 6. Level 5 - Representation-Preserving Semantic Compiler

### 定義

Classical、SDF、PGA等のDomain表現を早期に消去せず、Semanticまで保持し、同じ意味に対する複数Loweringを生成・比較する。

### 先に必要なもの

性能比較より先にObservation Contractを定義する。

- byte/image一致
- epsilon以内のsurface一致
- topology一致
- visibility一致
- collision一致
- numerical stability contract

### 主課題

- representation-preserving semantic nodes
- Classical/SDF/PGA Lowering candidates
- CPU展開 / Compute / Vertex / Pixel等の実行候補
- 意味等価性Verifier
- 誤差を含むCost vector

## 7. Level 6 - Production-Scale Workload Compiler

### 定義

理論的境界を維持したまま、実ゲーム規模、運用、長時間安定性を成立させる。

### 主課題

- 大規模Graph partition
- parallel / incremental analysis
- Package cache
- Asset system境界
- hot reload
- memory pressure / residency
- fallback Package
- telemetry
- long-running recovery
- 多数Shader/Material variant
- optional multi-GPU target profile

## 8. Level 7 - Research Frontier

Level 7は通常の「完成Level」よりResearch Frontierとして扱う。

### 研究対象

- constraint solving / integer programming
- Plan synthesis
- learned cost model
- representation synthesis
- Work fusion / splitting
- precision / approximation selection
- 誤差、時間、memory、energyの共同最適化
- 新しい数学表現とGPU executionの共設計
- formal proof of plan correctness

### 評価

- 人間が手段を書かずに済む範囲が増えたか
- Compilerが既知手法以外の有効Planを発見したか
- 既存方式より理論的または実測的に優れたか
- 新しい数学表現が実行上の意味を持ったか

## 9. Level間の主要な分水嶺

```text
Level 1 -> Level 2
特定実験Compilerから有限一般Graph Compilerへ

Level 2 -> Level 3
一つの正しいPlanの導出から、複数Planの検証と選択へ

Level 4 -> Level 5
表現消去Loweringから、数学表現を実行候補として保持するCompilerへ
```
