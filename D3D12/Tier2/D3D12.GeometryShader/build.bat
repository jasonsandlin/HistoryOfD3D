@echo off
setlocal
for %%I in ("%~dp0.") do set "NAME=%%~nxI"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\build-all.ps1" -Only "%NAME%"
