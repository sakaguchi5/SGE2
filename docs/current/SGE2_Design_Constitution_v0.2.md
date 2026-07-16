# Semantic GPU Engine 2 設計憲法 v0.2

## 0. 文書の地位

本書は、Semantic GPU Engine 2（SGE2）のLevel 2完成後に確定した最上位の設計規範である。

v0.1はLevel 1以前の設計仮説と出発原則を保存する歴史文書であり、削除または改変しない。v0.2はLevel 2で実証された事実、今回明確になった用語の境界、およびLevel 3以降で破ってはならない責務分離を定める。

規範の優先順位は次の通りとする。

1. 本設計憲法
2. Level Capability Constitution
3. Container ABI、Target Schema、Runtime Contract
4. Qualification Specification
5. Stage文書、実装ノート、README

本文で「必須」「禁止」と記す事項は規範である。「推奨」は、代替案と理由を文書化した場合にのみ変更できる。

## 第1章 目的

### 第1条 最上位目的

SGE2の目的は、DirectX 12を置き換えることでも、既存ゲームエンジンの機能数を再現することでもない。

目的は、GPUワークロード全体について、次の境界を構築することである。

```text
人間またはDomainが記述する目的と論理
    ↓
Semantic Graph
    ↓
Target Capability / Policy
    ↓
Compilerが導出する静的実行計画
    ↓
Frozen Executable Package
    ↓
Runtime Lifecycle / Invocation
    ↓
BackendによるTarget APIへの物理化
```

SGE2は、GPUの物理実行を決定論化するシステムではない。SGE2は、正しい実行に必要な静的判断と動的境界契約をCompilerが導出し、検証可能なFrozen Packageとして決定論的に生成するCompiler体系である。

### 第2条 非目的

少なくともLevel 2まで、次は目的ではない。

- GPU内部Schedulerの支配
- Queueの正確な開始・終了時刻の固定
- OS compositorによる正確な表示時刻の固定
- hardware-private tiling、圧縮形式、cache layoutの固定
- 一つのPackageで未来の任意Graph変化を処理すること
- あらゆるGPU機能を受理すること
- 複数APIまたは複数Vendorへの自動互換性
- 既存ゲームエンジンとの機能数競争
- 製品向け最適化をLevel 2の完成条件に含めること

## 第2章 用語の厳密化

### 第3条 「一般」の意味

SGE2で「一般Compiler」とは、宣言された有限Semantic vocabularyとTarget Profileの範囲内で、Resource数、Work数、Source ID、非意味的格納順、および有限DAGの接続形状を固定しないCompilerを指す。

「一般」は、あらゆるGPU処理を受理することを意味しない。Compilerは各入力について、次のいずれか一方を行わなければならない。

1. 入力全体を受理し、必要な判断を欠落なくPackageへ固定する。
2. D3D12物質化前に、名前付き診断で入力を拒否する。

Unsupportedな形状をBackendの推測や便利機能で通してはならない。

### 第4条 四種類の決定性

決定性を次の四種類に分ける。

**Compilation determinism**

同じCanonical Semantic Graph、Target Profile、Compiler Policy、Compiler contract versionから、byte単位で同じPackageを生成する。

**Contract determinism**

同じPackage bytesは、同じ静的実行契約と同じ実行影響情報を意味する。

**Semantic safety under nondeterminism**

物理的な実行速度や独立Queue間のinterleavingが変化しても、Packageが定めるhappens-before、Resource state、External、Temporal、Present契約を破らない。

**Physical timing determinism**

各Workの開始・終了時刻、GPU clock、独立Queue間interleaving、Presentの実表示時刻は保証対象外である。

### 第5条 四種類の順序

順序を次の四種類に分ける。

- Serialization order: canonical Package bytes内の記録順序
- Queue-local submission order: 同じQueueへ投入するOperation順序
- Semantic happens-before: Signal/Wait、Temporal、External等で正しさに必要な先行関係
- Physical execution order: 依存しないOperation間の実機上の順序

Packageは最初の三つを必要な範囲で固定する。依存しないOperation間に物理的全順序を要求してはならない。

### 第6条 「独立」の意味

Frozen Packageの独立性とは、実行時に次を必要としないことである。

- FrontendまたはDomain source
- Semantic Graph
- Semantic Analysisの内部型
- Target Compiler
- Shader source、entry point、include path
- 元Asset file
- Source側のdebug name

次への依存は明示契約として許される。

- 対応Runtime version
- 対応Target capability
- OS、Target API、GPU、Driver
- Surface host
- FrameInvocation
- Packageが宣言したExternal binding

