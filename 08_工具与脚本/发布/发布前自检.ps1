# ============================================================================
#  发布前自检.ps1 —— AMD CoGPU
#  作用：把"我有没有分发不该发的东西"从一个记忆问题变成一个可重复执行的检查。
#  用法：
#     .\发布前自检.ps1                                  # 查安装程序目录
#     .\发布前自检.ps1 -Path 'D:\要发布的包'             # 查指定目录
#     .\发布前自检.ps1 -Path 'D:\要发布的包' -Hash       # 慢，但会算大文件 SHA256
#  退出码：0 = 全部通过，1 = 有阻断项
# ============================================================================
param(
    [string]$Path = 'E:\AI\跨卡NR\08_工具与脚本\安装程序',
    [switch]$Hash
)

$ErrorActionPreference = 'Stop'
$block = 0
function Ok   ($m) { Write-Host "  ✔ $m" -ForegroundColor Green }
function Warn ($m) { Write-Host "  ▲ $m" -ForegroundColor Yellow }
function Fail ($m) { Write-Host "  ✘ $m" -ForegroundColor Red; $script:block++ }
function Head ($m) { Write-Host ""; Write-Host "===== $m =====" -ForegroundColor Cyan }

if (-not (Test-Path $Path)) { Write-Host "路径不存在：$Path" -ForegroundColor Red; exit 1 }
$Path = (Resolve-Path $Path).Path
Write-Host "自检目标：$Path" -ForegroundColor Gray

# ---------------------------------------------------------------------------
Head '① danielblnc —— 必须零命中'
#  他的许可：All rights reserved，禁止再分发，禁止修改/逆向。
#  你的项目一个字节都不能带。（来源：danielblnc/DLSS-NR-on-AMD 的 LICENSE）
# ---------------------------------------------------------------------------
$denyNames = @(
    'dlssnr_amd_pass*.dll',      # 他的运行时，v0.3.1 = 7,304,192 B
    'dlssnr_on_amd_weights.bin', # 由 NVIDIA nvngx_dlssnr.dll 生成的权重
    'neuralhub_host.dll',        # 0.8.0 宿主
    'dlssnr_core.dll',
    'AMDNRModelB*', 'amdnr-binding.json', 'dlssnr_on_amd.log'
)
$found = foreach ($n in $denyNames) {
    Get-ChildItem $Path -Recurse -File -Filter $n -ErrorAction SilentlyContinue
}
if ($found) {
    foreach ($f in $found) { Fail ("发现 danielblnc 的文件：{0}  ({1:N0} B)  {2}" -f $f.Name, $f.Length, $f.FullName) }
} else { Ok '无 danielblnc 的任何文件或引用' }

# 已知 SHA256（他的 v0.3.1 三份运行时完全相同）
$denyHash = 'b108d6407eb7f094a4f9111edd778eee7b978b648d413a9fc7aeedfdd914c154'
if ($Hash) {
    Get-ChildItem $Path -Recurse -File -Filter *.dll | ForEach-Object {
        if ((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower() -eq $denyHash) {
            Fail ("按 SHA256 命中 danielblnc v0.3.1 运行时：{0}" -f $_.FullName)
        }
    }
    Ok 'SHA256 指纹比对完成（无命中）'
}

# ---------------------------------------------------------------------------
Head '② NVIDIA —— 权重与运行时一律不得随包'
#  社区统一口径：任何包都不含 NVIDIA 权重，只能由用户在本地从自己的
#  nvngx_dlssnr.dll 提取。
# ---------------------------------------------------------------------------
$nv = Get-ChildItem $Path -Recurse -File -Include 'nvngx_*.dll','_nvngx.dll','nvapi64.dll' -ErrorAction SilentlyContinue
if ($nv) { foreach ($f in $nv) { Fail ("发现 NVIDIA 二进制：{0}  {1}" -f $f.Name, $f.FullName) } }
else { Ok '无 NVIDIA 二进制' }

# ---------------------------------------------------------------------------
Head '③ 权重三指纹 —— 必须是原样从上游拿的'
#  若大小或 SHA256 与上游不一致，说明它被自己重打包/改写过，
#  那一刻它就变成了"你自己生成/修改的权重"，性质完全不同。
# ---------------------------------------------------------------------------
$fp = @(
    @{ N='LmxxfNrRuntime.dll';     S=1433600;   H='9E7C9BAF57F47235' },
    @{ N='MochizukiNrRuntime.dll'; S=1433600;   H='9E7C9BAF57F47235' },  # 与上面同一份文件
    @{ N='LmxxfNrRuntime.pak';     S=509458860; H='270991761A5FC615' }
)
foreach ($x in $fp) {
    $f = Get-ChildItem $Path -Recurse -File -Filter $x.N -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $f) { Warn ("未找到 {0}（仅装代理的包可以没有）" -f $x.N); continue }
    if ($f.Length -ne $x.S) {
        Fail ("{0} 大小不符：{1:N0} B，应为 {2:N0} B ⇒ 被改写过" -f $x.N, $f.Length, $x.S)
    } elseif ($Hash) {
        $h = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToUpper()
        if ($h.Substring(0,16) -ne $x.H) { Fail ("{0} SHA256 不符：{1}" -f $x.N, $h.Substring(0,16)) }
        else { Ok ("{0} 指纹一致（{1:N0} B / {2}）" -f $x.N, $f.Length, $x.H) }
    } else { Ok ("{0} 大小一致（{1:N0} B），加 -Hash 可校验内容" -f $x.N, $f.Length) }
}

