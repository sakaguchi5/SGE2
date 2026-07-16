@echo off
setlocal
set "ROOT=%~dp0"
set "FINALDIR=%ROOT%build\level3-final"
if not exist "%FINALDIR%" mkdir "%FINALDIR%"

echo ============================================================
echo Re-qualifying the immutable Level 2 baseline...
echo ============================================================
call "%ROOT%run_level2_final.bat"
if errorlevel 1 goto :failed

echo Verifying Level 3 architectural dependency boundaries...
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%verify_dependencies.ps1"
if errorlevel 1 goto :failed

set "DEBUGBIN=%ROOT%build\bin\x64\Debug"
set "RELEASEBIN=%ROOT%build\bin\x64\Release"

echo ============================================================
echo Level 3 planning qualification: Debug x64
echo ============================================================
"%DEBUGBIN%\45_Level3PlanningTests.exe"
if errorlevel 1 goto :failed
"%DEBUGBIN%\46_Level3RuntimeTests.exe"
if errorlevel 1 goto :failed

echo ============================================================
echo Level 3 planning qualification: Release x64
echo ============================================================
"%RELEASEBIN%\45_Level3PlanningTests.exe"
if errorlevel 1 goto :failed
"%RELEASEBIN%\46_Level3RuntimeTests.exe"
if errorlevel 1 goto :failed

echo Emitting Debug manifest in two fresh processes...
"%DEBUGBIN%\45_Level3PlanningTests.exe" --emit-manifest "%FINALDIR%\Level3_Debug_A.freeze-manifest.txt"
if errorlevel 1 goto :failed
"%DEBUGBIN%\45_Level3PlanningTests.exe" --emit-manifest "%FINALDIR%\Level3_Debug_B.freeze-manifest.txt"
if errorlevel 1 goto :failed
fc /b "%FINALDIR%\Level3_Debug_A.freeze-manifest.txt" "%FINALDIR%\Level3_Debug_B.freeze-manifest.txt" >nul
if errorlevel 1 (
  echo Fresh-process Debug Candidate manifests differ.
  goto :failed
)

echo Emitting Release manifest...
"%RELEASEBIN%\45_Level3PlanningTests.exe" --emit-manifest "%FINALDIR%\Level3_Release.freeze-manifest.txt"
if errorlevel 1 goto :failed
fc /b "%FINALDIR%\Level3_Debug_A.freeze-manifest.txt" "%FINALDIR%\Level3_Release.freeze-manifest.txt" >nul
if errorlevel 1 (
  echo Debug and Release Level 3 manifests differ.
  goto :failed
)

echo ============================================================
echo SGE2 LEVEL 3 FINAL FREEZE PASSED
echo Level 2 regression corpus: 54 byte-identical CanonicalSafe Packages
echo Target schema/runtime: D3D12 v17 / Runtime v17
echo Candidate manifests: same-process, fresh-process, Debug, and Release identical
echo Verified schedule and Queue alternatives: WARP observations identical
echo Semantic GPU Engine 2 Level 3 is complete.
echo ============================================================
exit /b 0

:failed
echo Level 3 final qualification failed.
exit /b 1