PackageはSource-independent、Compiler-independentであるが、Environment-independentではない。

### 第7条 「完全」の意味

「完全な実行成果物」とは、Target Schemaの範囲内でBackendが上位の意味判断を再実行しなくてよい、完全な静的実行契約を意味する。

完全性は、GPUの全物理状態を固定することを意味しない。

## 第3章 七つの責務領域

### 第8条 Domain / 自由領域

Domain領域は、アプリケーションまたは数学的理論が自然に持つ表現を所有する。

例:

- Classical mesh
- SDF
- PGA / CGA
- Voxel
- Procedural geometry
- Experiment scene
- Application-specific quality target

Domain領域はD3D12_RESOURCE_STATE、Queue、Descriptor、PSO、Fence、Heap placement等のTarget手段を直接指定してはならない。

### 第9条 Semantic領域

Semantic領域は、GPU上で成立させたい論理的事実を所有する。

- Resourceの存在、論理shape、format meaning、lifetime intent
- Workの存在
- Workが何を読み、何を書くか
- Programが要求する意味的parameter
- Work間の明示的またはResource hazard由来の関係
- Temporal relation
- 外部へ公開する境界

Semantic領域はQueue、allocation identity、native state、descriptor index、shader binary、PSO specializationを所有してはならない。

### 第10条 Target Capability / Policy領域

Target領域は、Compilerが選択できる手段の範囲と目的を所有する。

**Capability**はTargetが可能なことを表す。

- Queue topology
- format support
- barrier model
- descriptor limits
- shader target
- frames-in-flightの上限
- supported operation version

**Policy**は許された選択の目的または制約を表す。

- dedicated queue利用可否
- memory/latency/throughput優先度
- fallback許可
- compile-time予算

Capability、Policy、Compilerが実際に選択したdecisionを同一視してはならない。

### 第11条 Compiler Planning領域

CompilerはSemantic factsとTarget capability/policyから、次を決定する。

- Canonicalization
- validationとtarget feasibility
- dependencyとhappens-before
- execution schedule
- Queue assignment
- Resource physical-instance count
- lifetime interval
- allocationとalias plan
- state-cell plan
- Queue synchronization
- Temporal、External、Present boundary
- shader target compilation
- Program binding layout
- executable specialization
- operation lowering
- Package canonicalizationとdigest

Compiler内部IRは外部ABIではない。RuntimeまたはBackendへ公開してはならない。

### 第12条 Frozen Contract領域

Frozen PackageはCompilerが選んだ静的実行契約を保存する。

- 固定幅、version付き、section化されたbinary
- Source/Compiler非依存
- canonical byte order
- Target profile明示
- digest付き
- 単独静的検証可能
- 別Processで読込可能
- Package-owned objectの再物質化情報を保持

PackageにBackendの推測を要求する欠落情報があってはならない。ただしTarget Schemaが明示的にBackendまたはDriverへ委譲した物理自由度は欠落ではない。

### 第13条 Invocation / Runtime Lifecycle領域

Runtime領域は、固定Packageに含めるべきでない動的値とlifecycleを所有する。

- frame number
- dynamic bytes
- External resource object
- External completion token
- Surface host
- Frame slot
- Fence実値
- Device epoch
- Active/Lost/AwaitingAdapter state
- Recovery手続き

値の変化によってPackageを再Compileしてはならない。Semantic構造またはPlanを変える場合は、別Packageを生成する。

### 第14条 Backend / Physical Materialization領域

BackendはPackage contractをTarget API objectとcommandへ機械的に写像する。

Backendに許されるもの:

- COM objectの具体的生成手順
- Descriptor handleの実値
- Fenceの実値
- Command allocator再利用
- GPU virtual address
- API error、DRED情報の取得
- Target Schemaが委譲したhardware-private layout

Backendに禁止するもの:

- Workの意味的並べ替え
- Queue assignmentの再選択
- Resource lifetimeまたはalias対象の再判断
- abstract state planの補完
- External incoming/outgoing contractの推測
- shader compile option、binding layout、Present有無の再決定

Backendの自由は、Target Schemaが明示的に委譲した物理化自由度に限定する。

## 第4章 唯一の依存方向

### 第15条 基本依存

```text
Domain
  ↓
Semantic
  ↓
Target Capability / Policy
  ↓
Compiler Planning
  ↓
Frozen Contract
  ↓
Invocation / Runtime Lifecycle
  ↓
Backend / Physical Materialization
```

Runtime入力はFrozen Packageを書き換えない。

