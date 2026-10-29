@echo off
rem ===== Ask the in-game proxy to capture the next frame (no hotkey needed) =====
rem Run this while the game is running. The proxy notices the file within a few
rem frames, deletes it, and dumps the frame. Pure ASCII on purpose.
setlocal
set GAME=E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2
if not exist "%GAME%\xess_proxy_capture.txt" (
  echo [WARN] capture mode is not installed. Run capture_install.bat first.
  pause
  exit /b 1
)
echo now> "%GAME%\xess_proxy_capnow.txt"
echo [OK] trigger written. The next frame will be captured (a few seconds later).
echo      Then run capture_collect.bat
timeout /t 3 >nul
