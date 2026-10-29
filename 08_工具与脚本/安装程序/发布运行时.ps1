#requires -Version 5.1
<#
      ⚠⚠  零分发模式提醒（2026-10-29 起）
      本脚本会把整个 payload\runtime（约 915MB，含 AMDNR 构建的 .pak 与权重）
      打进 runtime.zip 并写入清单的 selfHosted —— 这等于【由你承担第三方内容的分发】。
      如果本项目的发布策略是「零分发」（发布包只含 libxess.dll + 脚本 + 许可）：
          ⇒ 不要运行本脚本；请走「用户自备运行时」或「指向上游」路线。
      详见：06_交接包\安装器-交接包\交接-零分发模式与安装器问题-20261029.md
#>
param(
    [Parameter(Mandatory=$true)][string]$SourceGameDir,
    [string]$RepoUrl,            # 只在把包上传到 Release 之后填；-ZipOnly 时可省略
    [string[]]$Mirror = @(),
    [string]$OutDir,
    [switch]$SkipZip,
    [switch]$ZipOnly             # 只打包并打印 SHA，不改写清单（推荐：先出包上传，再单独跑 -SkipZip 填地址）
)

Write-Host '' + [char]0x26A0 + ' 警告：本脚本会打包并自托管第三方运行时（915MB）。零分发策略下不要运行。' -ForegroundColor Yellow
Write-Host ''
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) { $OutDir = Join-Path $here 'dist' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$zip = Join-Path $OutDir 'runtime.zip'

# ---------- 1) 打包 ----------
if (-not $SkipZip) {
    if (-not (Test-Path $SourceGameDir)) { throw "源目录不存在：$SourceGameDir" }
    $files = @('LmxxfNrRuntime.dll', 'MochizukiNrRuntime.dll', 'LmxxfNrRuntime.pak')
    $dirs  = @('dlssnr-amd', 'person-model')
    $stage = Join-Path $env:TEMP ('cogpu-rt-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Write-Host "收集运行时文件…"
    # 本机生成的编译缓存不属于上游内容，分发时排除
    $skipNames = @('pipeline.cache')
    foreach ($f in $files) {
        $src = Join-Path $SourceGameDir $f
        if (Test-Path $src) { Copy-Item $src (Join-Path $stage $f) -Force; Write-Host ("  + {0,-28} {1,8:N1} MB" -f $f, ((Get-Item $src).Length/1MB)) }
        else { Write-Host ("  ! 缺少 $f（跳过）") -ForegroundColor Yellow }
    }
    foreach ($d in $dirs) {
        $src = Join-Path $SourceGameDir $d
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $stage $d) -Recurse -Force
            if ($skipNames) { Get-ChildItem (Join-Path $stage $d) -Recurse -File | Where-Object { $skipNames -contains $_.Name } | Remove-Item -Force -ErrorAction SilentlyContinue }
            $s = Get-ChildItem (Join-Path $stage $d) -Recurse -File | Measure-Object Length -Sum
            Write-Host ("  + {0,-28} {1,8:N1} MB ({2} 文件)" -f ($d + '\'), ($s.Sum/1MB), $s.Count)
        } else { Write-Host ("  ! 缺少 $d\（跳过）") -ForegroundColor Yellow }
    }
    # 许可与署名文件（MIT 要求随分发附带）
    $licDir = Join-Path $here 'licenses'
    if (Test-Path $licDir) {
        Copy-Item $licDir (Join-Path $stage 'licenses') -Recurse -Force
        $lc = @(Get-ChildItem (Join-Path $stage 'licenses') -File)
        Write-Host ("  + {0,-28} {1} 个文件（MIT 许可与 pak 包内声明）" -f 'licenses\', $lc.Count)
    } else {
        Write-Host '  ! 缺少 licenses\（MIT 要求随分发附带声明与署名）' -ForegroundColor Yellow
    }

    $missing = @()
    foreach ($n in 'LmxxfNrRuntime.dll', 'LmxxfNrRuntime.pak', 'dlssnr-amd') {
        if (-not (Test-Path (Join-Path $stage $n))) { $missing += $n }
    }
    if ($missing.Count -gt 0) {
        Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
        throw ("缺少关键运行时：{0} —— 先把本机这套跑正常再发布" -f ($missing -join ', '))
    }
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Write-Host "压缩中（大文件较慢，请等待）…"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path $zip)) { throw "找不到 $zip（去掉 -SkipZip 先生成，或先手动放一份）" }

# ---------- 2) 算校验并改写清单 ----------
$zi  = Get-Item $zip
$sha = (Get-FileHash $zip -Algorithm SHA256).Hash
Set-Content -Path ($zip + '.sha256') -Value $sha -Encoding ASCII

if ($ZipOnly) {
    Write-Host ""
    Write-Host ("包已生成：{0}" -f $zip) -ForegroundColor Green
    Write-Host ("  size   = {0}" -f $zi.Length)
    Write-Host ("  sha256 = {0}" -f $sha)
    Write-Host ""
    Write-Host "下一步：把这个 zip 上传到 Release，然后运行（只改清单、不重新压缩）：" -ForegroundColor Cyan
    Write-Host ("  powershell -ExecutionPolicy Bypass -File 发布运行时.ps1 -SourceGameDir <游戏目录> -SkipZip -RepoUrl <附件地址>")
    exit 0
}

if (-not $RepoUrl) { throw "缺少 -RepoUrl（附件地址）。只想出包请加 -ZipOnly" }

$mfPath = Join-Path $here 'runtime-manifest.json'
$mf = if (Test-Path $mfPath) { Get-Content $mfPath -Raw -Encoding UTF8 | ConvertFrom-Json } else { $null }
if (-not $mf -or -not $mf.components) { throw '清单不是 v2 结构（缺 components 数组），先确认 runtime-manifest.json' }

# 写进 nr-runtime 组件的 selfHosted：安装器优先使用这个来源（上游 url 保留作参考/备用）
$comp = $mf.components | Where-Object { $_.id -eq 'nr-runtime' } | Select-Object -First 1
if (-not $comp) { $comp = $mf.components[0] }
$comp | Add-Member -NotePropertyName selfHosted -NotePropertyValue ([pscustomobject]@{
    url     = $RepoUrl
    mirrors = @($Mirror)
    size    = $zi.Length
    sha256  = $sha
}) -Force
($mf | ConvertTo-Json -Depth 8) | Set-Content -Path $mfPath -Encoding UTF8

Write-Host ""
Write-Host ("OK  runtime.zip  {0:N1} MB" -f ($zi.Length / 1MB)) -ForegroundColor Green
Write-Host ("    SHA256 = {0}" -f $sha)
Write-Host ("    自托管地址 = {0}（安装器优先用这个）" -f $RepoUrl)
if ($Mirror.Count -gt 0) { foreach ($m in $Mirror) { Write-Host ("    镜像   = {0}" -f $m) } }
Write-Host ("    清单已更新：{0}" -f $mfPath) -ForegroundColor Cyan
Write-Host ""
Write-Host "下一步：把 dist\runtime.zip 上传到上面的 Release（附件地址须与 -RepoUrl 完全一致）。" -ForegroundColor Yellow
Write-Host ""

# ---------- 3) 自检 ----------
$setup = Join-Path $here 'AMD-CoGPU-Setup.ps1'
if (Test-Path $setup) { & powershell -NoProfile -ExecutionPolicy Bypass -File $setup -SelfTest }
