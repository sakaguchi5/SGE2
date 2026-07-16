# Level 2 Stage G: Package-driven Runtime / Executor

## Purpose

Stage G removes the final presentation- and queue-role assumptions from the
D3D12 Runtime boundary. The Executor does not infer whether a Package is a
rendering Package or a compute Package. It materializes only the queues and
surface objects declared by the validated Frozen Package.

## Runtime contract

`LoadPackage` now has two valid forms:

```text
LoadPackage(package, executor, surface)
  A Package with a Surface slot requires a non-null Surface host.

LoadPackage(package, executor)
  A Package without Surface slots is loaded headlessly.
```

The presence of a Surface is derived from the Frozen Package invocation schema.
Passing a Surface host does not add a Surface to a headless Package, and omitting
a Surface host does not cause the Executor to invent an off-screen target for a
Surface Package.

## Queue runtime state

Native queue objects are stored in a dense table indexed by Package `QueueId`.
Each entry owns:

```text
queue class
ID3D12CommandQueue
fence and next fence value
frame-slot fence values
command allocator / command-list pools
frame batch cursor
frame submission state
```

Level 2 v1 still deliberately supports this target capability:

```text
Direct: exactly 1
Compute: 0 or 1
Copy: 0 or 1
```

That is a Target-feasibility limit, not an Executor field-layout assumption.
Operations address the table by the Queue ID fixed in the Package.

## Optional presentation

A Package with no `SurfaceSlot` has `surfaceImageCount == 0`. The Executor:

```text
does not require ISurfaceHost
does not create a swap chain
does not materialize back buffers
does not require PresentSurface
does not create a load command list when the load stream has no queue batch
```

A Package with a Surface slot keeps the existing explicit operation contract:

```text
AcquireSurfaceImage
Transition
ExecuteRaster
Transition
PresentSurface
SignalQueue
```

## Package view ownership

Raster execution uses the `RenderTarget` view and descriptor index fixed in the
Package. The current swap-chain image only selects the physical instance of the
Surface resource; it no longer selects an implicit RTV outside the Package view
plan.

## Qualification

`37_Level1ExecutionTests` adds two Stage-G checks before the historical Slice
suite:

1. A headless Compute-only Package is compiled, loaded without an
   `ISurfaceHost`, submitted on its Compute queue, reconstructed from the Frozen
   Package, and submitted again.
2. A Package with a Surface slot is rejected when no `ISurfaceHost` is supplied.

The existing eighteen Slice 1-15 Packages still execute through the same
Runtime and WARP Executor, preserving the Level-1 regression baseline.

## Versioning

The D3D12 Target schema and minimum Runtime version are advanced from 15 to 16.
Stage G changes the executable meaning of `surfaceImageCount == 0` from an
invalid profile to the canonical representation of a headless Package.
