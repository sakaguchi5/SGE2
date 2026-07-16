@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe was not found.
  exit /b 1
)
set "MSBUILD="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
  echo MSBuild was not found.
  exit /b 1
)

set "PROJECT=%ROOT%47_Level3AuthorityTests\47_Level3AuthorityTests.vcxproj"
if not exist "%PROJECT%" (
  echo Authority test project was not found: %PROJECT%
  exit /b 1
)

set "FAILED=0"
for %%C in (Debug Release) do (
  echo ============================================================
  echo Building decisive Level 3 authority tests: %%C x64
  echo ============================================================
  "%MSBUILD%" "%PROJECT%" /m /t:Build /p:Configuration=%%C /p:Platform=x64
  if errorlevel 1 (
    echo MSBuild failed for %%C x64.
    set "FAILED=1"
  ) else (
    set "EXE=%ROOT%build\bin\x64\%%C\47_Level3AuthorityTests.exe"
    if not exist "!EXE!" (
      echo MSBuild returned success, but the expected executable was not produced:
      echo   !EXE!
      echo Files currently present under the configuration output directory:
      if exist "%ROOT%build\bin\x64\%%C" dir /b "%ROOT%build\bin\x64\%%C"
      set "FAILED=1"
    ) else (
      echo Running 47_Level3AuthorityTests: %%C x64
      "!EXE!"
      if errorlevel 1 set "FAILED=1"
    )
  )
)

if "!FAILED!"=="0" (
  echo ============================================================
  echo SGE2 LEVEL 3 DECISIVE TEST GATE PASSED
  echo It is now valid to integrate this gate into run_level3_final.bat.
  echo ============================================================
  exit /b 0
)

echo ============================================================
echo SGE2 LEVEL 3 DECISIVE TEST GATE FAILED
echo The executable was built and the authority failures above are now meaningful.
echo On commit c42b0a5, Plan-authority failures are expected until repaired.
echo Do not declare the formal Level 3 Final Freeze yet.
echo ============================================================
exit /b 1