# 上游原压缩包内的 mtime 会保留下来；被自己重打包过的一定变成操作当时的时间
$pak = Get-ChildItem $Path -Recurse -File -Filter 'LmxxfNrRuntime.pak' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($pak -and $pak.LastWriteTime -gt (Get-Date '2026-10-20')) {
    Warn ("{0} 的 mtime = {1}，晚于上游发布日 ⇒ 可能被重写过" -f $pak.Name, $pak.LastWriteTime)
} elseif ($pak) {
    Ok ("{0} 保留了上游 mtime = {1}（未被改写的旁证）" -f $pak.Name, $pak.LastWriteTime)
}

# 本机运行时生成的缓存，不该进发布包
$cache = Get-ChildItem $Path -Recurse -File -Filter 'pipeline.cache' -ErrorAction SilentlyContinue
foreach ($c in $cache) { Warn ("发现运行时缓存（本机产物，建议剔除）：{0}" -f $c.FullName) }

# ---------------------------------------------------------------------------
Head '④ 符号文件 —— 泄漏 PDB 会把逆向成本从数百小时降到数十小时'
# ---------------------------------------------------------------------------
$sym = Get-ChildItem $Path -Recurse -File -Include '*.pdb','*.ilk','*.map','*.idb' -ErrorAction SilentlyContinue
if ($sym) { foreach ($f in $sym) { Fail ("发现符号文件：{0}  {1}" -f $f.Name, $f.FullName) } }
else { Ok '无 PDB / ILK / MAP' }

# ---------------------------------------------------------------------------
Head '⑤ 许可文件齐备'
# ---------------------------------------------------------------------------
$must = @{
    'LICENSE'        = 'GPL-3.0 全文（本项目代码）'
    'LICENSE-DOCS'   = 'CC BY-NC-SA 4.0 全文（本项目文档）'
    'NOTICE'         = '本项目署名与附加条款'
    'THIRD-PARTY.md' = '第三方组件与权利归属'
}
# 条件必需：只有把运行时随包分发时，才必须附其 MIT 原文。
# 方案 B（不分发运行时，由安装器从上游获取）下不适用。
$shipRuntime = @(Get-ChildItem $Path -Recurse -File -Include 'LmxxfNrRuntime.pak','LmxxfNrRuntime.dll' -ErrorAction SilentlyContinue).Count -gt 0
if ($shipRuntime) { $must['LICENSE-DLSSNR-AMD.txt'] = 'mochizuki0323 的 MIT 原文（随包分发其运行时为硬义务）' }

