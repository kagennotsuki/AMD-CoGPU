# 安装跨卡集成：把 dxgi 代理装进 RE2 目录
param(
    [string]$GameDir = 'E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2'
)

$ErrorActionPreference = 'Stop'
# 安装跨卡集成：把 dxgi 代理装进 RE2 目录
# 用法：.\安装游戏内集成.ps1  [-GameDir <路径>]


$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$root = Split-Path -Parent $PSScriptRoot
$proxy = Join-Path $root 'build\Release\dxgi.dll'

Write-Host '=== 跨卡神经渲染 · 游戏内集成安装 ==='
Write-Host ("游戏目录: " + $GameDir)

if (-not (Test-Path -LiteralPath (Join-Path $GameDir 're2.exe'))) {
    throw "游戏目录里没有 re2.exe，请用 -GameDir 指定正确路径"
}
if (-not (Test-Path -LiteralPath $proxy)) {
    throw "没有找到编译产物: $proxy（先编译 dxgi_proxy 目标）"
}

# 1) 备份系统 dxgi 到游戏目录（代理运行时要用）
$sysDxgi = 'C:\Windows\System32\dxgi.dll'
$origDxgi = Join-Path $GameDir 'dxgi_orig.dll'
if (-not (Test-Path -LiteralPath $origDxgi)) {
    Copy-Item -LiteralPath $sysDxgi -Destination $origDxgi
    Write-Host ("已放置真实 dxgi -> dxgi_orig.dll  (" + [math]::Round((Get-Item $origDxgi).Length/1KB) + " KB)")
} else {
    Write-Host "dxgi_orig.dll 已存在，保留"
}

# 2) 已有的 dxgi.dll 改名备份（可能是别家注入器）
$target = Join-Path $GameDir 'dxgi.dll'
if (Test-Path -LiteralPath $target) {
    $bak = Join-Path $GameDir ("dxgi.dll.安装前备份-" + $stamp)
    Move-Item -LiteralPath $target -Destination $bak
    Write-Host ("原有 dxgi.dll 已备份 -> " + (Split-Path -Leaf $bak))
}

# 2.5) d3d12 代理（抓设备与命令队列）
$proxyD3D = Join-Path $root 'build\Release\d3d12.dll'
if (-not (Test-Path -LiteralPath $proxyD3D)) { throw "没有 d3d12 代理产物: $proxyD3D" }
$origD3D = Join-Path $GameDir 'd3d12_orig.dll'
if (-not (Test-Path -LiteralPath $origD3D)) {
    Copy-Item -LiteralPath 'C:\Windows\System32\d3d12.dll' -Destination $origD3D
    Write-Host ("已放置真实 d3d12 -> d3d12_orig.dll (" + [math]::Round((Get-Item $origD3D).Length/1KB) + " KB)")
} else { Write-Host "d3d12_orig.dll 已存在，保留" }
# 真实的 d3d12 会按自身目录找 D3D12Core.dll（否则报 D3D12_ERROR_INVALID_REDIST 0x887E0003）
$coreSrc = 'C:\Windows\System32\D3D12Core.dll'
$coreDst = Join-Path $GameDir 'D3D12Core.dll'
if ((Test-Path -LiteralPath $coreSrc) -and -not (Test-Path -LiteralPath $coreDst)) {
    Copy-Item -LiteralPath $coreSrc -Destination $coreDst
    Write-Host ("已放置 D3D12Core.dll (" + [math]::Round((Get-Item $coreDst).Length/1KB) + " KB)")
}

$targetD3D = Join-Path $GameDir 'd3d12.dll'
if (Test-Path -LiteralPath $targetD3D) {
    Move-Item -LiteralPath $targetD3D -Destination (Join-Path $GameDir ("d3d12.dll.安装前备份-" + $stamp))
}
Copy-Item -LiteralPath $proxyD3D -Destination $targetD3D
Write-Host ("已放入代理 d3d12.dll (" + [math]::Round((Get-Item $targetD3D).Length/1KB) + " KB)")
# 3) 放入代理
Copy-Item -LiteralPath $proxy -Destination $target
Write-Host ("已放入代理 dxgi.dll  (" + [math]::Round((Get-Item $target).Length/1KB) + " KB)")

# 4.5) 写入 ini 配置（游戏会自重启，环境变量传不下去）
$ini = Join-Path $GameDir 'crossgpu.ini'
if (-not (Test-Path -LiteralPath $ini)) {
    $lines = @(
        '; 跨卡集成配置',
        'Log=1',
        'Invert=0',
        'Off=0'
    )
    [System.IO.File]::WriteAllLines($ini, $lines, (New-Object System.Text.ASCIIEncoding))
    Write-Host '已写入 crossgpu.ini (Log=1 Invert=0 Off=0)'
} else {
    Write-Host 'crossgpu.ini 已存在，保留'
}
# 4) 校验
$h1 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
$h2 = (Get-FileHash -LiteralPath $proxy -Algorithm SHA256).Hash
Write-Host ('哈希一致: ' + ($h1 -eq $h2))
Write-Host ''
Write-Host '安装完成。用「启动RE2测试.bat」启动游戏（会写日志到游戏目录 crossgpu.log）。'
Write-Host '卸载请运行「卸载游戏内集成.bat」。'
