#requires -Version 5.1
<#
    AMD CoGPU · 卸载
    作用：把装进游戏目录的代理、运行时可配置项与生成的文档移除，并恢复游戏自带的原版 XeSS。

    用法：
      powershell -ExecutionPolicy Bypass -File 卸载.ps1 -GameDir "E:\...\RESIDENT EVIL 4  BIOHAZARD RE4"
      powershell -ExecutionPolicy Bypass -File 卸载.ps1 -GameDir "..." -KeepRuntime   # 保留运行时（915MB，下次装更快）
#>
param(
    [Parameter(Mandatory=$true)][string]$GameDir,
    [switch]$KeepRuntime,
    [switch]$WhatIf
)

if (-not (Test-Path $GameDir)) { throw "目录不存在：$GameDir" }
$g = (Resolve-Path $GameDir).Path
Write-Host "卸载目标：$g"
if ($WhatIf) { Write-Host '（演练模式：只列出将要做的事，不实际删除）' -ForegroundColor Yellow }

function Do-Delete([string]$path, [string]$what) {
    if (-not (Test-Path $path)) { return }
    if ($WhatIf) { Write-Host ("  将删除  {0}" -f $what); return }
    Remove-Item $path -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host ("  已删除  {0}" -f $what) -ForegroundColor DarkGray
}

# 1) 配置与生成的文档
foreach ($n in 'xess_proxy.bform.txt','xess_proxy.queuehook.txt','xess_proxy.gpu.txt','xess_proxy.dumpshot.txt',
               'xess_proxy.saved.ini','xess_proxy.model.txt','xess_proxy.taa.txt','xess_proxy.diag5.txt',
               'xess_proxy.mirror.txt','xess_proxy.resid4k.txt','xess_proxy.bwtest.txt','xess_proxy.add4k.hlsl') {
    Do-Delete (Join-Path $g $n) $n
}
Get-ChildItem $g -File -Filter 'AMD-CoGPU-*' -ErrorAction SilentlyContinue | ForEach-Object { Do-Delete $_.FullName $_.Name }

# 2) 运行时（可选保留）
if (-not $KeepRuntime) {
    Do-Delete (Join-Path $g 'LmxxfNrRuntime.dll') 'LmxxfNrRuntime.dll'
    Do-Delete (Join-Path $g 'LmxxfNrRuntime.pak') 'LmxxfNrRuntime.pak'
    Do-Delete (Join-Path $g 'MochizukiNrRuntime.dll') 'MochizukiNrRuntime.dll'
    Do-Delete (Join-Path $g 'dlssnr-amd') 'dlssnr-amd\（模型目录）'
    Do-Delete (Join-Path $g 'person-model') 'person-model\'
}

# 3) 恢复原版 XeSS
$orig = Join-Path $g 'libxess_orig.dll'
$live = Join-Path $g 'libxess.dll'
$proxySize = 2000000   # 我们的代理约 790KB；原版 Intel XeSS 有 70MB+
if ((Test-Path $orig) -and (Test-Path $live) -and ((Get-Item $live).Length -lt $proxySize)) {
    if ($WhatIf) {
        Write-Host '  将删除  我们的代理 libxess.dll，并把 libxess_orig.dll 改回 libxess.dll'
    } else {
        Remove-Item $live -Force
        Move-Item $orig $live -Force
        Write-Host '  已恢复  原版 XeSS（libxess_orig.dll → libxess.dll）' -ForegroundColor Green
    }
} elseif (-not (Test-Path $orig)) {
    # 没有 orig：尝试用安装时留的备份
    $bak = Get-ChildItem $g -File -Filter 'libxess.dll.bak-setup-*' -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($bak) {
        if ($WhatIf) { Write-Host ("  将恢复  {0} → libxess.dll" -f $bak.Name) }
        else {
            Remove-Item $live -Force -ErrorAction SilentlyContinue
            Copy-Item $bak.FullName $live -Force
            Write-Host ("  已恢复  {0} → libxess.dll" -f $bak.Name) -ForegroundColor Green
        }
    } else {
        Write-Host '  ⚠ 找不到原版 XeSS（既无 libxess_orig.dll，也无 *.bak-setup-*），请从游戏校验文件恢复 libxess.dll' -ForegroundColor Yellow
    }
}

# 4) 清理安装备份（代理的 bak）
Get-ChildItem $g -File -Filter 'libxess.dll.bak-setup-*' -ErrorAction SilentlyContinue | ForEach-Object { Do-Delete $_.FullName $_.Name }

Write-Host ''
if ($WhatIf) { Write-Host '演练结束（未改动任何文件）' -ForegroundColor Yellow }
else { Write-Host '卸载完成。建议用 Steam「验证游戏文件的完整性」再确认一次原版 XeSS 完好。' -ForegroundColor Green }
