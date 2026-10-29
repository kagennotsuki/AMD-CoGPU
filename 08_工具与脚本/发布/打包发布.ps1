# ============================================================================
#  打包发布.ps1 —— AMD CoGPU（方案 B：发布包不含运行时）
#
#  做什么：
#    1. 组装一个干净的发布目录：只放自己的东西 + 许可文件
#    2. 强制排除 payload\runtime（488 MB 第三方运行时与权重）
#    3. 生成 SHA256SUMS.txt
#    4. 调 发布前自检.ps1 复核，不通过就中止
#    5. 打成 zip
#
#  为什么排除 payload\runtime：
#    运行时（LmxxfNrRuntime.dll/.pak）与模型（dlssnr-amd\）不是本项目的东西，
#    由安装器在用户机上从上游 release 获取。这样本项目只分发自己的代码，
#    不继承第三方（含 NVIDIA 派生权重、AMDNR 的 GPL 构建物）的分发风险。
#    见 NOTICE 第 5、6 节。
#
#  用法：
#     .\打包发布.ps1                  # 打包
#     .\打包发布.ps1 -KeepDir        # 保留组装目录以便检查
# ============================================================================
param(
    [string]$InstallerDir = 'E:\AI\跨卡NR\08_工具与脚本\安装程序',
    [string]$LicenseDir   = 'E:\AI\跨卡NR\10_对外发布',
    [string]$OutDir       = 'E:\AI\跨卡NR\10_对外发布\发布包',
    [string]$Version      = (Get-Date -Format 'yyyyMMdd'),
    [switch]$KeepDir
)

$ErrorActionPreference = 'Stop'
function Head ($m) { Write-Host ""; Write-Host "===== $m =====" -ForegroundColor Cyan }
function Ok   ($m) { Write-Host "  ✔ $m" -ForegroundColor Green }
function Warn ($m) { Write-Host "  ▲ $m" -ForegroundColor Yellow }

# --- 0. 前置检查 -----------------------------------------------------------
Head '0. 前置检查'
if (-not (Test-Path $InstallerDir)) { throw "安装程序目录不存在：$InstallerDir" }
if (-not (Test-Path $LicenseDir))   { throw "许可目录不存在：$LicenseDir" }

$mustExist = @('AMD-CoGPU-Setup.ps1', '卸载.ps1', 'runtime-manifest.json')
foreach ($n in $mustExist) {
    if (-not (Test-Path (Join-Path $InstallerDir $n))) { throw "安装程序目录缺少 $n" }
}
$licMust = @('LICENSE','LICENSE-DOCS','NOTICE','THIRD-PARTY.md')
foreach ($n in $licMust) {
    if (-not (Test-Path (Join-Path $LicenseDir $n))) { throw "许可目录缺少 $n" }
}
Ok '目录与必要文件齐备'

# --- 1. 组装 ---------------------------------------------------------------
$stage = Join-Path $OutDir ("AMD-CoGPU-v$Version")
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

Head '1. 复制安装器脚本（不含 payload）'
foreach ($n in $mustExist) {
    Copy-Item (Join-Path $InstallerDir $n) (Join-Path $stage $n) -Force
    Ok $n
}

Head '2. 复制代理本体（payload 根目录，排除 runtime 与 .bak 备份）'
$payloadSrc = Join-Path $InstallerDir 'payload'
$payloadDst = Join-Path $stage 'payload'
New-Item -ItemType Directory -Path $payloadDst -Force | Out-Null
Get-ChildItem $payloadSrc -File | ForEach-Object {
    # 备份档（.bak / .bak-xxx）绝不能进发布包：它们只是本机回退用的旧版
    if ($_.Name -match '\.bak($|[-.])') {
        Warn ("已排除备份档 payload\{0}（{1:N0} B）" -f $_.Name, $_.Length)
        return
    }
    Copy-Item $_.FullName (Join-Path $payloadDst $_.Name) -Force
    $h = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0,16)
    Ok ("payload\{0}  ({1:N0} B  SHA256 {2}…)" -f $_.Name, $_.Length, $h)
}
$excluded = Join-Path $payloadSrc 'runtime'
if (Test-Path $excluded) {
    $sz = (Get-ChildItem $excluded -Recurse -File | Measure-Object Length -Sum).Sum
    Warn ("已排除 payload\runtime（{0:N1} MB，第三方运行时与权重，由用户机从上游获取）" -f ($sz/1MB))
} else {
    Warn 'payload\runtime 不存在（已经是干净状态）'
}

Head '3. 复制许可文件'
foreach ($n in $licMust) {
    Copy-Item (Join-Path $LicenseDir $n) (Join-Path $stage $n) -Force
    Ok $n
}

Head '3b. 复制第三方许可与署名声明（licenses\）'
#  自托管分发运行时时，pak 的"原文照录声明与署名"义务落在这些文件上。
#  安装器会把本目录整体复制到游戏目录的 licenses\（见 Setup 脚本 $licSrc）。
$licNeed = @('LmxxfNrRuntime.pak-NOTICE.txt', 'AMDNR_RDNA3_BACKEND.txt', 'lmxxf_LICENSE.txt')
$licSrcDir = Join-Path $InstallerDir 'licenses'
if (-not (Test-Path $licSrcDir)) { throw '安装程序缺少 licenses\ 目录（自托管分发运行时必须随附第三方声明）' }
foreach ($n in $licNeed) {
    if (-not (Test-Path (Join-Path $licSrcDir $n))) { throw ("licenses\ 缺少必需声明：{0}" -f $n) }
}
$licDstDir = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licDstDir -Force | Out-Null
Get-ChildItem $licSrcDir -File | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $licDstDir $_.Name) -Force
    Ok ("licenses\{0}  ({1:N0} B)" -f $_.Name, $_.Length)
}
# --- 2. 生成 SHA256SUMS ----------------------------------------------------
Head '4. 生成 SHA256SUMS.txt'
$sums = Get-ChildItem $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($stage.Length).TrimStart('\')
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $rel
}
Set-Content -Path (Join-Path $stage 'SHA256SUMS.txt') -Value ($sums -join "`r`n") -Encoding ascii
Ok ("SHA256SUMS.txt（{0} 个文件）" -f $sums.Count)

# --- 3. 自检 ---------------------------------------------------------------
Head '5. 发布前自检'
$checker = Join-Path $LicenseDir '发布前自检.ps1'
if (Test-Path $checker) {
    $out = & $checker -Path $stage 2>&1 | Out-String
    Write-Host $out
    if ($LASTEXITCODE -ne 0) {
        throw "自检未通过（见上方 ✘ 项）。发布包保留在 $stage 供排查，未打 zip。"
    }
    Ok '自检通过'
} else {
    Warn '找不到 发布前自检.ps1，跳过自检（不建议）'
}

# --- 4. 打包 ---------------------------------------------------------------
Head '6. 打 zip'
$zip = Join-Path $OutDir ("AMD-CoGPU-v$Version.zip")
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Ok ("{0}  ({1:N1} MB)" -f (Split-Path $zip -Leaf), ((Get-Item $zip).Length/1MB))

if (-not $KeepDir) { Remove-Item $stage -Recurse -Force }

Write-Host ""
Write-Host "发布包已就绪：$zip" -ForegroundColor Green
Write-Host "上传到 GitHub Releases 时，请一并附上：LICENSE / NOTICE / THIRD-PARTY.md / SHA256SUMS.txt" -ForegroundColor Gray
