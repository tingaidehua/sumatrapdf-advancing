@echo off
setlocal
set "ROOT=%~dp0"
pushd "%ROOT%"

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 goto :failed

call bun cmd\build.ts -release
if errorlevel 1 goto :failed

copy /y "out\rel64\SumatraPDF.exe" "SumatraPDF.exe" >nul
if errorlevel 1 goto :failed

echo.
echo Built: %ROOT%SumatraPDF.exe
popd
exit /b 0

:failed
echo Build failed.
popd
exit /b 1
