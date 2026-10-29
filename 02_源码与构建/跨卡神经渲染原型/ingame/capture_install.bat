@echo off
rem ===== Install the capture-only proxy into the game folder =====
rem Pure ASCII on purpose: Chinese in a .bat body breaks cmd.exe's codepage.
setlocal
set HERE=%~dp0
set SRC=%HERE%..\build\Release\libxess.dll
set GAME=E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2

if not exist "%SRC%" (
  echo [ERROR] proxy not found: %SRC%
  echo         build it first:  cmake --build build --config Release --target xess_proxy
  pause
  exit /b 1
)
if not exist "%GAME%" (
  echo [ERROR] game folder not found: %GAME%
  pause
  exit /b 1
)

rem keep the real DLL as libxess_orig.dll (the proxy loads it)
if not exist "%GAME%\libxess_orig.dll" (
  echo [1/3] backing up the real libxess.dll -^> libxess_orig.dll
  copy /y "%GAME%\libxess.dll" "%GAME%\libxess_orig.dll" >nul
) else (
  echo [1/3] libxess_orig.dll already present, keep it
)

echo [2/3] installing the capture proxy as libxess.dll
copy /y "%SRC%" "%GAME%\libxess.dll" >nul

echo [3/3] enabling capture mode
echo 1> "%GAME%\xess_proxy_capture.txt"
del /q "%GAME%\xess_proxy_level.txt" 2>nul

echo.
echo Done. Launch the game, then in-game press F2 to capture one frame.
echo   (or drop a file named xess_proxy_capnow.txt into the game folder)
echo Data goes to the game folder as nr_cap_*.bin / nr_cap_*.bmp / nr_cap_params.txt
echo Then run  capture_collect.bat  to copy it next to this script.
echo.
pause
