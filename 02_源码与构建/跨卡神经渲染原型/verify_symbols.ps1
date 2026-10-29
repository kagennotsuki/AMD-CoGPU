$ErrorActionPreference = 'Stop'
# 校验：源码里用到的每个 D3D12_* / ID3D12* 符号，是否真实存在于 d3d12.h
$src  = 'E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\src\transport_bench.cpp'
$hdr  = 'E:\AI\_归档\安装包暂存\d3d12hdr\d3d12.h'
$code = Get-Content -LiteralPath $src -Raw -Encoding UTF8
$head = Get-Content -LiteralPath $hdr -Raw -Encoding UTF8

$ids = [regex]::Matches($code, '\b(D3D12_[A-Za-z0-9_]+|ID3D12[A-Za-z0-9_]+|DXGI_[A-Za-z0-9_]+|D3D_FEATURE_LEVEL_[A-Za-z0-9_]+)\b') |
       ForEach-Object { $_.Value } | Sort-Object -Unique

Write-Host ("检查 {0} 个符号（源码 vs d3d12.h）" -f $ids.Count)
$missing = @()
foreach ($id in $ids) {
    if ($head -notmatch [regex]::Escape($id)) { $missing += $id }
}
if ($missing.Count -eq 0) {
    Write-Host ("全部 {0} 个符号都在 d3d12.h 中找到 ✅" -f $ids.Count)
} else {
    Write-Host ("未在 d3d12.h 中找到 {0} 个（可能属于 dxgi 头或名字写错）：" -f $missing.Count)
    $missing | ForEach-Object { Write-Host ('   ? ' + $_) }
}
Write-Host ''
Write-Host '=== 逐项清单 ==='
$ids | ForEach-Object { Write-Host ('   ' + $_) }
