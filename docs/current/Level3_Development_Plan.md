# SGE2 Level 3 Development Plan

## 1. Level 3の目的

Level 2は、一つのSemantic Graphから一つの完全で正しいCanonical Planを生成した。

Level 3の目的は、同じSemantic obligationを満たす複数の合法Planを生成し、Plannerとは独立したVerifierで正しさを確認し、明示Policyに基づいて一つを選ぶことである。

```text
Semantic Graph
  ↓
Semantic Obligation
  ↓
Candidate Planner(s)
  ↓
Execution Plan IR[]
  ↓
Independent Plan Verifier
  ↓
Cost Vector / Policy Selection
  ↓
Selected Verified Plan
  ↓
Frozen Package
```

Level 3は「最適化を入れる」だけのLevelではない。正しさ、候補生成、検証、評価、選択を分離するLevelである。

## 2. 非目的

Level 3初期では次を目的にしない。

- 全組合せの真の大域最適解
- 機械学習Cost model
- Package composition/streaming
- 新Resource/Rendering vocabularyの大量追加
- Runtimeによる同一PackageのPlan変更
- Backendによる再Planning
- Profile結果によるPackage bytesの暗黙変更

## 3. Level 2 freezeの扱い

Level 2 Finalを変更しない。

Level 3系列はLevel 2のCanonical Planを`CanonicalSafePlan`として再現可能にする。Candidate generation、Cost model、profileが失敗した場合でもSafe Planを生成できなければならない。

## 4. Core internal types

### 4.1 SemanticObligation

Target-specific配置を含まない、Planが満たすべき義務。

- Work identityとkind
- required ResourceUse
- RAW/WAR/WAW
- explicit semantic dependencies
- Temporal Previous/Current
- External acquire/release
- Present boundary
- Resource lifetime lower bounds
- required View/state class
- Program parameter/binding obligation
- alias compatibility constraints

### 4.2 ExecutionPlanIR

Compiler内部型。RuntimeまたはPackage ABIへ公開しない。

- canonical Work schedule
- Queue assignment
- Queue-local batches
- SignalPoint/Wait graph
- Resource instance plan
- lifetime intervals
- allocation assignment
- alias activation plan
- state-cell transitions
- View/descriptor assignment
- Binding layout
- Executable specialization
- Load/Frame abstract operation order

### 4.3 PlanIdentity

Plan IRをcanonical encodeしたdigest。Package execution digestとは別。

同じPlan identityから同じPackage bytesを生成することをQualificationする。

### 4.4 VerificationReport

- verified / rejected
- violated obligation identity
- counterexample pathまたはresource/use/work
- stage
- diagnostic code

## 5. Independent Plan Verifier

VerifierはPlannerの内部補助関数を信頼しない。

最低限検証する。

- 全Workがexactly once schedule
- dependency/hazard topological order
- Queue capability
- cross-queue SignalPoint producer/consumer整合
- wait cycleなし
- Resource instance selection
- FrameLocal/Temporal index law
- lifetime overlapとalias safety
- state-before/state-after continuity
- Copy exact View semantics
- binding completenessとregister identity
- External/Surface boundary
- final Present uniqueness
- Package lowering前のartifact cardinality bounds

Verifierを通過していないPlanはPackageへLoweringしない。

## 6. Candidate axes

候補軸は段階的に導入する。

### Axis 1 - Topological schedule

同じpartial orderを満たす複数canonical topological schedule。

### Axis 2 - Queue assignment

- Compute: Direct / dedicated Compute
- Copy: Direct / dedicated Copy
- queue transitionを伴う合法割当

### Axis 3 - Allocation and alias

- conservative committed
- placed without alias
- legal alias groups

### Axis 4 - Frames in flight / physical instances

Target policy許容範囲で候補化。ただしTemporal/FrameLocal semanticsをVerifierで確認する。

### Axis 5 - Binding layout

Canonical baselineの後に導入。Root-signature costとdescriptor countを比較する。

### Axis 6 - Executable specialization

同一Programのspecialization共有/分離候補。

一つのStageで全軸を同時に探索しない。

## 7. Cost vector

最初から単一weighted scoreへ潰さない。

```text
peakAllocationBytes
allocationCount
aliasGroupCount
barrierCount
transitionCount
queueHandoffCount
waitCount
signalCount
operationCount
descriptorCount
rootParameterCost
physicalInstanceCount
estimatedParallelSlack
compileWorkUnits
```

Policyは次の方法を選べる。

- lexicographic priority
- hard constraint + lexicographic
- Pareto frontier + deterministic tie-break
- explicit weighted score（後段）

Tie-breakはPlanIdentity等のstable orderで決定し、candidate enumerationはProcess/Build configurationに依存してはならない。

## 8. Policy model

初期Policy:

- CanonicalSafe
- MinimizeMemory
- MinimizeQueueHandoffs
- PreferDedicatedQueues
- MinimizeOperationCount

PolicyはTarget capabilityとは別型にする。

## 9. Profile feedback

ProfileはPackage外の測定記録として保存する。

```text
package execution digest
target profile digest
adapter/driver fingerprint
measurement scenario digest
sample count
queue/work timings
memory observations
```

Profileを使って別Planを選ぶ場合は新しいCompileを行い、新Packageと新execution digestを生成する。Runtimeが既存Packageを変更してはならない。

## 10. Proposed stages

### Stage N - Vocabulary and identity freeze

- SemanticObligation型
- ExecutionPlanIR型
- PlanIdentity canonical encoding
- Level 2 Canonical PlanをIRへ移す
- bytes regressionを維持

### Stage O - Independent verifier core

- schedule/dependency/hazard
- Queue capability
- state continuity
- diagnostics
- intentionally invalid Plan corpus

### Stage P - Queue candidates

- Direct/dedicated Compute/Copy候補
- SignalPoint/Wait generation
- verifierとsemantic Readback

### Stage Q - Allocation candidates

- committed/placed/alias候補
- lifetime/alias verifier
- peak memory Cost vector

### Stage R - Candidate manifest and deterministic selection

- Candidate list
- PlanIdentity
- Cost vector
- Policy/tie-break
- fresh-process/Debug/Release identity

### Stage S - Profile record and offline reselection

- measurement sidecar
- profile provenance
- profile-guided recompile
- Runtime mutation禁止の証明

### Stage T - Level 3 qualification and freeze

- fixed multi-plan corpus
- independent verifier adversarial corpus
- semantic equivalence across selected Plans
- Safe Plan fallback
- final manifest freeze

Stage名はLevel 3実装開始時に正式確定する。Level 2のStage A-M historyは変更しない。

## 11. Completion criteria

Level 3完成には次をすべて要求する。

1. 同じSemantic Graphから2個以上の異なるvalid Planを生成。
2. 全Candidateを独立Verifierがaccept/reject。
3. 同じSemantic obligationを満たすvalid PlanのGPU観測結果が一致。
4. 複数Policyが異なるPlanを選択。
5. Candidate enumerationと選択がbyte-deterministic。
6. Invalid Plan mutation corpusをPackage lowering前にreject。
7. CanonicalSafePlanへ常にfallback可能。
8. Profile-guided selectionは新Packageを生成し、Runtime/Backendは再Planningしない。
9. same-process、fresh-process、Debug、ReleaseのCandidate manifestが一致。
10. Level 2 fixed corpusが回帰しない。

## 12. Architectural rule

```text
Planner may propose.
Verifier must prove contract satisfaction.
Policy may select only verified Plans.
Package freezes the selected Plan.
Runtime and Backend never choose again.
```
