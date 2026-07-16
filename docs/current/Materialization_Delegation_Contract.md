# Materialization and Delegation Contract

## 1. Purpose

本書は、SGE2が「何を凍結し、何をRuntime/Backend/Driverへ委譲するか」を明確にする。

Frozen PackageはGPUの全物理状態を凍結しない。凍結対象は、正しい実行に必要な静的判断と境界契約である。

## 2. Ownership matrix

| Decision / Fact | Semantic | Target/Compiler | Package | Runtime | Backend/Driver |
|---|---|---|---|---|---|
| Resourceの論理意味 | owns | reads | does not retain source meaning | no | no |
| Read/Write/Temporal relation | owns | derives obligations | frozen as plan effects/ops | validates lifecycle | executes |
| Queue capability | no | Target owns | profileに保存 | validates | creates native queue |
| Queue assignment | no | Compiler decides | freezes | tracks completion | follows |
| Queue内order | no | Compiler decides | freezes | submits | follows |
| 必要happens-before | Semantic/Compiler | Compiler decides | Signal/Wait等でfreezes | tracks | follows |
| 物理開始時刻 | no | no | no | no | hardware/OS |
| physical instance count | intent only | Compiler decides | freezes | selects frame/temporal instance | materializes |
| Allocation class/alias relation | no | Compiler decides | freezes | lifecycle | creates heap/resource |
| GPU virtual address | no | no | no | stores runtime fact | assigns |
| Texture private tiling/compression | no | no | no | no | Driver/hardware |
| Resource state plan | use meaning | Compiler decides | freezes | validates tracked state | emits barrier |
| Descriptor index/layout | interface meaning | Compiler decides | freezes | instance stride/slot | creates handles |
| Descriptor handle value | no | no | no | stores | allocates |
| Shader source compile | Source | Target Compiler | binary only | no | may perform native backend compilation |
| Root Signature policy | interface | Compiler decides | serialized binary + canonical layout | no | creates object |
| Fence value | no | logical SignalPoint only | SignalPoint identity | assigns values | signals/waits |
| External object | declares slot meaning | freezes slot contract | slot schema | receives per invocation | binds native object |
| Present request | semantic boundary | Compiler orders | operation | invokes | API/OS completes |
| Actual display time | no | no | no | no | OS/compositor/display |

## 3. Compiler-fixed contract

Compilerは少なくとも次を固定する。

- accepted Target Profile
- Resource artifactsとinstance count
- allocation/alias plan
- Viewとdescriptor plan
- shader binaries
- Program/binding/executable artifacts
- Queue assignment
- Queue-local order
- necessary Signal/Wait/Temporal/External relations
- state transitions
- dynamic/external/surface slot schema
- Load/Frame operation sequence

Backendがこれらを再選択した場合、Compiler/Package境界違反である。

## 4. Explicitly delegated physical freedom

次はTarget APIまたはDriverへ委譲する。

- COM object identity
- GPU virtual address
- descriptor CPU/GPU handle
- Fence counter value
- Command allocator/list reuse
- native shader ISA generation
- native PSO internal representation
- texture tiling/swizzle/compression
- residency implementation
- cache behavior
- independent queuesの物理interleaving
- thermal/power scheduling
- Presentの実表示時刻

これらは「欠落情報」ではない。Package Schemaが値そのものを必要とせず、実行時にTarget contractに従って生成されることを明示しているためである。

## 5. Mechanical mappingの意味

Backendによる「機械的写像」とは、一つのPackage contractに対して一つの固定API call sequenceしか存在しないという意味ではない。

次の違いは許される。

- API object生成時の一時buffer利用方法
- Command allocator reuse
- error checkingの実装
- descriptor handleの実値
- 同じ契約を満たすAPI call batching

ただし、観測可能なPackage contractを変更してはならない。

## 6. Materialization invariants

PackageInstanceは次を満たす。

- Package execution digestとTarget Profileに対応
- current DeviceEpochに属する
- Package table identityからnative objectへ全参照が解決
- Load stream完了前にFrame streamを実行しない
- tracked stateはPackage operation before/afterと一致
- External/Surface objectはslot contractを満たす

## 7. Device recovery

Recovery後は同じPackage contractを満たす新Instanceを作る。

保持するもの:

- Package bytes
- execution digest
- Target requirement
- initial data
- shader/root/executable artifact
- operation stream

変化してよいもの:

- Device/adapter identity（互換性の範囲）
- DeviceEpoch
- GPU address
- native object/descriptor/fence
- command allocator

失われ、reset/rebindするもの:

- Temporal runtime history
- External object binding
- old completion token
- old epoch runtime-managed Surface object

## 8. Future Target schemas

Vulkan、別D3D12 profile、vendor-specific profile等を追加する場合、各Schemaはdelegation boundaryを明記する。

同じSemantic decisionがTargetごとに異なる物質化を許すことは、SGE2の境界違反ではない。Target Compilerが選択をPackageへ固定し、BackendがSchema委譲範囲を越えないことが条件である。
