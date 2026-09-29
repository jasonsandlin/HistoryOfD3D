@echo off
setlocal
rem Assign to a temp var first: %ProgramFiles(x86)% contains parens that break the for/f parsing.
set "PF86=%ProgramFiles(x86)%"
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH echo Could not locate Visual Studio via vswhere. & exit /b 1
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /EHsc /W3 /Zi /Fe:CubeD3D7.exe main.cpp user32.lib
exit /b %errorlevel%