### 第16条 禁止依存

- Semantic -> Compiler内部型
- Semantic -> RuntimeまたはBackend
- Frozen Contract -> Source IRまたはCompiler実装
- Runtime -> Semantic Graph
- Backend -> Semantic Graph、Compiler plan、Shader source compiler
- Frontend A -> Frontend B

禁止依存はProjectReferenceとCI/qualificationで物理的に検査する。

## 第5章 Packageと物質化

### 第17条 Packageが固定するもの

- Target profileとrequired runtime
- Resource artifactとphysical-instance count
- allocation class、size、alignment、alias relation
- Viewとdescriptor plan
- shader binary
- Program interface digestとbinding layout
- executable specialization
- Queue-local orderと必要なhappens-before
- State transition
- Dynamic、External、Surface slot schema
- Load/Frame operation stream

### 第18条 Packageが固定しないもの

- 実GPU上の正確な開始・終了時刻
- hardware-private texture tilingまたはcompression
- GPU virtual address
- Descriptor handle
- Fenceの実値
- Command allocator identity
- Driver内部のnative pipeline representation
- OS compositorの表示時刻

### 第19条 Recovery

Recoveryは、同じD3D12 object、handle、addressを復元することではない。

Recoveryとは、同じFrozen Packageの意味を満たす新しいPackageInstanceを、互換Target上へ再物質化することである。

Recovery時:

- Package bytesは不変
- Package-owned objectはLoad streamから再作成
- Device epochを更新
- Temporal historyをreset
- External resourceは再bindを要求
- Surface/runtime-managed objectはRuntime contractに従う
- 互換Adapterが得られない場合はAwaitingAdapterを維持

## 第6章 Compiler完全性と拒否

### 第20条 Accept / Reject原則

CompilerとPackage validatorは、理解できないものを推測して続行してはならない。

- Unsupported Semantic shapeはCompilerがrejectする。
- Malformed PackageはCoreまたはTarget Schema validatorがrejectする。
- Unknown required sectionはrejectする。
- Unknown operationまたはoperation versionは常にrejectする。
- Device removalをPackage validatorとして利用してはならない。

### 第21条 Level相対の完全性

Compiler完全性はLevel Capability Constitutionに相対的である。Level 2における完全性は、Level 2 vocabulary内の全合法入力を受理し、範囲外入力を物質化前に拒否することである。

## 第7章 テストと証拠

### 第22条 Qualificationの独立性

完成条件は「デモが動く」ことではない。少なくとも次を分離して証明する。

- Semantic validation
- metamorphic equivalence
- generated graph coverage
- Package canonicalization
- corruption/adversarial rejection
- schema/runtime version
- Source/Compiler-free fresh-process load
- WARP executionとReadback
- controlled/actual device removal
- Debug/Release byte identity
- architectural dependency gate

### 第23条 凍結成果物

完成Levelは、基準commit、Target Schema、Runtime version、accepted corpus、semantic corpus digest、authoritative commandを持つFreeze identityで固定する。

完成Levelの実装は、文書整理やnamespace修正のために変更しない。後続Levelは別開発系列で進める。

## 第8章 Level 3以降

### 第24条 Planの生成と検証の分離

Level 3以降、複数Candidate Planを生成するPlannerと、そのPlanがSemantic obligationを満たすか検証するPlan Verifierを分離する。

Verifierを通過していないPlanをPackageへLoweringしてはならない。

### 第25条 最適性と正しさの分離

- 正しさは必須である。
- 最適性は目的関数、Target、計測条件に相対的である。
- Level 2のCanonical Safe Planを常にfallbackとして保持する。
- Cost modelが失敗しても正しさを失ってはならない。

## 第9章 設計変更手続き

### 第26条 変更記録

本憲法、Container ABI、Target Schema、Runtime Contractを変更する場合は、次を記録する。

- 変更理由
- 破られる旧原則または旧contract
- 新しい原則
- 代替案
- 長期的影響
- Container/Schema/Runtime versionへの影響
- 既存Qualification corpusへの影響

### 第27条 最終原則

SGE2の中心はBackendでもSemantic Graph単体でもない。

中心は、意味から導かれた静的判断と動的境界契約を、欠落なく、検証可能に、決定論的なbytesとして保存するFrozen Executable Packageである。

Domainは目的を記述する。Semanticは論理を記述する。Targetは可能性とPolicyを記述する。Compilerは手段を選択する。Packageは選択結果を保存する。Runtimeは変化する値とlifecycleを管理する。Backendは保存された契約をTarget APIへ物理化する。
