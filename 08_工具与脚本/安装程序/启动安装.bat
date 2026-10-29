@echo off
chcp 65001 >nul
setlocal
set "HERE=%~dp0"
where pwsh >nul 2>nul
if errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%AMD-CoGPU-Setup.ps1"
) else (
  pwsh -NoProfile -ExecutionPolicy Bypass -File "%HERE%AMD-CoGPU-Setup.ps1"
)
if errorlevel 1 pause
endlocal
