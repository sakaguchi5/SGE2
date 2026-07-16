@echo off
setlocal
set "ROOT=%~dp0"
set "FINALDIR=%ROOT%build\level2-final"
if not exist "%FINALDIR%" mkdir "%FINALDIR%"

echo Verifying source/compiler/package/runtime/executor dependency boundaries...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%verify_dependencies.ps1"
if errorlevel 1 goto :failed

echo ============================================================
echo Level 2 final qualification: Debug x64
echo ============================================================
call "%ROOT%run_tests.bat" Debug
if errorlevel 1 goto :failed

echo ============================================================
echo Level 2 final qualification: Release x64
echo ============================================================
call "%ROOT%run_tests.bat" Release
if errorlevel 1 goto :failed

set "DEBUGBIN=%ROOT%build\bin\x64\Debug"
set "RELEASEBIN=%ROOT%build\bin\x64\Release"
set "DEBUGMANIFEST=%FINALDIR%\Level2_Debug.freeze-manifest.txt"
set "RELEASEMANIFEST=%FINALDIR%\Level2_Release.freeze-manifest.txt"

echo Emitting fixed 54-Package Debug qualification manifest...
"%DEBUGBIN%\44_Level2FinalFreezeTests.exe" --emit-manifest "%DEBUGMANIFEST%"
if errorlevel 1 goto :failed

echo Emitting fixed 54-Package Release qualification manifest...
"%RELEASEBIN%\44_Level2FinalFreezeTests.exe" --emit-manifest "%RELEASEMANIFEST%"
if errorlevel 1 goto :failed

echo Comparing Debug and Release Frozen Package manifests...
fc /b "%DEBUGMANIFEST%" "%RELEASEMANIFEST%" >nul
if errorlevel 1 (
  echo Debug and Release qualification manifests differ.
  goto :failed
)

echo ============================================================
echo SGE2 LEVEL 2 FINAL FREEZE PASSED

echo Fixed corpus: 54 accepted Packages
echo Schema/runtime: D3D12 v17 / Runtime v17
echo Same-process, fresh-process, Debug, and Release manifests are byte-identical.
echo Semantic GPU Engine 2 Level 2 is complete.
echo ============================================================
exit /b 0

:failed
echo Level 2 final qualification failed.
exit /b 1
