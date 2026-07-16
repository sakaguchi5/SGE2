# Package Runtime Contract v17

## 1. Scope

RuntimeはFrozen Packageを保持し、PackageInstanceのlifecycle、FrameInvocation validation、FrameSubmission、Device epoch、Recoveryを管理する。

RuntimeはSemantic Graph、Compiler plan、Shader sourceを知らない。

As-built source:

```text
09_PackageRuntime/PackageRuntime.h
09_PackageRuntime/PackageRuntime.cpp
10_D3D12Executor/*
```

Minimum Runtime versionは17。

## 2. Main API boundary

```cpp
LoadedPackage LoadPackage(FrozenExecutablePackage, IPackageExecutor, optional ISurfaceHost)
FrameSubmission Submit(LoadedPackage&, IPackageExecutor&, FrameInvocation)
DeviceRecoveryReport RecoverDevice(LoadedPackage&, IPackageExecutor&, DeviceRecoveryMode)
```

Runtime APIに`Execute(SemanticGraph)`またはCompiler入力を渡す統合APIを置かない。

## 3. Frozen Package and PackageInstance

**FrozenExecutablePackage**

- immutable bytesとdecoded Core sections
- Source/Compiler非依存
- Deviceに属さない

**PackageInstance**

- 特定Device epochに属する
- Resource、View、Descriptor、Root Signature、PSO、Queue、Fence tracking等
- Device lossで無効化される
- Frozen Packageから再構築可能

同じPackageから再構築されたInstanceは、同じobject addressまたはhandleを持つ必要はない。

## 4. FrameInvocation

Fields:

- frameNumber
- DynamicDataBinding[]
- ExternalResourceBinding[]

DynamicDataBinding:

- public slot ID
- bytes

ExternalResourceBinding:

- public slot ID
- IExternalResource
- ICompletionToken availableAfter

Runtime/ExecutorはSubmit前に次を検証する。

- required slotが全て存在
- duplicate slotなし
- unknown slotなし
- Dynamic bytesはexact size/alignment contract
- External kind/format/sizeはPackage contract一致
- ResourceとCompletionTokenがcurrent DeviceEpochに属する
- Recovery後に旧epoch External objectを受理しない

FrameInvocationはPackage bytesを書き換えない。

## 5. FrameSubmission

FrameSubmissionは実行後のRuntime事実を返す。

- deviceEpoch
- frameSlot
- framesInFlight
- reusedSlotFenceValue
- temporalPreviousInstance / temporalCurrentInstance
- temporalDependencyFenceValue
- QueueCompletion[]
- ExternalRelease[]

ExternalRelease.safeAfterは外部所有者が再利用前に待つcompletion tokenである。

## 6. Device epoch

Device epochは、Resource、CompletionToken、PackageInstanceがどのDevice lifetimeに属するかを識別する単調なidentityである。

- Load成功時にactive epochを持つ
- Recovery成功時にnew epochへ進む
- old epoch objectは新Instanceへbinding不可
- Package bytesとexecution digestはepochに依存しない

## 7. Runtime states

```text
Active
Lost
AwaitingAdapter
```

**Active**: Submit可能。

**Lost**: device removal検出後。通常Submit不可。

**AwaitingAdapter**: removed adapterを除外した上で互換Adapterが得られず、再取得待ち。

Runtimeは互換Adapterが得られない場合に、無関係なTargetへ勝手にfallbackしてPackage contractを変えてはならない。

## 8. Recovery modes

```text
ControlledRebuild
RecoverDetectedLoss
ForceRemovalForTest
RetryAdapterReacquisition
```

Recovery reportは次を含む。

- previous/new epoch
- removal reason
- DRED breadcrumb/page-fault counts
- removed adapter LUID
- state before/after
- adapter reacquired
- package objects rebuilt
- temporal history reset
- external rebind required

## 9. Recovery semantics

Recovery成功の条件:

- compatible Target/Adapter
- Frozen Package validation済み
- Load streamからPackage-owned object再物質化
- Runtime-managed Surfaceを新Deviceへ接続
- Temporal historyを未初期化状態へreset
- External slotsをunboundに戻し、再bind要求
- removed adapter LUIDを再選択候補から除外

Recoveryは旧GPU memory内容の保存を保証しない。Package initial dataまたは明示Runtime入力から再構築できない状態を黙って復元したことにしてはならない。

## 10. Surface contract

Surfaceを要求するPackageではISurfaceHostが必須。Headless PackageではSurface hostなしでLoadできる。

Runtime/ExecutorはPackageのSurfaceSlotに従い、AcquireとPresentをOperation stream外で暗黙に追加してはならない。

## 11. Error boundary

RuntimeErrorは少なくともstageとmessageを持つ。

Errorは次の境界で区別する。

- Package Core/Schema validation error
- Load/materialization error
- Invocation validation error
- Submit error
- Device loss / recovery error

Source Work位置はRuntimeの必須知識ではない。必要な場合はoptional DebugMapで対応する。

## 12. Prohibited behavior

- Semantic Graphへのfallback
- RuntimeでのQueue/State/Alias/Binding再Planning
- missing External stateの推測
- old epoch resourceの黙認
- PresentSurface Operationなしの暗黙Present
- recovery中のPackage bytes変更
- recovery成功前のTemporal history継続
