@echo off
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
set "EXECUTOR_OPTION=%~2"
set "RECOVERY_OPTION=%~3"
call "%~dp0build.bat" %CONFIG%
if errorlevel 1 exit /b 1
set "BIN=%~dp0build\bin\x64\%CONFIG%"
set "DEMO=%~dp0build\demo"
if not exist "%DEMO%" mkdir "%DEMO%"
"%BIN%\41_PackageCompiler.exe" "%DEMO%\CommonExperiment.sgep" all
if errorlevel 1 exit /b 1
"%BIN%\40_Launcher.exe" "%DEMO%\CommonExperiment.sgep" %EXECUTOR_OPTION% %RECOVERY_OPTION%
exit /b %errorlevel%
