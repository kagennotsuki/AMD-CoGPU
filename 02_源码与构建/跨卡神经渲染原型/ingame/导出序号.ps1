$ErrorActionPreference = 'Stop'
# 重新生成两个代理的 .def，导出序号与系统 DLL 完全一致
$dumpbin = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe'

function Get-Exports([string]$path) {
    $map = [ordered]@{}
    $out = & $dumpbin /exports $path 2>&1
    foreach ($l in $out) {
        if ($l -match '^\s+(\d+)\s+[0-9A-F]+\s+[0-9A-F]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*$') {
            $map[$matches[2]] = [int]$matches[1]
        }
    }
    $map
}

# ---------- dxgi ----------
$sysDxgi = Get-Exports 'C:\Windows\System32\dxgi.dll'
$ourDxgi = @('CreateDXGIFactory1', 'CreateDXGIFactory2')   # 自己实现的
$def = New-Object System.Collections.Generic.List[string]
$def.Add('LIBRARY dxgi')
$def.Add('EXPORTS')
foreach ($k in $sysDxgi.Keys) {
    $ord = $sysDxgi[$k]
    if ($ourDxgi -contains $k) {
        $def.Add("    $k @$ord")
    } else {
        $def.Add("    $k = fwd_$k @$ord")
    }
}
$defPath = 'E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\ingame\dxgi_proxy.def'
[System.IO.File]::WriteAllLines($defPath, $def, (New-Object System.Text.ASCIIEncoding))
Write-Host ("dxgi_proxy.def: {0} 个导出，序号与系统一致" -f $sysDxgi.Count)

# ---------- d3d12 ----------
$sysD3D = Get-Exports 'C:\Windows\System32\d3d12.dll'
$def2 = New-Object System.Collections.Generic.List[string]
$def2.Add('LIBRARY d3d12')
$def2.Add('EXPORTS')
foreach ($k in $sysD3D.Keys) {
    $ord = $sysD3D[$k]
    if ($k -eq 'D3D12CreateDevice') {
        $def2.Add("    $k = ProxyD3D12CreateDevice @$ord")
    } else {
        $def2.Add("    $k = d3dfwd_$k @$ord")
    }
}
# 我们自己的辅助导出放高位序号，避免撞车
$def2.Add('    CrossGpuQueueCount @500')
$def2.Add('    CrossGpuGetQueue @501')
$def2Path = 'E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\ingame\d3d12_proxy.def'
[System.IO.File]::WriteAllLines($def2Path, $def2, (New-Object System.Text.ASCIIEncoding))
Write-Host ("d3d12_proxy.def: {0} 个导出 + 2 个辅助，序号与系统一致" -f $sysD3D.Count)

Write-Host ''
Write-Host '=== dxgi_proxy.def 前 8 行 ==='
$def | Select-Object -First 8 | ForEach-Object { '  ' + $_ }
Write-Host '=== d3d12_proxy.def 前 8 行 ==='
$def2 | Select-Object -First 8 | ForEach-Object { '  ' + $_ }
