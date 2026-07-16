# Dependency rules

`ProjectReference` is treated as an executable design boundary.

## Executor boundary

Forbidden transitively from `10_D3D12Executor`, `35_D3D12ReadbackTests`, and `40_Launcher`:

- 02_SemanticModel
- 03_SemanticBuilder
- 04_SemanticAnalysis
- 05_TargetModel
- 06_D3D12TargetCompiler
- 20_ClassicalFrontend
- 21_SdfFrontend
- 22_PgaFrontend
- 23_ExperimentDomain

Allowed to `10_D3D12Executor`:

- 00_Base
- 07_FrozenPackageCore
- 08_D3D12PackageSchema
- 09_PackageRuntime
- Windows SDK D3D12 / DXGI utilities

## Frontend isolation

Forbidden transitively:

- Classical → SDF or PGA
- SDF → Classical or PGA
- PGA → Classical or SDF
- ExperimentDomain → any frontend

Classical, SDF, and PGA may depend on the neutral ExperimentDomain. No frontend may use a sibling frontend as its implementation.

`verify_dependencies.ps1` checks all of these rules mechanically.
