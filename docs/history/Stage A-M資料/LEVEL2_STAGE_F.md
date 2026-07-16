# Level 2 Stage F — Explicit Signal Identity and Boundary ABI

Stage F removes synchronization guesses from the Executor.

## ABI v15

`SignalPointId` is local to one operation stream and is assigned densely and deterministically by the Compiler.

- `SignalQueue(signalPoint)` creates a completion identity.
- `WaitQueue(signalPoint)` waits for the exact current-frame producer signal.
- `WaitTemporal(resource, producerSignalPoint)` waits for that producer point from the previous submitted frame.
- `ReleaseExternal(slot, releaseSignalPoint)` returns the exact last-use completion token.

The Executor only maps the referenced point to a native D3D12 queue fence/value. It does not choose a producer queue or infer “the latest signal”.

## External boundary

Incoming and outgoing states are derived from canonical first and last Work use, not ResourceUse registration order. Release completion references the signal after the canonical last consumer Work.

## Temporal boundary

SemanticAnalysis already requires exactly one current writer for each Temporal resource. Stage F records that writer's signal point in every previous-generation wait, allowing cross-queue previous-frame producer/consumer relations without Backend inference.

## Compatibility

Target schema/runtime version is 15. Older v14 Package bytes are rejected rather than reinterpreted.
