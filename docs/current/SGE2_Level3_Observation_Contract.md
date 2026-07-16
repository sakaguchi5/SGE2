# SGE2 Level 3 Observation Contract

## 1. Purpose

Different legal plans are not required to have identical Package bytes or physical timing. They are equivalent when they preserve every observation declared below for the same Semantic obligation and invocation sequence.

## 2. Required observations

- Final bytes of every Published Buffer.
- Final bytes and guaranteed outgoing state of every External Resource.
- Temporal Current/Previous progression for the complete frame sequence.
- Present count, presented Surface identity, and final Present state.
- Deterministic Raster readback used by the qualification scenario.
- Equivalent failure boundary for missing Dynamic, External, or Surface invocation data.

Physical Queue overlap, GPU timestamps, driver-private layout, fence values, descriptor handles, GPU virtual addresses, and compositor timing are not Semantic observations.

## 3. Comparison procedure

Each accepted candidate is lowered independently to a complete Schema 17 Package. Qualification executes Packages from a fresh Package instance, supplies byte-identical invocation data, reads back the declared observations, and compares those observations rather than Package bytes.

Device-reconstruction qualification discards all device-owned objects, rematerializes from the same selected Package bytes, resets Temporal history according to Runtime 17, rebinds External/Surface inputs, and repeats the observation comparison.

## 4. Determinism distinction

Candidate enumeration, PlanIdentity, manifest encoding, policy selection from fixed inputs, Package lowering, and offline reselection from fixed Profile bytes are byte-deterministic. Hardware measurements stored in a Profile record are observations of a physical environment and are not required to be byte-deterministic across measurement runs.

