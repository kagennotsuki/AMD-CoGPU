# 抓帧-对比.ps1 —— 全屏抓若干张 PNG，用于对比"无历史/有历史"两种状态的噪点水平
#
# 用法：
#   powershell -File 抓帧-对比.ps1 -Out "E:\AI\跨卡NR\07_素材与抓帧\噪声对比\passReset0" -Count 3 -DelayMs 400
#   powershell -File 抓帧-对比.ps1 -Out "...\passReset2" -Count 3 -DelayMs 400 -Monitor 2
#
# 说明：
#   · 抓的是**屏幕显示内容**（不是 ReShade 的截图通道），所以能看到 MGPU 叠加出来的神经画面；
#   · 静止场景下多抓几张，取同一区域的方差作为噪点指标，由后续步骤比较；
#   · 不修改游戏、不注入任何东西。
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [int]$Count = 3,
    [int]$DelayMs = 400,
    [int]$Monitor = 0,          # 0 = 主屏；1 = 第一块副屏；以此类推
    [int]$CropW = 0,            # 需要时只存中心区域（0 = 全屏）
    [int]$CropH = 0
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

if (-not (Test-Path -LiteralPath $Out)) { New-Item -ItemType Directory -Path $Out -Force | Out-Null }

# 目标屏幕的边界
$screens = [System.Windows.Forms.Screen]::AllScreens
if ($Monitor -ge $screens.Count) { throw "只有 $($screens.Count) 块屏，却要第 $Monitor 块" }
$b = $screens[$Monitor].Bounds
Write-Host ("屏幕 #{0}: {1}x{2} @ ({3},{4})" -f $Monitor, $b.Width, $b.Height, $b.X, $b.Y)

$w = $b.Width; $h = $b.Height
$srcX = $b.X;  $srcY = $b.Y
if ($CropW -gt 0 -and $CropH -gt 0) {
    $srcX = $b.X + [int](($b.Width  - $CropW) / 2)
    $srcY = $b.Y + [int](($b.Height - $CropH) / 2)
    $w = $CropW; $h = $CropH
}
Write-Host ("抓取区域: {0}x{1} @ ({2},{3})" -f $w, $h, $srcX, $srcY)

$made = @()
for ($i = 1; $i -le $Count; $i++) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($srcX, $srcY, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $g.Dispose()
    $p = Join-Path $Out ("shot{0}.png" -f $i)
    $bmp.Save($p, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $made += $p
    Write-Host ("  已存 {0}" -f $p)
    if ($i -lt $Count) { Start-Sleep -Milliseconds $DelayMs }
}

# 立刻算一个粗略的噪点指标：相邻像素差的平均绝对值（只取中心 1/4 区域，避开 UI 与 HUD）
function Noise-Metric([string]$path) {
    $img = [System.Drawing.Bitmap]::FromFile($path)
    try {
        $x0 = [int]($img.Width * 0.375); $y0 = [int]($img.Height * 0.375)
        $x1 = [int]($img.Width * 0.625); $y1 = [int]($img.Height * 0.625)
        $sum = 0.0; $n = 0
        for ($y = $y0; $y -lt $y1; $y += 3) {
            for ($x = $x0; $x -lt $x1; $x += 3) {
                $c1 = $img.GetPixel($x, $y); $c2 = $img.GetPixel($x + 1, $y)
                $l1 = 0.2126 * $c1.R + 0.7152 * $c1.G + 0.0722 * $c1.B
                $l2 = 0.2126 * $c2.R + 0.7152 * $c2.G + 0.0722 * $c2.B
                $sum += [math]::Abs($l1 - $l2); $n++
            }
        }
        return [math]::Round($sum / [math]::Max($n, 1), 3)
    } finally { $img.Dispose() }
}

Write-Host ''
Write-Host '=== 噪点指标（相邻像素亮度差的平均绝对值；越大=越噪）==='
foreach ($p in $made) { Write-Host ("  {0}  ->  {1}" -f (Split-Path $p -Leaf), (Noise-Metric $p)) }
