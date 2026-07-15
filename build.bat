@echo off
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
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
"%MSBUILD%" "%~dp0SemanticGpuEngine2.sln" /m /t:Build /p:Configuration=%CONFIG% /p:Platform=x64
exit /b %errorlevel%
