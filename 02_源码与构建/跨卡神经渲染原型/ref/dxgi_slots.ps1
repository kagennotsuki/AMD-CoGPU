$ErrorActionPreference = 'Stop'
# 从 SDK 的 dxgi 头文件解析真实 vtable 槽位（MIDL_INTERFACE + STDMETHODCALLTYPE 风格）
$inc = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
$files = @(
    (Join-Path $inc 'shared\dxgi.h'),
    (Join-Path $inc 'shared\dxgi1_2.h'),
    (Join-Path $inc 'shared\dxgi1_3.h')
)

$ifaces = [ordered]@{}
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) { continue }
    $lines = Get-Content -LiteralPath $f -Encoding UTF8
    $i = 0
    while ($i -lt $lines.Count) {
        if ($lines[$i] -match '^\s*(IDXGI[A-Za-z0-9_]+)\s*:\s*public\s+(IDXGI[A-Za-z0-9_]+|IUnknown)\s*$') {
            $name = $matches[1]
            $parent = $matches[2]
            $methods = New-Object System.Collections.ArrayList
            $j = $i + 1
            $done = $false
            while (-not $done -and $j -lt $lines.Count) {
                $line = $lines[$j]
                if ($line -match 'virtual\s+.*?STDMETHODCALLTYPE\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(') {
                    $m = $matches[1]
                    $dup = ($methods.Count -gt 0 -and $methods[$methods.Count - 1] -eq $m)
                    if (-not $dup) { [void]$methods.Add($m) }
                }
                if ($line -match '^\s*\};') { $done = $true }
                $j++
            }
            if (-not $ifaces.Contains($name)) {
                $ifaces[$name] = [pscustomobject]@{ parent = $parent; methods = $methods }
            }
            $i = $j
        } else { $i++ }
    }
}

function Slots([string]$name) {
    $chain = New-Object System.Collections.ArrayList
    $cur = $name
    while ($cur -and $cur -ne 'IUnknown' -and $script:ifaces.Contains($cur)) {
        [void]$chain.Insert(0, $cur)
        $cur = $script:ifaces[$cur].parent
    }
    $map = [ordered]@{}
    $idx = 3
    foreach ($n in $chain) {
        foreach ($m in $script:ifaces[$n].methods) { $map[$m] = $idx; $idx++ }
    }
    Write-Host ("=== " + $name + "  chain: " + ($chain -join ' -> '))
    foreach ($k in $map.Keys) { Write-Host ("   {0,3}  {1}" -f $map[$k], $k) }
    Write-Host ''
}

$script:ifaces = $ifaces
Write-Host ("parsed: " + $ifaces.Count + " interfaces`n")
Slots 'IDXGIFactory2'
Slots 'IDXGISwapChain1'
