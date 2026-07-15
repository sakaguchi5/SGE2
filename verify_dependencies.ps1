$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectFiles = Get-ChildItem -Path $root -Recurse -Filter *.vcxproj | Sort-Object FullName
if ($projectFiles.Count -eq 0) { throw 'No Visual Studio C++ projects were found.' }

$graph = @{}
foreach ($project in $projectFiles) {
    $projectPath = [IO.Path]::GetFullPath($project.FullName)
    [xml]$xml = Get-Content -Raw -LiteralPath $projectPath
    $references = @()
    foreach ($node in $xml.SelectNodes("//*[local-name()='ProjectReference']")) {
        $referencePath = [IO.Path]::GetFullPath((Join-Path $project.DirectoryName $node.Include))
        if (-not (Test-Path -LiteralPath $referencePath)) {
            throw "Missing ProjectReference: $projectPath -> $referencePath"
        }
        $references += $referencePath
    }
    $graph[$projectPath] = $references
}

$visitState = @{}
$stack = New-Object System.Collections.Generic.List[string]
function Visit-Project([string]$projectPath) {
    $state = if ($visitState.ContainsKey($projectPath)) { $visitState[$projectPath] } else { 0 }
    if ($state -eq 1) {
        $cycle = (@($stack) + @($projectPath) | ForEach-Object { Split-Path (Split-Path $_ -Parent) -Leaf }) -join ' -> '
        throw "ProjectReference cycle detected: $cycle"
    }
    if ($state -eq 2) { return }
    $visitState[$projectPath] = 1
    [void]$stack.Add($projectPath)
    foreach ($dependency in $graph[$projectPath]) { Visit-Project $dependency }
    $stack.RemoveAt($stack.Count - 1)
    $visitState[$projectPath] = 2
}
foreach ($projectPath in $graph.Keys) { Visit-Project $projectPath }

$forbidden = @(
    '02_SemanticModel',
    '03_SemanticBuilder',
    '04_SemanticAnalysis',
    '05_TargetModel',
    '06_D3D12TargetCompiler',
    '20_ClassicalFrontend',
    '21_SdfFrontend',
    '22_PgaFrontend',
    '23_ExperimentDomain'
)
$targets = @('10_D3D12Executor', '35_D3D12ReadbackTests', '40_Launcher')

foreach ($targetName in $targets) {
    $targetProject = $projectFiles | Where-Object { $_.Directory.Name -eq $targetName } | Select-Object -First 1
    if (-not $targetProject) { throw "Target project was not found: $targetName" }

    $seen = @{}
    $pending = New-Object System.Collections.Generic.Stack[string]
    $pending.Push([IO.Path]::GetFullPath($targetProject.FullName))
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($dependency in $graph[$current]) {
            if (-not $seen.ContainsKey($dependency)) {
                $seen[$dependency] = $true
                $pending.Push($dependency)
            }
        }
    }

    foreach ($dependency in $seen.Keys) {
        $dependencyName = Split-Path (Split-Path $dependency -Parent) -Leaf
        if ($forbidden -contains $dependencyName) {
            throw "$targetName transitively references forbidden project $dependencyName"
        }
    }
}


function Get-TransitiveDependencies([string]$projectPath) {
    $seen = @{}
    $pending = New-Object System.Collections.Generic.Stack[string]
    $pending.Push([IO.Path]::GetFullPath($projectPath))
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($dependency in $graph[$current]) {
            if (-not $seen.ContainsKey($dependency)) {
                $seen[$dependency] = $true
                $pending.Push($dependency)
            }
        }
    }
    return $seen
}

$frontendIsolation = @{
    '20_ClassicalFrontend' = @('21_SdfFrontend', '22_PgaFrontend')
    '21_SdfFrontend'       = @('20_ClassicalFrontend', '22_PgaFrontend')
    '22_PgaFrontend'       = @('20_ClassicalFrontend', '21_SdfFrontend')
    '23_ExperimentDomain'  = @('20_ClassicalFrontend', '21_SdfFrontend', '22_PgaFrontend')
}
foreach ($frontendName in $frontendIsolation.Keys) {
    $frontendProject = $projectFiles | Where-Object { $_.Directory.Name -eq $frontendName } | Select-Object -First 1
    if (-not $frontendProject) { throw "Frontend project was not found: $frontendName" }
    $dependencies = Get-TransitiveDependencies $frontendProject.FullName
    foreach ($dependency in $dependencies.Keys) {
        $dependencyName = Split-Path (Split-Path $dependency -Parent) -Leaf
        if ($frontendIsolation[$frontendName] -contains $dependencyName) {
            throw "$frontendName transitively references forbidden sibling frontend $dependencyName"
        }
    }
}

Write-Host "Dependency boundary check passed. Projects: $($projectFiles.Count), references: $((($graph.Values | ForEach-Object { $_.Count }) | Measure-Object -Sum).Sum)."