$root = Split-Path $Path -Parent
foreach ($k in $must.Keys) {
    $hit = @(Get-ChildItem $Path, $root -Recurse -File -Filter $k -ErrorAction SilentlyContinue)
    if ($hit.Count -gt 0) { Ok ("{0} 在（{1}）" -f $k, $must[$k]) }
    else { Fail ("缺少 {0} —— {1}" -f $k, $must[$k]) }
}
if ($shipRuntime) { Warn '本包含运行时本体 ⇒ 属于分发第三方内容，必须附 MIT 原文（已计入门槛）' }
else { Warn '本包不含运行时（装的时候才取）⇒ 运行时许可原文不强制随包；但安装器包仍须带 licenses\ 下的第三方声明，见下一条' }

# ---------------------------------------------------------------------------
# ⑤b 安装器包必须随附第三方声明（自托管分发 pak 时的"原文照录"义务落在它上面）
$isInstallerPkg = (Test-Path (Join-Path $Path 'AMD-CoGPU-Setup.ps1')) -and (Test-Path (Join-Path $Path 'runtime-manifest.json'))
if ($isInstallerPkg) {
    $needLic = @('LmxxfNrRuntime.pak-NOTICE.txt', 'AMDNR_RDNA3_BACKEND.txt', 'lmxxf_LICENSE.txt')
    $licDir  = Join-Path $Path 'licenses'
    foreach ($n in $needLic) {
        if (Test-Path (Join-Path $licDir $n)) { Ok ("licenses\{0} 在" -f $n) }
        else { Fail ("安装器包缺少 licenses\{0} —— 自托管分发运行时时这是硬义务" -f $n) }
    }
}
Head '⑥ warning 级：厂商二进制（权利归各家，最好不带）'
# ---------------------------------------------------------------------------
$vendor = Get-ChildItem $Path -Recurse -File -Include 'libxess_orig.dll','libxell*.dll','amd_fidelityfx_*.dll','dxgi.dll','D3D12Core.dll' -ErrorAction SilentlyContinue
if ($vendor) { foreach ($f in $vendor) { Warn ("带上了厂商/他人二进制：{0}（确认权利来源）" -f $f.Name) } }
else { Ok '无厂商二进制' }

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
Head '⑦ 源码树可构建性 —— CMakeLists 引用的文件必须都在包里'
#  防的是白名单漏了扩展名：只发 .cpp 不发 .asm/.def，
#  clone 下来根本编译不过，GPL 意义上的对应源码就不成立。
# ---------------------------------------------------------------------------
$cmlFile = Get-ChildItem $Path -Recurse -File -Filter 'CMakeLists.txt' -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -notmatch 'build-verify' } |
           Where-Object { $_.FullName -notmatch 'third_party' } |
           Where-Object { $_.FullName -notmatch '\\build\\' } |
           Select-Object -First 1
if (-not $cmlFile) {
    Warn '未找到 CMakeLists.txt，跳过可构建性检查'
} else {
    $cmlRoot = Split-Path $cmlFile.FullName -Parent
    $refs = @()
    foreach ($line in (Get-Content $cmlFile.FullName -Encoding UTF8)) {
        if ($line -notmatch '^\s*(add_library|add_executable|target_sources)\s*\(') { continue }
        # 变量引用（如 "${imguiDir}/imgui.cpp"）无法静态检查，整段剔除后再提取
        $scan = $line -replace '\$\{[^}]*\}[^\s)]*', ''
        foreach ($mm in [regex]::Matches($scan, '[A-Za-z0-9_./\\-]+\.(cpp|cxx|cc|asm|def|inc)')) { $refs += $mm.Value }
    }
    $refs = @($refs | Sort-Object -Unique)
    $lost = @()
    foreach ($r in $refs) { if (-not (Test-Path (Join-Path $cmlRoot $r))) { $lost += $r } }
    if ($lost.Count -gt 0) { foreach ($l in $lost) { Fail ("CMakeLists 引用但包内缺失：{0}" -f $l) } }
    else { Ok ("CMakeLists 引用的 {0} 个文件（源码/汇编/def）全部在包内" -f $refs.Count) }
}
Head '结论'
# ---------------------------------------------------------------------------
if ($block -eq 0) {
    Write-Host "  自检通过（$block 项阻断）。可以发布。" -ForegroundColor Green
    exit 0
} else {
    Write-Host "  有 $block 项阻断，不要发布。" -ForegroundColor Red
    exit 1
}
