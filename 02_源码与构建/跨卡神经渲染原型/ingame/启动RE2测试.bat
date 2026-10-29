@echo off
setlocal
set CROSSGPU_LOG=1
set CROSSGPU_INVERT=1
set GAME=E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2
echo Launching RE2 with cross-GPU proxy (LOG=1 INVERT=1)...
pushd "%GAME%"
start "" "re2.exe"
popd
echo Started. Log: %GAME%\crossgpu.log
timeout /t 3 >nul