# Slice 15 telemetry UI

The Launcher shows the cumulative GPU execution state plus the new frontend-equivalence boundary.

## Frontend equivalence panel

The panel records the proof embodied by the loaded Package:

- Classical input: explicit vertices
- SDF input: three half-spaces per triangle
- PGA input: homogeneous point/line join and meet
- convergence: bit-identical common geometry and Package bytes
- Backend origin: no frontend identity remains in execution data

This is static Package provenance established by `41_PackageCompiler all`; the Launcher itself references neither frontend nor the Experiment project.

## Retained runtime panels

The remaining panels continue to display:

- device epoch and recovery state
- controlled reconstruction count
- DRED summary
- dynamic FrameInvocation values
- Temporal ping-pong state
- Copy-to-Direct queue handoff
- External acquire/release completion
- Alias activation
- Compute output
- depth behavior
- Frozen Package digest and section count

The telemetry panel is presentation-only and does not alter Package execution.
