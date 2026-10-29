# move_re2_window.ps1 - move RE2's borderless window between monitors.
#
# WHY THIS EXISTS: RE2 runs borderless fullscreen (WS_POPUP, no caption), and
# Win+Shift+Arrow does not act on that kind of window. SetWindowPos does.
#
# USAGE
#   move_re2_window.bat              -> next monitor
#   powershell -ExecutionPolicy Bypass -File move_re2_window.ps1 -Primary
#   powershell -ExecutionPolicy Bypass -File move_re2_window.ps1 -Secondary
#   powershell -ExecutionPolicy Bypass -File move_re2_window.ps1 -Index 1

param(
    [switch]$Next,
    [switch]$Primary,
    [switch]$Secondary,
    [int]$Index = -1
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class MW {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int L, T, R, B; }
}
"@

$proc = Get-Process re2 -ErrorAction SilentlyContinue
if (-not $proc) { Write-Host "RE2 is not running." -ForegroundColor Red; exit 1 }
$h = $proc.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Host "No main window yet (still loading?)." -ForegroundColor Yellow; exit 1 }

Add-Type -AssemblyName System.Windows.Forms
$mons = @([System.Windows.Forms.Screen]::AllScreens | Sort-Object { $_.Bounds.X } | ForEach-Object {
    [pscustomobject]@{
        X = $_.Bounds.X; Y = $_.Bounds.Y; W = $_.Bounds.Width; H = $_.Bounds.Height
        Primary = $_.Primary; Name = $_.DeviceName
    }
})

$cur = New-Object MW+RECT
[MW]::GetWindowRect($h, [ref]$cur) | Out-Null
$centerX = [int](($cur.L + $cur.R) / 2)
$curIdx = 0
for ($i = 0; $i -lt $mons.Count; $i++) {
    if ($centerX -ge $mons[$i].X -and $centerX -lt ($mons[$i].X + $mons[$i].W)) { $curIdx = $i; break }
}

if ($Primary)        { $t = $mons | Where-Object { $_.Primary }        | Select-Object -First 1 }
elseif ($Secondary)  { $t = $mons | Where-Object { -not $_.Primary }   | Select-Object -First 1 }
elseif ($Index -ge 0){ $t = $mons[[Math]::Min($Index, $mons.Count - 1)] }
else                 { $t = $mons[($curIdx + 1) % $mons.Count] }

if (-not $t) { Write-Host "No matching monitor." -ForegroundColor Red; exit 1 }

# SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW
$ok = [MW]::SetWindowPos($h, [IntPtr]::Zero, $t.X, $t.Y, $t.W, $t.H, (0x4 -bor 0x10 -bor 0x40))
Start-Sleep -Milliseconds 400

$after = New-Object MW+RECT
[MW]::GetWindowRect($h, [ref]$after) | Out-Null

Write-Host ("Monitors: " + (($mons | ForEach-Object {
    "{0} @({1},{2}) {3}x{4}{5}" -f $_.Name, $_.X, $_.Y, $_.W, $_.H, $(if ($_.Primary) { " [primary]" } else { "" })
}) -join "  |  "))
Write-Host ("Window now: ({0},{1}) - ({2},{3})   ok={4}" -f $after.L, $after.T, $after.R, $after.B, $ok) -ForegroundColor Green
