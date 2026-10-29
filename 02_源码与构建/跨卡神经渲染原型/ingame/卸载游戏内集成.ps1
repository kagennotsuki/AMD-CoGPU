# 卸载跨卡集成：移除游戏目录里的 dxgi 代理与 dxgi_orig
param(
    [string]$GameDir = 'E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2'
)

$ErrorActionPreference = 'Stop'
# 卸载跨卡集成：移除游戏目录里的 dxgi 代理与 dxgi_orig

Write-Host '=== 跨卡神经渲染 · 游戏内集成卸载 ==='
Write-Host ("游戏目录: " + $GameDir)

$target = Join-Path $GameDir 'dxgi.dll'
$origDxgi = Join-Path $GameDir 'dxgi_orig.dll'

if (Test-Path -LiteralPath $target) {
    $size = (Get-Item $target).Length
    Remove-Item -LiteralPath $target -Force
    Write-Host ("已删除 dxgi.dll (" + [math]::Round($size/1KB) + " KB)")
} else {
    Write-Host 'dxgi.dll 不存在，无需删除'
}

if (Test-Path -LiteralPath $origDxgi) {
    Remove-Item -LiteralPath $origDxgi -Force
    Write-Host '已删除 dxgi_orig.dll'
}

foreach ($n in @('d3d12.dll', 'd3d12_orig.dll', 'D3D12Core.dll')) {
    $p = Join-Path $GameDir $n
    if (Test-Path -LiteralPath $p) {
        Remove-Item -LiteralPath $p -Force
        Write-Host ("已删除 " + $n)
    }
}

$log = Join-Path $GameDir 'crossgpu.log'
if (Test-Path -LiteralPath $log) {
    Write-Host ('保留日志: ' + $log + '（如需删除请手动）')
}

Write-Host ''
Write-Host '卸载完成。系统 dxgi.dll 未受影响。'
