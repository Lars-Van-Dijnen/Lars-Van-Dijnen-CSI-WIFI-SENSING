@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto compile

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto missing
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "CSI_VS_INSTALL=%%i"
if not defined CSI_VS_INSTALL goto missing
call "%CSI_VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto missing

:compile
cl /nologo /std:c11 /W4 /O2 /TC capture_windows.c csi_parse.c /Fe:capture.exe /link mfplat.lib mfreadwrite.lib mfuuid.lib mf.lib ole32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /W4 /O2 /TC csi_parse.c test_csi_parse.c /Fe:test_csi_parse.exe
if errorlevel 1 exit /b 1
test_csi_parse.exe
if errorlevel 1 exit /b 1
echo Built capture.exe and passed parser tests.
exit /b 0

:missing
echo A prebuilt capture.exe is included in the download.
echo To rebuild, install Visual Studio Build Tools with Desktop development with C++,
echo including the Windows SDK. The recorder source is compiled as C, not C++.
echo Alternatively, use the MinGW or Zig command in README.md.
exit /b 1
