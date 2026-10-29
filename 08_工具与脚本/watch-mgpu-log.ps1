# watch-mgpu-log.ps1 —— 现场盯 ReShade.log 里的 MGPU 关键行
# 用法: powershell -File watch-mgpu-log.ps1
# 只打印"有信息量"的行；把最吵的 R71/R82/R101/P9.1 诊断行过滤掉（它们每次 600 帧就刷一遍）。
param(
    [string]$Log = 'E:\SteamLibrary\steamapps\common\RESIDENT EVIL 4  BIOHAZARD RE4\ReShade.log',
    [int]$PollMs = 700
)
$ErrorActionPreference = 'Continue'

# 想看到的事件（白名单）。命中即打印。
$keep = @(
    'Initializing crosire',
    'Loading add-on',
    'Registered add-on',
    '\[MGPU\]\[T1\]','\[MGPU\]\[T2\]','\[MGPU\]\[T3\]','\[MGPU\]\[T4\]','\[MGPU\]\[T5\]',
    '\[MGPU\]\[R141\]','\[MGPU\]\[P7\.2\]','\[MGPU\]\[P7\.10\]',
    '\[MGPU\]\[P4\.0\]','\[MGPU\]\[P4\.1\]','\[MGPU\]\[P4\.2\]',
    '\[MGPU\]\[R63\]','\[MGPU\]\[R78\]','\[MGPU\]\[R139\]','\[MGPU\]\[R185\]','\[MGPU\]\[R189\]',
    '\[MGPU\]\[R280\]','\[MGPU\]\[R312\]','\[MGPU\]\[SEAL\]','\[MGPU\]\[R26\]','\[MGPU\]\[R27',
    '\[MGPU\]\[V49\]','\[MGPU\]\[V53\]','\[MGPU\]\[V55\]','\[MGPU\]\[R273\]','\[MGPU\]\[R267\]',
    '\[MGPU\]\[R110\]','\[MGPU\]\[R115\]','\[MGPU\]\[C2-SR\]','\[MGPU\]\[RSD\]',
    '\[MGPU\].*(REFUSING|FATAL|MISMATCH|ERROR|FAILED|refus)',
    'Destroyed runtime environment','Reference count'
)
# 明确不要的（噪声）
$drop = @('\[R71\]','\[R82\]','\[R101\]','\[P9\.1\]','\[R72\]','\[R87\]','\[R167\]','\[R168\]','\[R258\]','\[R300')

function Cut([string]$s, [int]$n = 240) {
    $s = $s.TrimEnd()
    if ($s.Length -gt $n) { return $s.Substring(0, $n) + ' …' } else { return $s }
}

$off = 0
if (Test-Path -LiteralPath $Log) { $off = (Get-Item -LiteralPath $Log).Length }
Write-Output ("[watch] 开始盯：$Log  (从偏移 $off 起)")
Write-Output "[watch] 只打印关键行；R71/R82/R101/P9.1 等高频诊断已静音。Ctrl+C 或杀进程停止。"

while ($true) {
    try {
        if (Test-Path -LiteralPath $Log) {
            $len = (Get-Item -LiteralPath $Log).Length
            if ($len -lt $off) {
                $off = 0
                Write-Output "======== [watch] 日志被重写（新的一次启动） ========"
            }
            if ($len -gt $off) {
                $fs = [System.IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
                try {
                    [void]$fs.Seek($off, 'Begin')
                    $sr = New-Object System.IO.StreamReader($fs)
                    $txt = $sr.ReadToEnd()
                    $off = $fs.Position
                    $sr.Close()
                } finally { $fs.Close() }
                foreach ($line in ($txt -split "`r?`n")) {
                    if ([string]::IsNullOrWhiteSpace($line)) { continue }
                    $skip = $false
                    foreach ($d in $drop) { if ($line -match $d) { $skip = $true; break } }
                    if ($skip) { continue }
                    foreach ($k in $keep) {
                        if ($line -match $k) { Write-Output (Cut $line); break }
                    }
                }
            }
        }
    } catch {
        Write-Output ("[watch] 读日志出错（可能正在被写）: " + $_.Exception.Message)
    }
    Start-Sleep -Milliseconds $PollMs
}
