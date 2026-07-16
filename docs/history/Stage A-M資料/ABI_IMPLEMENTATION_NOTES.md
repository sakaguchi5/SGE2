# Slice 13 ABI implementation notes

The D3D12 target schema version and minimum Runtime version remain 13.

No new Frozen Package opcode is required for device recovery. Recovery belongs to the Runtime lifecycle; Package rematerialization is a replay of the existing Load Operation stream.

## ABI-visible correction

Default and placed Buffer initial states are now `COMMON`, and the Load stream includes five explicit `InitializeState(COMMON, CopyDestination)` operations before Buffer upload. Because this changes execution-affecting Package bytes, the Package digest and byte size differ from the earlier defective Slice 13 build while remaining Schema V13.

The validator enforces:

- Resources 0, 4, 5, 8 and 9 are Package-owned Buffers beginning in `COMMON`
- exactly five Copy-queue `COMMON → CopyDestination` preparation operations
- all Copy-queue transitions use only `COMMON`, `CopySource` or `CopyDestination`

## Runtime-only state

The following are not serialized into the Package:

- current `DeviceRuntimeState`
- device epoch
- active and excluded adapter LUIDs
- DRED snapshot counts
- adapter reacquisition result
- live External bindings and completion tokens

They are Runtime facts surrounding execution of the immutable Package.

## Slice 13 final lifecycle corrections retained by Slice 15

D3D12 Debug Layer and DRED settings are process-level configuration. They are guarded by `std::call_once` and are not repeated by Device rematerialization.

The final Direct queue lifecycle boundary is:

```text
EndQueueBatch
PresentSurface
SignalQueue
ReleaseExternal
```

This makes the waited completion include Present before SwapChain, Queue, and Device teardown. It changes execution-affecting Operation order and therefore Package bytes/digest, but does not require a Target Schema version increase because it corrects the emitted V13 program rather than changing record encoding.

## Slice 14 ABI impact

Slice 14 adds no Package section, opcode, record field, or Runtime ABI. Frontend identity is deliberately absent from execution-affecting Package data. Identical Semantic execution therefore serializes to identical V13 Package bytes.


## Slice 15 ABI impact

Slice 15 adds no Package section, opcode, record field, Target Schema version, or Runtime ABI. PGA point/line coordinates, join/meet operations, and frontend identity are source-domain facts and are deliberately absent from execution-affecting Package data. The three equivalent frontends therefore serialize to the same existing V13 Package bytes.
