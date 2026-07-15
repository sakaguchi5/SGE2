# Slice 13 — Device Loss and Reconstruction

## Goal

Prove that the Frozen Executable Package is a durable device-materialization description while treating device loss as a stateful Runtime event.

Slice 13 does not add a hidden “recovery command” to the Package. Reconstruction replays the already validated Load Operation stream against an eligible adapter.

## Public Runtime contract

```cpp
DeviceRuntimeState
  Active
  Lost
  AwaitingAdapter

DeviceRecoveryMode
  ControlledRebuild
  RecoverDetectedLoss
  ForceRemovalForTest
  RetryAdapterReacquisition
```

`DeviceRecoveryReport` records:

- previous and new device epoch
- recovery mode
- state before and after
- removal HRESULT
- removed adapter LUID
- DRED breadcrumb and page-fault allocation counts
- whether another adapter was acquired
- whether Package-owned objects were rebuilt
- whether Temporal history was reset
- whether External resources must be rebound

## Controlled reconstruction

Controlled reconstruction waits for Direct and Copy queue completion, tears down the live object graph in reverse dependency order and rematerializes the same Frozen Package. It then increments the device epoch.

The previous External Resource and completion token remain valid C++ objects, but their epoch no longer matches the Runtime and they are rejected at invocation validation.

## Actual device loss

Production code enters this path with `RecoverDetectedLoss` after a D3D12/DXGI operation reports device removal. The deterministic WARP test uses `ForceRemovalForTest`, which calls `ID3D12Device5::RemoveDevice` before executing the same loss-handling path.

The Runtime then:

1. enters `Lost`
2. reads `GetDeviceRemovedReason`
3. captures DRED only because the device is removed
4. records and excludes the removed adapter LUID
5. tears down without waiting on removed fences
6. enumerates eligible adapters
7. either rematerializes the Package and returns to `Active`, or enters `AwaitingAdapter`

`AwaitingAdapter` is a successful recovery state, not an arbitrary HRESULT failure. Submit and External creation are rejected until reacquisition succeeds.

## Adapter retry

`RetryAdapterReacquisition` is legal only from `AwaitingAdapter`. The caller invokes it after an adapter-topology change or another recovery policy event.

If no eligible adapter exists, the Runtime remains `AwaitingAdapter`. If one exists, Package Load Operations replay, the epoch increments, Temporal state resets and External rebind becomes mandatory.

## Teardown dependency graph

The explicit release sequence is:

```text
External bindings
Command lists and allocators
Back buffers
Swap chain
Package resources and placed heaps
Upload/readback resources
PSOs and root signatures
Descriptor heaps
Fences and events
Direct and Copy queues
D3D12 device
DXGI factory
```

Back buffers must be released before their swap chain, and the swap chain before the Direct queue used to create it.

## State-model correction

D3D12 Buffers are represented as native `COMMON` at creation. Package-owned default/placed Buffers 0, 4, 5, 8 and 9 also declare `COMMON` as their initial Package state.

Five explicit Copy-queue preparation transitions are emitted before immutable uploads:

```text
Resource 0: COMMON → CopyDestination
Resource 4: COMMON → CopyDestination
Resource 5: COMMON → CopyDestination
Resource 8: COMMON → CopyDestination
Resource 9: COMMON → CopyDestination
```

The validator requires this contract. Copy-queue transitions remain limited to `COMMON`, `CopySource` and `CopyDestination`.

## Deterministic WARP test split

The WARP test deliberately does not claim that a removed WARP adapter can be recreated in the same process.

- controlled reconstruction proves Package replay and epoch reset
- actual removal proves DRED, teardown, LUID exclusion and `AwaitingAdapter`
- a child process proves fresh-device rematerialization from the same Package bytes

## Final completion-boundary correction

The final Direct queue completion used by recovery is emitted after `PresentSurface`:

```text
EndQueueBatch
PresentSurface
SignalQueue
ReleaseExternal
```

This distinguishes ordinary Resource-use completion from the stronger lifecycle boundary required before BackBuffer, SwapChain, Queue, and Device teardown.

D3D12 Debug Layer and DRED configuration are process-level and execute once before the first Device, not on every Package rematerialization.
