@echo off
rem ===== Copy the captured frame data out of the game folder =====
rem Pure ASCII on purpose.
setlocal
set HERE=%~dp0
set GAME=E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2
set DST=%HERE%..\capture

if not exist "%DST%" mkdir "%DST%"

set FOUND=0
for %%F in (color.bin depth.bin mv.bin out.bin color.bmp out.bmp params.txt) do (
  if exist "%GAME%\nr_cap_%%F" (
    copy /y "%GAME%\nr_cap_%%F" "%DST%\" >nul
    echo   copied nr_cap_%%F
    set FOUND=1
  )
)

if "%FOUND%"=="0" (
  echo [WARN] no nr_cap_* files in the game folder.
  echo        Launch the game with capture mode installed, then press F2.
) else (
  echo.
  echo Done. Files are in: %DST%
  echo Now tell the agent, or run:
  echo   build\Release\nr_from_capture.exe
)
echo.
pause
