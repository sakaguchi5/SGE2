@echo off
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
call "%~dp0build.bat" %CONFIG%
if errorlevel 1 exit /b 1

set "BIN=%~dp0build\bin\x64\%CONFIG%"
set "TESTDIR=%~dp0build\tests"
if not exist "%TESTDIR%" mkdir "%TESTDIR%"
set "PACKAGE=%TESTDIR%\CommonExperiment_Slice15.sgep"

echo Generating Slice-15 Classical/SDF/PGA common experiment package...
"%BIN%\41_PackageCompiler.exe" "%PACKAGE%" all
if errorlevel 1 exit /b 1

for %%T in (30_SemanticTests 31_CompilerTests 32_PackageContractTests 33_D3D12ConformanceTests 34_ExperimentTests 36_Level1ScenarioTests) do (
  echo Running %%T...
  "%BIN%\%%T.exe"
  if errorlevel 1 goto :failed
)

echo Running 35_D3D12ReadbackTests with WARP...
"%BIN%\35_D3D12ReadbackTests.exe" "%PACKAGE%"
if errorlevel 1 exit /b 1

echo All Slice-15 and Level-1 Stage-A tests passed.
exit /b 0

:failed
exit /b 1
