# check-shaders.ps1 —— 部署前离线预检：把源码里的 HLSL 字符串抽出来，用 fxc 逐个编译
#
# 为什么需要它（2026-10-28 的教训）：
#   探针代码被误插进 A 侧 kHLSL（那里没有 gMode 这个 cbuffer 字段）→ DownCS/ResidCS 编译失败
#   → 代理 "初始化失败，永久放弃" → A 形态完全没有 NR，但游戏照常能玩，日志里只有一行
#   混在几千行里的 "shader DownCS 编译失败"。浪费了两次游戏启动。
#
# 用法： pwsh -File check-shaders.ps1
#   全部 OK → 退出码 0；任何 FAIL → 退出码 1（部署脚本可以先跑它）

$ErrorActionPreference = 'Stop'
$src = 'E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\ingame\libxess_proxy_v9.cpp'
$fxc = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter fxc.exe -ErrorAction SilentlyContinue |
       Where-Object { $_.FullName -match 'x64' } | Select-Object -First 1 -ExpandProperty FullName
if (-not $fxc) { Write-Error '找不到 fxc.exe（Windows SDK）'; exit 1 }
if (-not (Test-Path $src)) { Write-Error "找不到源码 $src"; exit 1 }

# 源码里的着色器字符串名 → 入口点列表
$shaders = @(
    @{ name = 'kHLSL';        ents = @('DownCS', 'ResidCS') },
    @{ name = 'kBformResid';  ents = @('main') },
    @{ name = 'kBformDS';     ents = @('main') },
    @{ name = 'kBformUp';     ents = @('main') },
    @{ name = 'kBformTAA';    ents = @('main') },
    @{ name = 'kMvPackHLSL';  ents = @('main') }
)

# 注意：必须按 UTF-8 读源码、按【无 BOM】写出，否则 fxc 会报 X3000 Illegal character
$txt = [System.IO.File]::ReadAllText($src, [System.Text.Encoding]::UTF8)
$tmp = Join-Path $env:TEMP 'shcheck'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$enc = New-Object System.Text.UTF8Encoding($false)
$fail = 0
$ErrorActionPreference = 'Continue'   # fxc 的 warning 走 stderr，不能当致命错误

foreach ($s in $shaders) {
    $marker = "static const char* $($s.name)"
    $i = $txt.IndexOf($marker)
    if ($i -lt 0) { Write-Host "SKIP  $($s.name)（源码里找不到）" -ForegroundColor Yellow; continue }
    $b = $txt.IndexOf('R"(', $i)
    if ($b -lt 0) { Write-Host "SKIP  $($s.name)（不是 raw string）" -ForegroundColor Yellow; continue }
    $start = $b + 3
    $end = $txt.IndexOf(')"', $start)
    if ($end -lt 0) { Write-Host "FAIL  $($s.name)（raw string 未闭合）" -ForegroundColor Red; $fail++; continue }
    $path = Join-Path $tmp "$($s.name).hlsl"
    [System.IO.File]::WriteAllText($path, $txt.Substring($start, $end - $start), $enc)

    foreach ($e in $s.ents) {
        $out = & $fxc /nologo /T cs_5_0 /E $e /Fo (Join-Path $tmp 'out.cso') $path 2>&1 | Out-String
        $rc = $LASTEXITCODE
        if ($rc -eq 0) {
            Write-Host "OK    $($s.name) /E $e" -ForegroundColor Green
        } else {
            Write-Host "FAIL  $($s.name) /E $e" -ForegroundColor Red
            $out | Select-Object -First 5 | ForEach-Object { Write-Host "        $_" -ForegroundColor Red }
            $fail++
        }
    }
}

if ($fail) { Write-Host "`n共 $fail 个失败 —— 不要部署" -ForegroundColor Red; exit 1 }
Write-Host "`n全部通过，可以部署" -ForegroundColor Green
exit 0
