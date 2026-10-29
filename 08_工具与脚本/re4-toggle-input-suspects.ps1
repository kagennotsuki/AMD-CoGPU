# re4-toggle-input-suspects.ps1 —— RE4 输入故障定位：逐个停用可疑组件
#
# 症状：游戏里键盘不能操作、鼠标正常；Alt+Tab 切出切回无效（排除焦点问题）。
# 思路：一次只停一个，看键盘是否恢复。
#
# 用法（游戏必须已退出）：
#   powershell -File re4-toggle-input-suspects.ps1 -Status
#   powershell -File re4-toggle-input-suspects.ps1 -Off REFramework
#   powershell -File re4-toggle-input-suspects.ps1 -Off ReShade
#   powershell -File re4-toggle-input-suspects.ps1 -Off MGPU
#   powershell -File re4-toggle-input-suspects.ps1 -On  REFramework      # 恢复
#
param(
    [ValidateSet('Status','Off','On')][string]$Action = 'Status',
    [ValidateSet('ReShade','REFramework','MGPU','AMDNR','All')][string]$Target = 'All',
    # 简写形式： -Off REFramework / -On REFramework（等价于 -Action Off -Target REFramework）
    [string]$Off = '',
    [string]$On  = '',
    [string]$GameDir = 'E:\SteamLibrary\steamapps\common\RESIDENT EVIL 4  BIOHAZARD RE4'
)
$ErrorActionPreference = 'Stop'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

# 简写参数到 Action/Target 的映射（放在这里，早于任何判断）
if ($Off) { $Action = 'Off'; $Target = $Off }
if ($On)  { $Action = 'On';  $Target = $On  }
if (-not $Off -and -not $On -and $Action -ne 'Status' -and $Target -eq 'All') {
    Write-Host '提示：-Action Off 需要配合 -Target <组件>，或用简写 -Off <组件>。' -ForegroundColor Yellow
}

# 组件 → 主文件名
$files = [ordered]@{
    'ReShade'     = 'dxgi.dll'
    'REFramework' = 'dinput8.dll'
    'MGPU'        = 'nvngx.dll_mgpu_bridge.addon64'
    'AMDNR'       = 'amd-nr.addon64'
}

function Show-Status {
    Write-Host '=== 当前状态 ===' -ForegroundColor Cyan
    foreach ($k in $files.Keys) {
        $p  = Join-Path $GameDir $files[$k]
        $off = Get-ChildItem -LiteralPath $GameDir -File -Filter ($files[$k] + '.off-*') -ErrorAction SilentlyContinue
        $state = if (Test-Path -LiteralPath $p) { '启用' } elseif ($off) { '已停用（' + $off[-1].Name + '）' } else { '不存在' }
        Write-Host ("  {0,-12} {1,-44} {2}" -f $k, $files[$k], $state)
    }
    $run = Get-Process -Name re4 -ErrorAction SilentlyContinue
    Write-Host ('  re4 进程: ' + $(if ($run) { "运行中 (PID $($run.Id)) —— 改名会失败，请先退出游戏" } else { '未运行（可以改名）' }))
}

function Get-Targets {
    if ($Target -eq 'All') { return @($files.Keys) } else { return @($Target) }
}

if ($Action -eq 'Status') { Show-Status; exit 0 }

if (Get-Process -Name re4 -ErrorAction SilentlyContinue) {
    Write-Host '❌ 游戏正在运行，请先完全退出 RE4 再做这个操作。' -ForegroundColor Red
    exit 1
}

foreach ($t in (Get-Targets)) {
    if (-not $files.Contains($t)) { Write-Host "未知组件: $t"; continue }
    $name = $files[$t]
    $live = Join-Path $GameDir $name
    if ($Action -eq 'Off') {
        if (Test-Path -LiteralPath $live) {
            $dst = Join-Path $GameDir ($name + '.off-inputtest-' + $stamp)
            if (-not $dst.StartsWith($GameDir, [System.StringComparison]::OrdinalIgnoreCase)) { throw "拒绝改名到游戏目录外: $dst" }
            Move-Item -LiteralPath $live -Destination $dst
            Write-Host ("  [已停用] {0,-12} {1}  ->  {2}" -f $t, $name, (Split-Path $dst -Leaf)) -ForegroundColor Green
        } else {
            Write-Host ("  [跳过]   {0,-12} {1} 本来就不在位" -f $t, $name)
        }
    } else {
        # 恢复：用 -like 过滤 + @() 强制数组，避开 -Filter 与 Select-Object 在不同宿主下的差异
        $cands = @(Get-ChildItem -LiteralPath $GameDir -File -ErrorAction SilentlyContinue |
                   Where-Object { $_.Name -like ($name + '.off-*') } |
                   Sort-Object Name)
        if ($cands.Count -ge 1) {
            $cand = $cands[$cands.Count - 1]
            $full = [string]$cand.FullName
            if ([string]::IsNullOrEmpty($full)) { Write-Host ("  [错误] 取不到完整路径: " + $cand.Name) -ForegroundColor Red; continue }
            if (-not $full.StartsWith($GameDir, [System.StringComparison]::OrdinalIgnoreCase)) { throw "拒绝移动游戏目录外的文件: $full" }
            if (Test-Path -LiteralPath $live) {
                Write-Host ("  [跳过]   {0,-12} {1} 已经在位" -f $t, $name)
            } else {
                Move-Item -LiteralPath $full -Destination $live
                Write-Host ("  [已恢复] {0,-12} {1}   <-  {2}" -f $t, $name, $cand.Name) -ForegroundColor Green
            }
        } else {
            Write-Host ("  [跳过]   {0,-12} 没有可恢复的 .off-* 文件" -f $t)
        }
    }
}

Write-Host ''
Show-Status
Write-Host ''
Write-Host '提醒：一次只停一个组件，然后启动游戏、进实际游玩、按几下键盘（方向键/WASD）判断。' -ForegroundColor Yellow
Write-Host '      键盘恢复正常 → 元凶就是刚停掉的那个；测完记得用 -On 恢复。' -ForegroundColor Yellow
