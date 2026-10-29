@echo off
rem Move RE2's borderless window to the next monitor.
rem Tip: create a shortcut to this .bat and give it a "Shortcut key" (e.g. Ctrl+Alt+M)
rem      in the shortcut's properties - then that key replaces Win+Shift+Left.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0move_re2_window.ps1" %*
if /i "%~1"=="" timeout /t 2 >nul
