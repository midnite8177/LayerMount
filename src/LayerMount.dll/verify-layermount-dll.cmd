@echo off
rem Post-build symbol-hygiene guard for LayerMount.dll. Thin wrapper that
rem delegates to verify-layermount-dll.ps1; see that script for the import
rem allowlist and export-mangling checks.
rem
rem Invoked with two arguments:
rem   %1 = full path to built LayerMount.dll
rem   %2 = $(IntDir) for scratch output (imports.txt, exports.txt)
rem
rem MSBuild passes $(IntDir) with a trailing backslash. Embedding that
rem inside a double-quoted argument to powershell makes the closing `"`
rem look like an escaped quote and breaks path parsing -- strip the
rem trailing backslash before forwarding.

setlocal
set "OUT=%~2"
if "%OUT:~-1%"=="\" set "OUT=%OUT:~0,-1%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify-layermount-dll.ps1" -Dll "%~1" -OutDir "%OUT%"
exit /b %ERRORLEVEL%
