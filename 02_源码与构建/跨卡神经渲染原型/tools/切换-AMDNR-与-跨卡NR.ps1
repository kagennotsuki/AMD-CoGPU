<#
  切换工具：在「AMDNR（OptiScaler 路线）」与「跨卡 NR（我们的代理）」之间切换
  用法：
    .\切换-AMDNR-与-跨卡NR.ps1 -To AMDNR     # 恢复成 AMDNR
    .\切换-AMDNR-与-跨卡NR.ps1 -To CrossGpu  # 换成我们的跨卡代理
    .\切换-AMDNR-与-跨卡NR.ps1 -Status       # 只看当前状态
  安全：只改名/复制，从不删除文件；改名前会检查目标是否已存在。
#>
param(
    [ValidateSet('AMDNR','CrossGpu')] [string]$To,
    [switch]$Status
)
$ErrorActionPreference = 'Stop'
$Game = 'E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2'
$ProxySrc = 'E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\build\Release\libxess.dll'

$RealXess = Join-Path $Game 'libxess_orig.dll'
$Proxy    = Join-Path $Game 'libxess.dll'
$Amdnr    = Join-Path $Game 'dxgi.dll'
$AmdnrOff = Join-Path $Game 'dxgi.dll.停用-测试中'

function Show-Status {
    Write-Host '=== 当前状态 ==='
    $realOk  = Test-Path $RealXess
    Write-Host ('  真实 libxess 备份 (libxess_orig.dll) : ' + $(if($realOk){'有 ✓'}else{'缺失 !!'}))
    $pxSize = if (Test-Path $Proxy) { [math]::Round((Get-Item $Proxy).Length/1MB,2) } else { 0 }
    $rnSize = if ($realOk) { [math]::Round((Get-Item $RealXess).Length/1MB,2) } else { 0 }
    $kind = if (-not (Test-Path $Proxy)) { '无' }
            elseif ($rnSize -gt 1 -and $pxSize -lt 5) { '跨卡代理（我们）' }
            elseif ($rnSize -gt 1 -and $pxSize -gt 50) { '真实 XeSS（原版）' }
            else { '未知' }
    Write-Host ('  libxess.dll = ' + $pxSize + ' MB  → ' + $kind)
    Write-Host ('  dxgi.dll（AMDNR）    : ' + $(if(Test-Path $Amdnr){'已启用'}else{'未启用'}) +
                '   dxgi.dll.停用-测试中 : ' + $(if(Test-Path $AmdnrOff){'存在（被停用）'}else{'无'}))
    Write-Host ''
    Write-Host '  判读：AMDNR = dxgi.dll 存在 + libxess.dll 是真实 XeSS'
    Write-Host '        跨卡   = dxgi.dll 不存在 + libxess.dll 是 0.6MB 代理'
}

if ($Status -or -not $To) { Show-Status; return }

Get-Process -Name re2 -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host '请先关闭游戏（检测到 re2.exe）'; return
}

if ($To -eq 'AMDNR') {
    Write-Host '=== 切换到 AMDNR ==='
    # 1) 恢复真实 libxess
    if (-not (Test-Path $RealXess)) { throw '缺少 libxess_orig.dll，无法还原真实 XeSS' }
    Copy-Item $RealXess $Proxy -Force
    Write-Host '  libxess.dll ← 真实 XeSS 已还原'
    # 2) 恢复 dxgi.dll
    if (Test-Path $AmdnrOff) {
        if (Test-Path $Amdnr) { throw 'dxgi.dll 与 dxgi.dll.停用-测试中 同时存在，请人工确认' }
        Rename-Item -LiteralPath $AmdnrOff -NewName 'dxgi.dll' -Force
        Write-Host '  dxgi.dll ← AMDNR 已恢复'
    } elseif (Test-Path $Amdnr) {
        Write-Host '  dxgi.dll 已是启用状态'
    } else {
        Write-Host '  !! 找不到 AMDNR 的 dxgi.dll，请手动确认'
    }
}
elseif ($To -eq 'CrossGpu') {
    Write-Host '=== 切换到 跨卡 NR ==='
    if (-not (Test-Path $ProxySrc)) { throw ('找不到代理文件：' + $ProxySrc) }
    # 1) 备份真实 XeSS（仅当代理尚未安装）
    $pxSize = if (Test-Path $Proxy) { (Get-Item $Proxy).Length } else { 0 }
    if (-not (Test-Path $RealXess) -and $pxSize -gt 50MB) {
        Copy-Item $Proxy $RealXess -Force
        Write-Host '  已备份真实 XeSS → libxess_orig.dll'
    }
    # 2) 装代理
    Copy-Item $ProxySrc $Proxy -Force
    Write-Host ('  libxess.dll ← 代理 ' + [math]::Round((Get-Item $ProxySrc).Length/1KB) + ' KB')
    # 3) 停用 AMDNR
    if (Test-Path $Amdnr) {
        Rename-Item -LiteralPath $Amdnr -NewName 'dxgi.dll.停用-测试中' -Force
        Write-Host '  dxgi.dll → dxgi.dll.停用-测试中（AMDNR 已停用）'
    } else {
        Write-Host '  dxgi.dll 不存在（AMDNR 本就停用）'
    }
}
Show-Status