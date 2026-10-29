#requires -Version 5.1
<#
    AMD CoGPU · 安装程序
    ---------------------------------------------------------------
    向导页：
      1) 欢迎 / 游戏内用法（热键与功能）
      2) 选择游戏目录
      3) 游戏是否具备超分功能？（否 → 告知不可安装并结束）
      4) 选择游戏卡 / 神经卡（强烈建议神经卡为 9000 系）
      5) 安装设定（形态 / 队列钩子 / NR 默认）
      6) 安装（备份 → 复制 → 写配置）
      7) 完成（生成使用说明与安装报告）

    用法：双击「启动安装.bat」，或
          pwsh -ExecutionPolicy Bypass -File AMD-CoGPU-Setup.ps1
          自检（无界面，用于验证打包是否完整）：
          pwsh -ExecutionPolicy Bypass -File AMD-CoGPU-Setup.ps1 -SelfTest
#>
[CmdletBinding()] param(
    [switch]$SelfTest,
    [string]$TestGameDir,
    [string]$TestNrDeviceId = '7550',
    [string]$TestBform      = '9',
    [int]$StartPage = 0,          # 仅测试用：直接打开某一页截图用
    [string]$InstallTo,           # 无界面安装到指定目录（测试/批量用）
    [string]$RuntimeMode = 'auto', # auto | download | skip
    [string]$NrDev = '7550',
    [string]$Bform = '0',
    [switch]$UiSmokeTest           # 界面冒烟测试：逐页构建并戳控件，验证事件处理不炸
)

$ErrorActionPreference = 'Stop'
$script:LogLines = New-Object System.Collections.ArrayList
$script:LogBox   = $null
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$script:Root     = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:Payload  = Join-Path $script:Root 'payload'
$script:Version  = 'B4K-20261029c'
$script:AppName  = 'AMD CoGPU'

# ---------------------------------------------------------------- 通用工具

function Write-Log([string]$msg, [string]$level = 'INFO') {
    $line = '[{0}] {1,-5} {2}' -f (Get-Date -Format 'HH:mm:ss'), $level, $msg
    $script:LogLines.Add($line) | Out-Null
    if ($script:LogBox) { $script:LogBox.AppendText($line + "`r`n"); $script:LogBox.ScrollToCaret(); [System.Windows.Forms.Application]::DoEvents() }
}

function Get-VramMap {
    # 注册表里的 qwMemorySize 是真实显存（WMI 的 AdapterRAM 只能到 4GB，会误报）
    $map = @{}
    $base = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
    if (-not (Test-Path $base)) { return $map }
    foreach ($k in Get-ChildItem $base -ErrorAction SilentlyContinue) {
        try {
            $pr = Get-ItemProperty $k.PSPath -ErrorAction Stop
            $mid = [string]$pr.MatchingDeviceId
            $qw  = $pr.'HardwareInformation.qwMemorySize'
            if ($mid -match 'DEV_([0-9A-Fa-f]{4})' -and $qw) {
                $dev = $Matches[1].ToUpper()
                $gb = [math]::Round([double]$qw / 1GB, 0)
                if (-not $map.ContainsKey($dev) -or $map[$dev] -lt $gb) { $map[$dev] = $gb }
            }
        } catch { }
    }
    return $map
}

function Get-Gpus {
    # 只用 DeviceId 做判定，名称仅用于显示与"建议"提示（显卡可能被改名）
    $vram = Get-VramMap
    $out = @()
    foreach ($v in Get-CimInstance Win32_VideoController) {
        $dev = ''
        if ($v.PNPDeviceID -match 'DEV_([0-9A-Fa-f]{4})') { $dev = $Matches[1].ToUpper() }
        $isAmd = ($v.PNPDeviceID -match 'VEN_1002')
        $ramGB = if ($vram.ContainsKey($dev)) { $vram[$dev] } elseif ($v.DeviceId -eq '') { 0 } else { 4 }   # 取不到时按 4GB 占位
        $out += [pscustomobject]@{
            Name     = $v.Name
            DeviceId = $dev
            Vendor   = if ($isAmd) { 'AMD' } else { '其他' }
            IsAmd    = $isAmd
            VramGB   = $ramGB
            Driver   = $v.DriverVersion
        }
    }
    return $out | Where-Object { $_.DeviceId -ne '' }
}

function Test-Rdna4([string]$deviceId) {
    # 9000 系（RDNA4 / Navi48 等）的 PCI DeviceId 目前落在 0x75xx 段
    if ($deviceId -match '^75[0-9A-F]{2}$') { return $true }
    return $false
}

function Test-GameDir([string]$dir) {
    if ([string]::IsNullOrWhiteSpace($dir) -or -not (Test-Path $dir)) { return @{ Ok = $false; Msg = '目录不存在' } }
    $exe = Get-ChildItem $dir -Filter '*.exe' -File -ErrorAction SilentlyContinue
    if (-not $exe) { return @{ Ok = $false; Msg = '目录里没有 exe，可能选错了（请选游戏根目录）' } }
    $names = ($exe | Select-Object -First 4 | ForEach-Object { $_.Name }) -join ', '
    return @{ Ok = $true; Msg = "检测到可执行文件：$names" }
}

# ---------------------------------------------------------------- 运行时（从仓库下载）

function Get-RuntimeManifest {
    $mf = Join-Path $script:Root 'runtime-manifest.json'
    if (-not (Test-Path $mf)) { return $null }
    try { return Get-Content $mf -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return $null }
}

# 兼容 v1（单个 zip + expect）与 v2（components 数组）
function Get-RuntimeComponents {
    $mf = Get-RuntimeManifest
    if (-not $mf) { return @() }
    if ($mf.components) { return @($mf.components) }
    if ($mf.zip) {
        return @([pscustomobject]@{
            id = 'nr-runtime'; title = '神经降噪运行时'; author = 'mochizuki0323'; license = 'MIT';
            url = $mf.zip.url; mirrors = $mf.zip.mirrors; size = $mf.zip.size; sha256 = $mf.zip.sha256;
            type = 'zip'; expect = @($mf.expect); proxyRequires = @()
        })
    }
    return @()
}

function Test-ComponentPresent {
    param($Comp, [string]$Dir)
    if ([string]::IsNullOrWhiteSpace($Dir) -or -not (Test-Path $Dir)) { return $false }
    $need = @($Comp.expect)
    if ($need.Count -eq 0) { $need = @('dlssnr-amd') }
    foreach ($n in $need) { if (-not (Test-Path (Join-Path $Dir $n))) { return $false } }
    return $true
}

function Test-RuntimePresent([string]$dir) {
    $comps = Get-RuntimeComponents
    if ($comps.Count -eq 0) { return $false }
    foreach ($c in $comps) { if (-not (Test-ComponentPresent -Comp $c -Dir $dir)) { return $false } }
    return $true
}

function Invoke-Download {
    param([string]$Url, [string]$OutFile, [scriptblock]$OnProgress)
    if (Test-Path $OutFile) { Remove-Item $OutFile -Force -ErrorAction SilentlyContinue }
    $wc = New-Object System.Net.WebClient
    $wc.Headers.Add('User-Agent', 'AMD-CoGPU-Setup')
    $script:__lastPct = -1
    $handler = {
        param($sender, $e)
        if ($e.TotalBytesToReceive -gt 0) {
            $pct = [int](100 * $e.BytesReceived / $e.TotalBytesToReceive)
            if ($pct -ne $script:__lastPct -and ($pct % 5 -eq 0)) {
                $script:__lastPct = $pct
                if ($OnProgress) { & $OnProgress $pct ([math]::Round($e.BytesReceived / 1MB, 1)) ([math]::Round($e.TotalBytesToReceive / 1MB, 1)) }
            }
        } elseif ($e.BytesReceived -gt 0) {
            if ($OnProgress) { & $OnProgress -1 ([math]::Round($e.BytesReceived / 1MB, 1)) 0 }
        }
    }
    $sub = Register-ObjectEvent -InputObject $wc -EventName DownloadProgressChanged -Action $handler
    try {
        $wc.DownloadFileTaskAsync($Url, $OutFile).Wait()
    } catch {
        throw $_.Exception.GetBaseException().Message
    } finally {
        Unregister-Event -SubscriptionId $sub.Id -ErrorAction SilentlyContinue
        $wc.Dispose()
    }
    if (-not (Test-Path $OutFile)) { throw '下载未产生文件' }
}

function Install-Component {
    param($Comp, [string]$GameDir, [scriptblock]$OnProgress)
    $src = $null
    if ($Comp.selfHosted -and $Comp.selfHosted.url -and $Comp.selfHosted.url -notlike '*<*') {
        $src = $Comp.selfHosted; Write-Log ("组件 {0}：使用作者自托管包" -f $Comp.id)
    } elseif ($Comp.url -and $Comp.url -notlike '*<*') {
        $src = $Comp; Write-Log ("组件 {0}：从上游下载（{1}）" -f $Comp.id, $Comp.author)
    } else {
        throw ("组件 {0} 没有可用的下载地址" -f $Comp.id)
    }
    $urls = @($src.url); if ($src.mirrors) { $urls += @($src.mirrors) }
    $zip = Join-Path $env:TEMP ('cogpu-' + $Comp.id + '-' + [guid]::NewGuid().ToString('N') + '.zip')
    $ok = $false
    foreach ($u in $urls) {
        if (-not $u) { continue }
        Write-Log ("下载 {0}：{1}" -f $Comp.id, $u)
        try { Invoke-Download $u $zip $OnProgress; $ok = $true; break }
        catch { Write-Log ("该地址失败：{0}" -f $_.Exception.GetBaseException().Message) 'WARN' }
    }
    if (-not $ok) { throw ("组件 {0}：所有地址都下载失败" -f $Comp.id) }
    Write-Log ("下载完成：{0:N1} MB" -f ((Get-Item $zip).Length / 1MB))
    if ($src.sha256) {
        $h = (Get-FileHash $zip -Algorithm SHA256).Hash
        if ($h -ne $src.sha256.ToUpper()) { Remove-Item $zip -Force -ErrorAction SilentlyContinue
            throw ("组件 {0} 校验失败（期望 {1}… 实际 {2}…）" -f $Comp.id, $src.sha256.Substring(0,16), $h.Substring(0,16)) }
        Write-Log 'SHA256 校验通过'
    } else { Write-Log '清单未提供 sha256，跳过校验' 'WARN' }
    Write-Log '解压到游戏目录…'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $GameDir)
    Remove-Item $zip -Force -ErrorAction SilentlyContinue

    # 期望存在的文件
    $miss = @()
    foreach ($n in @($Comp.expect)) { if ($n -and -not (Test-Path (Join-Path $GameDir $n))) { $miss += $n } }
    if ($miss.Count -gt 0) { throw ("组件 {0} 解压后缺少：{1}" -f $Comp.id, ($miss -join ', ')) }

    # 我们代理真正需要的文件（兼容性门）
    $preq = @()
    foreach ($n in @($Comp.proxyRequires)) { if ($n -and -not (Test-Path (Join-Path $GameDir $n))) { $preq += $n } }
    if ($preq.Count -gt 0) {
        $msg = "组件 {0} 已下载并解压，但【与我们的代理不兼容】：缺少 {1}。" -f $Comp.id, ($preq -join ', ')
        $msg += "`n  说明：上游包的文件名/布局与 libxess.dll 期望的不一致（我们按 LmxxfNrRuntime.* 加载运行时）。"
        $msg += "`n  处理办法：① 在清单里把 selfHosted.url 指向适配过的包；② 或先把本机已能正常跑的运行时放进游戏目录，再重跑安装。"
        throw $msg
    }
    if (@($Comp.proxyRequires).Count -gt 0) { Write-Log '代理所需文件齐备（兼容性检查通过）' 'OK' }
    Write-Log ("组件 {0} 安装完成" -f $Comp.id) 'OK'
}

# ---------------------------------------------------------------- 安装执行

function New-InstallReport {
    param([hashtable]$Plan, $Items, [string]$RuntimeDone)
    $rep = Join-Path $Plan.GameDir ("AMD-CoGPU-安装报告-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $rt = @()
    $rt += 'AMD CoGPU 安装报告'
    $rt += ('版本      ：' + $script:Version)
    $rt += ('时间      ：' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
    $rt += ('游戏目录  ：' + $Plan.GameDir)
    $rt += ('游戏卡    ：' + $Plan.GameGpuName + '（DeviceId 0x' + $Plan.GameDeviceId + '）')
    $rt += ('神经卡    ：' + $Plan.NrGpuName + '（DeviceId 0x' + $Plan.NrDeviceId + '）')
    $rt += ('默认形态  ：' + $(if ($Plan.Bform -eq '9') { 'B-4K（4K 后置）' } else { 'A 形态' }))
    $rt += ('队列钩子  ：' + $(if ($Plan.QueueHook -eq '1') { '开' } else { '关' }))
    $rt += ('运行时    ：' + $RuntimeDone)
    $rt += ''
    $rt += '---- 动作明细 ----'
    foreach ($it in $Items) { $rt += ('{0,-4} {1,-24} {2}' -f $it.Action, $it.File, $it.Note) }
    $rt += ''
    $rt += '---- 安装后文件校验 ----'
    foreach ($n in 'libxess.dll','xess_proxy.bform.txt','xess_proxy.queuehook.txt','xess_proxy.gpu.txt','xess_proxy.dumpshot.txt','xess_proxy.saved.ini','LmxxfNrRuntime.dll','LmxxfNrRuntime.pak') {
        $q = Join-Path $Plan.GameDir $n
        if (Test-Path $q) {
            $i = Get-Item $q
            $h = if ($i.Length -gt 0) { (Get-FileHash $q -Algorithm SHA256).Hash.Substring(0,16) } else { '-' }
            $rt += ('{0,-26} {1,10} B  SHA256前16 {2}' -f $n, $i.Length, $h)
        } else { $rt += ('{0,-26} 缺失' -f $n) }
    }
    Set-Content -Path $rep -Value ($rt -join "`r`n") -Encoding UTF8
    return $rep
}
function Invoke-Uninstall {
    param([string]$GameDir, [switch]$KeepRuntime, [switch]$Quiet)
    if (-not (Test-Path $GameDir)) { throw "目录不存在：$GameDir" }
    $g = (Resolve-Path $GameDir).Path
    $script:__uninstLog = @()   # 脚本块里改局部变量无效，统一写 script 作用域
    $say = { param($m) if (-not $Quiet) { Write-Log $m }; $script:__uninstLog += $m }

    # 1) 配置与生成文档
    foreach ($n in 'xess_proxy.bform.txt','xess_proxy.queuehook.txt','xess_proxy.gpu.txt','xess_proxy.dumpshot.txt',
                   'xess_proxy.saved.ini','xess_proxy.model.txt','xess_proxy.taa.txt','xess_proxy.diag5.txt',
                   'xess_proxy.mirror.txt','xess_proxy.resid4k.txt','xess_proxy.bwtest.txt','xess_proxy.add4k.hlsl') {
        $q = Join-Path $g $n
        if (Test-Path $q) { Remove-Item $q -Force -ErrorAction SilentlyContinue; & $say ("删除 " + $n) }
    }
    Get-ChildItem $g -File -Filter 'AMD-CoGPU-*' -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue; & $say ("删除 " + $_.Name)
    }
    $licDir = Join-Path $g 'licenses'
    if (Test-Path $licDir) { Remove-Item $licDir -Recurse -Force -ErrorAction SilentlyContinue; & $say '删除 licenses\' }

    # 2) 运行时（可选保留）
    if (-not $KeepRuntime) {
        foreach ($n in 'LmxxfNrRuntime.dll','LmxxfNrRuntime.pak','MochizukiNrRuntime.dll') {
            $q = Join-Path $g $n
            if (Test-Path $q) { Remove-Item $q -Force -ErrorAction SilentlyContinue; & $say ("删除 " + $n) }
        }
        foreach ($n in 'dlssnr-amd','person-model') {
            $q = Join-Path $g $n
            if (Test-Path $q) { Remove-Item $q -Recurse -Force -ErrorAction SilentlyContinue; & $say ("删除 " + $n + '\') }
        }
    }

    # 3) 恢复原版 XeSS
    $orig = Join-Path $g 'libxess_orig.dll'
    $live = Join-Path $g 'libxess.dll'
    if ((Test-Path $orig) -and (Test-Path $live) -and ((Get-Item $live).Length -lt 2000000)) {
        Remove-Item $live -Force -ErrorAction SilentlyContinue
        Move-Item $orig $live -Force
        & $say '恢复原版 XeSS（libxess_orig.dll → libxess.dll）'
    } else {
        $bak = Get-ChildItem $g -File -Filter 'libxess.dll.bak-setup-*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($bak -and ((Test-Path $live) -eq $false -or (Get-Item $live).Length -lt 2000000)) {
            Remove-Item $live -Force -ErrorAction SilentlyContinue
            Copy-Item $bak.FullName $live -Force
            & $say ("恢复原版 XeSS（" + $bak.Name + " → libxess.dll）")
        } elseif (Test-Path $live) {
            & $say '提示：libxess.dll 看起来不是我们的代理（未改名），未做替换'
        }
    }
    Get-ChildItem $g -File -Filter 'libxess.dll.bak-setup-*' -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue; & $say ("删除 " + $_.Name)
    }
    return $script:__uninstLog
}

function Install-RuntimeComponents {
    param([string]$GameDir, [string]$Mode, [scriptblock]$OnProgress)
    $comps = Get-RuntimeComponents
    $localRt = Join-Path $script:Payload 'runtime'
    $done = @()
    if ($Mode -eq 'skip') { Write-Log '按用户选择：跳过运行时'; return @{ Done = '跳过'; Items = @() } }
    if ($comps.Count -eq 0) { Write-Log '清单里没有运行时组件，跳过' 'WARN'; return @{ Done = '清单为空'; Items = @() } }
    foreach ($c in $comps) {
        $present = Test-ComponentPresent -Comp $c -Dir $GameDir
        if ($present -and $Mode -ne 'download') {
            Write-Log ("组件 {0}：目标目录已有 → 跳过" -f $c.id)
            $done += ("{0}=已有" -f $c.id); continue
        }
        if ((Test-Path $localRt) -and $Mode -ne 'download') {
            Write-Log '安装包内自带 payload\runtime → 用本地副本（离线安装）'
            $rf = Get-ChildItem $localRt -Recurse -File
            foreach ($f in $rf) {
                $rel = $f.FullName.Substring($localRt.Length).TrimStart('\')
                $dst = Join-Path $GameDir $rel
                $dd = Split-Path $dst -Parent
                if (-not (Test-Path $dd)) { New-Item -ItemType Directory -Path $dd -Force | Out-Null }
                Copy-Item $f.FullName $dst -Force
            }
            Write-Log ("本地运行时复制完成：{0} 个文件" -f $rf.Count) 'OK'
            $done += ("{0}=本地副本" -f $c.id); continue
        }
        Install-Component -Comp $c -GameDir $GameDir -OnProgress $OnProgress
        $done += ("{0}=下载({1})" -f $c.id, $c.author)
    }
    return @{ Done = ($done -join '; '); Items = @() }
}
function Invoke-Install {
    param([hashtable]$Plan)

    $game = $Plan.GameDir
    $ts   = Get-Date -Format 'yyyyMMdd-HHmmss'
    $items = @()

    # 1) 备份同名文件
    foreach ($n in @('libxess.dll', 'xess_proxy.gpu.txt', 'xess_proxy.bform.txt', 'xess_proxy.queuehook.txt')) {
        $p = Join-Path $game $n
        if (Test-Path $p) {
            $bak = "$p.bak-setup-$ts"
            Copy-Item $p $bak -Force
            Write-Log "备份 $n → $(Split-Path $bak -Leaf)"
            $items += [pscustomobject]@{ Action = '备份'; File = $n; Note = (Split-Path $bak -Leaf) }
        }
    }

    # 1b) 首次部署：把游戏自带的原版 XeSS 改名成 libxess_orig.dll（我们的代理会加载它）
    $origDll = Join-Path $game 'libxess_orig.dll'
    $liveDll = Join-Path $game 'libxess.dll'
    if ((Test-Path $liveDll) -and -not (Test-Path $origDll)) {
        $len = (Get-Item $liveDll).Length
        if ($len -gt 2MB) {   # 原版 Intel XeSS 有 70MB+；我们的代理只有 ~790KB
            Move-Item $liveDll $origDll -Force
            Write-Log ("原版 XeSS 已改名：libxess.dll（{0:N1} MB）→ libxess_orig.dll" -f ($len / 1MB))
            $items += [pscustomobject]@{ Action = '改名'; File = 'libxess.dll'; Note = '→ libxess_orig.dll（原版 XeSS）' }
        } else {
            Write-Log '目标目录的 libxess.dll 看起来已经是我们的代理（<2MB），跳过改名' 'WARN'
        }
    } elseif (Test-Path $origDll) {
        Write-Log '原版 XeSS（libxess_orig.dll）已存在，无需改名'
    }

    # 2) 复制代理 DLL
    $dllSrc = Join-Path $script:Payload 'libxess.dll'
    if (-not (Test-Path $dllSrc)) { throw "安装包缺少 payload\libxess.dll" }
    Copy-Item $dllSrc (Join-Path $game 'libxess.dll') -Force
    Write-Log "复制 libxess.dll（$([math]::Round((Get-Item $dllSrc).Length/1KB,0)) KB）"
    $items += [pscustomobject]@{ Action = '安装'; File = 'libxess.dll'; Note = '跨卡代理' }

    # 3) 复制参数基线（若安装包里有）
    $iniSrc = Join-Path $script:Payload 'xess_proxy.saved.ini'
    if (Test-Path $iniSrc) {
        Copy-Item $iniSrc (Join-Path $game 'xess_proxy.saved.ini') -Force
        Write-Log '复制 xess_proxy.saved.ini（参数基线：gateK=15 / mvConf=2.0）'
        $items += [pscustomobject]@{ Action = '安装'; File = 'xess_proxy.saved.ini'; Note = '参数基线' }
    }

    # 3b) 许可与署名文件（分发第三方 pak / 模型时必须随附，MIT 要求保留声明与署名）
    $licSrc = Join-Path $script:Root 'licenses'
    if (Test-Path $licSrc) {
        $licDst = Join-Path $game 'licenses'
        if (-not (Test-Path $licDst)) { New-Item -ItemType Directory -Path $licDst -Force | Out-Null }
        $lf = @(Get-ChildItem $licSrc -File)
        foreach ($f in $lf) { Copy-Item $f.FullName (Join-Path $licDst $f.Name) -Force }
        Write-Log ("复制许可与署名文件：{0} 个 → licenses\" -f $lf.Count)
        $items += [pscustomobject]@{ Action = '许可'; File = 'licenses\'; Note = ('' + $lf.Count + ' 个文件') }
    }

    # 4) 运行时【不在这里处理】：统一由 Install-RuntimeComponents 负责（避免重复复制与模式冲突）


    # 5) 写配置
    $cfgs = @(
        @{ Name = 'xess_proxy.bform.txt';     Value = $Plan.Bform;     Note = if ($Plan.Bform -eq '9') { '4K 后置形态' } else { 'A 形态' } },
        @{ Name = 'xess_proxy.queuehook.txt'; Value = $Plan.QueueHook; Note = '队列钩子' },
        @{ Name = 'xess_proxy.gpu.txt';       Value = "nr=dev:$($Plan.NrDeviceId)"; Note = '神经卡选择' },
        @{ Name = 'xess_proxy.dumpshot.txt';  Value = '0 0';           Note = '关闭抓帧' }
    )
    foreach ($c in $cfgs) {
        Set-Content -Path (Join-Path $game $c.Name) -Value $c.Value -NoNewline -Encoding ASCII
        Write-Log ("写配置 {0} = {1}（{2}）" -f $c.Name, $c.Value, $c.Note)
        $items += [pscustomobject]@{ Action = '配置'; File = $c.Name; Note = $c.Value }
    }

    # 第三方组件声明（MIT 要求保留版权与许可声明）
    $lic = Join-Path $game 'dlssnr-amd\LICENSE-DLSSNR-AMD.txt'
    $noticePath = Join-Path $game 'AMD-CoGPU-第三方组件声明.txt'
    $notice = @()
    $notice += 'AMD CoGPU · 第三方组件声明'
    $notice += '============================================================'
    $notice += ''
    $notice += '本工具（libxess.dll 跨卡代理）由 AMD CoGPU 项目提供。'
    $notice += ''
    $notice += '本工具依赖但【不属于】AMD CoGPU 的第三方组件：'
    $notice += ''
    $notice += '1) 神经网络降噪运行时（LmxxfNrRuntime.dll / MochizukiNrRuntime.dll / LmxxfNrRuntime.pak）'
    $notice += '   与模型目录 dlssnr-amd\  ——  作者：mochizuki0323'
    $notice += '   许可：MIT License（Copyright (c) 2026 mochizuki0323）'
    $notice += '   · LmxxfNrRuntime.pak 内声明：Kien (lmxxf) 网络/权重/内核 = MIT；RDNA3 后端 3zwr1 (AMDNR) = MIT'
    $notice += '     该声明为已认证：被编辑或删除的 pak 会被运行时拒绝，所以本项目原样分发。'
    $notice += '   ★ 本项目【原样分发】上述第三方运行时内容，不做任何修改，并随包附上 MIT 许可原文。'
    $notice += '     本项目按 MIT 要求原样分发并附完整许可原文；安装器负责下载与 SHA256 校验。'
    if (Test-Path $lic) {
        $notice += '   许可原文随附于：dlssnr-amd\LICENSE-DLSSNR-AMD.txt（请勿删除）'
        $notice += ''
        $notice += '---- 许可原文 ----'
        $notice += (Get-Content $lic -Raw -Encoding UTF8)
    } else {
        $notice += '   ⚠ 未在目标目录找到许可原文文件，请自行向原作者索取并保留。'
    }
    $notice += ''
    $notice += '2) Intel XeSS 运行库（libxess_orig.dll）'
    $notice += '   归属：Intel Corporation（XeSS SDK）。该文件由游戏本体自带，'
    $notice += '   本安装器不复制、不分发它。'
    $notice += ''
    $notice += "生成时间：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Set-Content -Path $noticePath -Value ($notice -join "`r`n") -Encoding UTF8
    $items += [pscustomobject]@{ Action = '声明'; File = (Split-Path $noticePath -Leaf); Note = '第三方组件与 MIT 许可原文' }

    return $items
}

function New-UsageDoc {
    param([hashtable]$Plan, [string]$GameDir)
    $p = Join-Path $GameDir 'AMD-CoGPU-使用说明.txt'
    $t = @'
============================================================
  AMD CoGPU · 使用说明（游戏内）
============================================================

【热键】
  Del                打开 / 关闭 参数菜单
  F10                神经降噪（NR）开 / 关
  小键盘 1 / End     形态热切换（NumLock 需处于关闭状态）
                     · 没有小键盘的键盘：直接按 End，与"小键盘 1"是同一个键
                     · A 形态      ：NR 在 1440p 上做，再交给游戏放大出画
                     · B-4K 形态   ：A 卡真 XeSS 放大到 4K，B 卡在 4K 上做 NR

  其余参数（残差增益、门限、时域稳定等）全部在 Del 菜单里调，
  旧版的 F2~F8 / F11 单键已停用。

【启动前检查】
  1. 游戏目录里的 libxess.dll 必须是本工具装的（809,984 字节 ≈ 791 KB，定版 B4K-20261029c）
  2. 神经卡（默认 RX 9070 XT）要能被系统识别，且不要被其它程序独占
  3. 进游戏后直接按 F10 就会出 NR 画面（定版 c 起不再需要"先开一次菜单"）；
     日志里出现 "WND: 已定位游戏主窗口 …（不依赖菜单）" 就说明窗口已自动拿到

【怎么判断在工作】
  游戏目录下的 xess_proxy.log：
    · worker: 使用 <显卡名>                  ← 神经卡选对了
    · BFORM: 开关 -> 开 阶段=6               ← B 形态已启动
    · BFORM-P: 产出 … 均值 17ms 左右          ← 出画节奏正常

【本机安装信息】
'@
    $t += "  游戏目录 ：$GameDir`r`n"
    $t += "  游戏卡   ：$($Plan.GameGpuName)（DeviceId 0x$($Plan.GameDeviceId)）`r`n"
    $t += "  神经卡   ：$($Plan.NrGpuName)（DeviceId 0x$($Plan.NrDeviceId)）`r`n"
    $t += "  默认形态 ：$(if ($Plan.Bform -eq '9') { 'B-4K（4K 后置）' } else { 'A 形态' })`r`n"
    $t += "  安装时间 ：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')`r`n"
    $t += "  安装版本 ：$($script:Version)`r`n"
    $t += @'

【回退】
  安装时已把同名旧文件备份为 *.bak-setup-<时间戳>，
  把 libxess.dll 换回备份即可恢复原状。

============================================================
'@
    Set-Content -Path $p -Value $t -Encoding UTF8
    return $p
}

# ---------------------------------------------------------------- 自检模式

if ($SelfTest) {
    $ok = $true
    Write-Host "==== AMD CoGPU 安装程序自检 ===="
    Write-Host "[1] 安装包完整性"
    $dll = Join-Path $script:Payload 'libxess.dll'
    if (Test-Path $dll) {
        $i = Get-Item $dll
        Write-Host ("    libxess.dll  {0} B  {1}" -f $i.Length, $i.LastWriteTime)
    } else { Write-Host '    ✗ 缺少 payload\libxess.dll' -ForegroundColor Red; $ok = $false }
    foreach ($n in 'xess_proxy.saved.ini') {
        $p = Join-Path $script:Payload $n
        Write-Host ("    {0,-22} {1}" -f $n, $(if (Test-Path $p) { '存在' } else { '缺失（可选）' }))
    }
    $rt = Join-Path $script:Payload 'runtime'
    if (Test-Path $rt) {
        $f = Get-ChildItem $rt -Recurse -File
        Write-Host ("    runtime\              {0} 个文件 / {1:N1} MB" -f $f.Count, (($f | Measure-Object Length -Sum).Sum / 1MB))
    } else { Write-Host '    runtime\              未打包（仅装代理，适用于已装运行时的机器）' }

    Write-Host "[1b] 运行时清单（runtime-manifest.json）"
    $comps = Get-RuntimeComponents
    if ($comps.Count -eq 0) { Write-Host '    ✘ 清单里没有运行时组件' -ForegroundColor Red; $ok = $false }
    else {
        foreach ($c in $comps) {
            $u = if ($c.selfHosted -and $c.selfHosted.url) { $c.selfHosted.url } else { $c.url }
            Write-Host ("    [{0}] {1}  ←  {2}（{3}）" -f $c.id, $c.title, $c.author, $c.license)
            if ($u -like '*<*' -or -not $u) { Write-Host '          ⚠ 下载地址还是占位符（发布前必须填写）' -ForegroundColor Yellow }
            else { Write-Host ("          url   : {0}" -f $u) }
            if ($c.sha256) { Write-Host ("          sha256: {0}…" -f $c.sha256.Substring(0,16)) } else { Write-Host '          sha256: （未填，将跳过校验）' }
            if ($c.proxyRequires -and @($c.proxyRequires).Count -gt 0) { Write-Host ("          代理需要: {0}" -f ((@($c.proxyRequires)) -join ', ')) }
            if ($c.selfHosted -and $c.selfHosted.url) { Write-Host ("          自托管: {0}" -f $c.selfHosted.url) }
        }
    }
    Write-Host "[2] 显卡枚举"
    $gpus = Get-Gpus
    if (-not $gpus) { Write-Host '    ✗ 没枚举到显卡' -ForegroundColor Red; $ok = $false }
    foreach ($g in $gpus) {
        $tag = if (Test-Rdna4 $g.DeviceId) { '9000系' } elseif ($g.IsAmd) { 'AMD' } else { $g.Vendor }
        Write-Host ("    {0,-42} DeviceId=0x{1}  [{2}] {3} GB" -f $g.Name, $g.DeviceId, $tag, $g.VramGB)
    }

    Write-Host "[3] 游戏目录校验"
    $dir = if ($TestGameDir) { $TestGameDir } else { 'E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2' }
    $r = Test-GameDir $dir
    Write-Host ("    {0} → {1}（{2}）" -f $dir, $(if ($r.Ok) { '通过' } else { '不通过' }), $r.Msg)
    if (-not $r.Ok) { $ok = $false }

    Write-Host "[4] 干跑：配置生成 + 报告（写到临时目录）"
    $tmp = Join-Path $env:TEMP ('cogpu-selftest-' + (Get-Date -Format 'HHmmss'))
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    $plan = @{
        GameDir      = $tmp
        Bform        = $TestBform
        QueueHook    = '1'
    RuntimeMode  = 'auto'   # auto=已有就跳过，没有就从仓库下载 / download / skip
    RuntimeDone  = ''
    RuntimePct   = -1
        NrDeviceId   = $TestNrDeviceId
        GameGpuName  = 'SelfTest-Render'
        GameDeviceId = '744C'
        NrGpuName    = 'SelfTest-NR'
    }
    $items = Invoke-Install -Plan $plan
    $usage = New-UsageDoc -Plan $plan -GameDir $tmp
    Get-ChildItem $tmp -File | ForEach-Object { Write-Host ("    {0,-26} {1} B" -f $_.Name, $_.Length) }
    Write-Host ("    使用说明 → {0}" -f $usage)
    if (-not (Test-Path (Join-Path $tmp 'xess_proxy.bform.txt'))) { Write-Host '    ✗ 配置未生成' -ForegroundColor Red; $ok = $false }

    Write-Host ""
    if ($ok) { Write-Host '==== 自检通过 ====' -ForegroundColor Green } else { Write-Host '==== 自检失败 ====' -ForegroundColor Red }
    exit $(if ($ok) { 0 } else { 1 })
}

# ---------------------------------------------------------------- 界面

$script:LogLines = New-Object System.Collections.ArrayList
$script:LogBox   = $null
$script:State    = @{
    Page         = 0
    GameDir      = ''
    HasUpscale   = $null
    GameGpu      = $null
    NrGpu        = $null
    Bform        = '0'
    QueueHook    = '1'
    RuntimeMode  = 'auto'   # auto=已有就跳过，没有就从仓库下载 / download / skip
    RuntimeDone  = ''
    RuntimePct   = -1
    Installed    = $false
    ReportPath   = ''
    UsagePath    = ''
}
$script:Steps = @('用法', '游戏目录', '超分确认', '选择显卡', '安装设定', '运行时', '安装', '完成')

# ---------------------------------------------------------------- 无界面安装（测试 / 批量）
if ($InstallTo) {
    Write-Host "==== AMD CoGPU 无界面安装 ===="
    $ok = $true
    if (-not (Test-Path $InstallTo)) { New-Item -ItemType Directory -Path $InstallTo -Force | Out-Null }
    $chk = Test-GameDir $InstallTo
    Write-Host ("[1] 目标目录：{0} → {1}" -f $InstallTo, $chk.Msg)
    if (-not $chk.Ok) { Write-Host '    ⚠ 目录里没有 exe（测试用目录可忽略）' -ForegroundColor Yellow }

    $plan = @{
        GameDir      = $InstallTo
        Bform        = $Bform
        QueueHook    = '1'
        NrDeviceId   = $NrDev
        GameGpuName  = 'Headless-Render'
        GameDeviceId = '744C'
        NrGpuName    = 'Headless-NR'
    }
    Write-Host '[2] 安装代理与配置…'
    $items = Invoke-Install -Plan $plan

    Write-Host ("[3] 运行时（模式 {0}）…" -f $RuntimeMode)
    $rt = Install-RuntimeComponents -GameDir $InstallTo -Mode $RuntimeMode -OnProgress {
        param($pct, $got, $total)
        if ($pct -ge 0) { Write-Host ("    下载 {0}%  ({1}/{2} MB)" -f $pct, $got, $total) }
        else { Write-Host ("    下载 {0} MB" -f $got) }
    }
    Write-Host ("    结果：{0}" -f $rt.Done)

    Write-Host '[4] 生成使用说明…'
    $usage = New-UsageDoc -Plan $plan -GameDir $InstallTo
    Write-Host ("    {0}" -f (Split-Path $usage -Leaf))

    Write-Host '[4b] 生成安装报告…'
    $rep = New-InstallReport -Plan $plan -Items $items -RuntimeDone $rt.Done
    Write-Host ("    {0}" -f (Split-Path $rep -Leaf))

    Write-Host '[5] 校验产物：'
    foreach ($n in 'libxess.dll','xess_proxy.bform.txt','xess_proxy.queuehook.txt','xess_proxy.gpu.txt','xess_proxy.dumpshot.txt','LmxxfNrRuntime.dll','LmxxfNrRuntime.pak','dlssnr-amd') {
        $q = Join-Path $InstallTo $n
        $has = Test-Path $q
        $extra = if ($has -and (Get-Item $q).PSIsContainer -eq $false) { " {0:N1} KB" -f ((Get-Item $q).Length/1KB) } else { '' }
        Write-Host ("    {0,-26} {1}{2}" -f $n, $(if ($has) {'✔'} else {'—'}), $extra)
        if (-not $has -and $RuntimeMode -ne 'skip' -and $n -like 'Lmxxf*') { $ok = $false }
    }
    Write-Host ''
    if ($ok) { Write-Host '==== 无界面安装完成 ====' -ForegroundColor Green; exit 0 } else { Write-Host '==== 安装未完整 ====' -ForegroundColor Red; exit 1 }
}
$form = New-Object System.Windows.Forms.Form
$form.Text          = "$($script:AppName) 安装程序  ·  $($script:Version)"
$form.Size          = New-Object System.Drawing.Size(880, 620)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox   = $false
$form.BackColor     = [System.Drawing.Color]::FromArgb(24, 26, 30)
$form.ForeColor     = [System.Drawing.Color]::Gainsboro
$form.Font          = New-Object System.Drawing.Font('Microsoft YaHei UI', 9)

# 左侧步骤栏
$nav = New-Object System.Windows.Forms.Panel
$nav.Location = New-Object System.Drawing.Point(0, 0)
$nav.Size     = New-Object System.Drawing.Size(190, 570)
$nav.BackColor = [System.Drawing.Color]::FromArgb(16, 18, 22)
$form.Controls.Add($nav)
$navLabels = @()
for ($i = 0; $i -lt $script:Steps.Count; $i++) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = ("  {0}. {1}" -f ($i + 1), $script:Steps[$i])
    $l.Location = New-Object System.Drawing.Point(0, (20 + $i * 34))
    $l.Size = New-Object System.Drawing.Size(190, 28)
    $l.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 10)
    $l.ForeColor = [System.Drawing.Color]::Gray
    $nav.Controls.Add($l)
    $navLabels += $l
}

# 内容区
$body = New-Object System.Windows.Forms.Panel
$body.Location = New-Object System.Drawing.Point(190, 0)
$body.Size     = New-Object System.Drawing.Size(674, 520)
$body.Padding  = New-Object System.Windows.Forms.Padding(20, 16, 20, 10)
$form.Controls.Add($body)

# 底部按钮
$btnBack   = New-Object System.Windows.Forms.Button
$btnNext   = New-Object System.Windows.Forms.Button
$btnCancel = New-Object System.Windows.Forms.Button
$btnBack.Text = '上一步';   $btnBack.Location   = New-Object System.Drawing.Point(560, 540); $btnBack.Size = New-Object System.Drawing.Size(90, 32)
$btnNext.Text = '下一步';   $btnNext.Location   = New-Object System.Drawing.Point(660, 540); $btnNext.Size = New-Object System.Drawing.Size(90, 32)
$btnCancel.Text = '取消';   $btnCancel.Location = New-Object System.Drawing.Point(760, 540); $btnCancel.Size = New-Object System.Drawing.Size(80, 32)
foreach ($b in $btnBack, $btnNext, $btnCancel) {
    $b.FlatStyle = 'Flat'
    $b.BackColor = [System.Drawing.Color]::FromArgb(40, 44, 52)
    $b.ForeColor = [System.Drawing.Color]::WhiteSmoke
    $form.Controls.Add($b)
}

function Add-Title([string]$text, [string]$sub = '') {
    $t = New-Object System.Windows.Forms.Label
    $t.Text = $text
    $t.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 14, [System.Drawing.FontStyle]::Bold)
    $t.ForeColor = [System.Drawing.Color]::FromArgb(255, 138, 0)
    $t.Location = New-Object System.Drawing.Point(20, 14)
    $t.Size = New-Object System.Drawing.Size(630, 34)
    $body.Controls.Add($t)
    if ($sub) {
        $s = New-Object System.Windows.Forms.Label
        $s.Text = $sub
        $s.Location = New-Object System.Drawing.Point(22, 50)
        $s.Size = New-Object System.Drawing.Size(628, 40)
        $s.ForeColor = [System.Drawing.Color]::Silver
        $body.Controls.Add($s)
    }
}

function Add-Text([string]$text, [int]$x, [int]$y, [int]$w = 620, [int]$h = 22, [string]$color = 'Gainsboro', [switch]$Return) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text
    $l.Location = New-Object System.Drawing.Point($x, $y)
    $l.Size = New-Object System.Drawing.Size($w, $h)
    $l.ForeColor = [System.Drawing.Color]::FromName($color)
    $body.Controls.Add($l)
    if ($Return) { return $l }
}

function Show-Page {
    $S = $script:State   # 闭包捕获用：GetNewClosure 会让 $script: 失效，所以用局部引用
    $body.Controls.Clear()
    foreach ($l in $navLabels) { $l.ForeColor = [System.Drawing.Color]::Gray; $l.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 10) }
    if ($S.Page -lt $navLabels.Count) {
        $navLabels[$S.Page].ForeColor = [System.Drawing.Color]::FromArgb(255, 138, 0)
        $navLabels[$S.Page].Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 10, [System.Drawing.FontStyle]::Bold)
    }
    $btnBack.Enabled = ($S.Page -gt 0 -and $S.Page -lt 7)
    $btnNext.Text = if ($S.Page -eq 7) { '关闭' } elseif ($S.Page -eq 6) { '安装中…' } else { '下一步' }
    $btnNext.Enabled = $true

    switch ($S.Page) {

    0 {   # ---- 欢迎 / 游戏内用法
        Add-Title "欢迎使用 $($script:AppName)" "跨卡神经渲染 · 一张卡渲染、另一张卡实时降噪"
        Add-Text '这个工具装好后，游戏里只有三个热键需要记住：' 22 100 620 24 'White'
        $grid = New-Object System.Windows.Forms.DataGridView
        $grid.Location = New-Object System.Drawing.Point(22, 130)
        $grid.Size = New-Object System.Drawing.Size(628, 190)
        $grid.AllowUserToAddRows = $false
        $grid.RowHeadersVisible = $false
        $grid.ReadOnly = $true
        $grid.BackgroundColor = [System.Drawing.Color]::FromArgb(32, 35, 40)
        $grid.GridColor = [System.Drawing.Color]::FromArgb(44, 47, 53)
        $grid.CellBorderStyle = 'SingleHorizontal'
        $grid.SelectionMode = 'FullRowSelect'
        $grid.RowTemplate.Height = 30
        $grid.ColumnHeadersBorderStyle = 'Single'
        $grid.ColumnHeadersHeight = 30
        $grid.EnableHeadersVisualStyles = $false
        $grid.ColumnHeadersDefaultCellStyle.BackColor = [System.Drawing.Color]::FromArgb(45, 49, 56)
        $grid.ColumnHeadersDefaultCellStyle.ForeColor = [System.Drawing.Color]::WhiteSmoke
        $grid.DefaultCellStyle.BackColor = [System.Drawing.Color]::FromArgb(32, 35, 40)
        $grid.DefaultCellStyle.ForeColor = [System.Drawing.Color]::Gainsboro
        $grid.DefaultCellStyle.SelectionBackColor = [System.Drawing.Color]::FromArgb(60, 64, 70)
        $grid.DefaultCellStyle.SelectionForeColor = [System.Drawing.Color]::White
        $null = $grid.Columns.Add('k', '热键')
        $null = $grid.Columns.Add('f', '功能')
        $grid.Columns[0].Width = 205
        $grid.Columns[1].Width = 405
        $grid.ScrollBars = 'None'
        $grid.Rows.Add('Del', '打开 / 关闭 参数菜单（所有可调参数都在里面）') | Out-Null
        $grid.Rows.Add('F10', '神经降噪（NR）开 / 关') | Out-Null
        $grid.Rows.Add('小键盘 1 / End', '形态热切换：A 形态 ↔ B-4K 形态（NumLock 关；无小键盘者按 End）') | Out-Null
        $grid.ClearSelection()
        $grid.CurrentCell = $null
        $body.Controls.Add($grid)
        Add-Text '· A 形态：NR 在 1440p 上做，再交给游戏放大出画（更省算力，画质略佳）' 22 330 620 22
        Add-Text '· B-4K 形态：A 卡放大到 4K 后再做 NR（算力更大；UI 在超分后才覆盖的游戏请选 A）' 22 354 620 22
        Add-Text '提示：旧版的 F2~F8 / F11 单键已停用，那些参数请进 Del 菜单调。' 22 380 620 24 'Goldenrod'
        # ---- 卸载区（装错了 / 想恢复原版时用，不必另开脚本）
        Add-Text '──────────────────────────────────────────────' 22 406 626 18 'DimGray'
        Add-Text '已经装过了？需要卸载 / 恢复原版 XeSS' 22 428 626 22 'White'
        $chkKeepRt = New-Object System.Windows.Forms.CheckBox
        $chkKeepRt.Text = '同时删除运行时（LmxxfNrRuntime.pak 等，约 915MB）'
        $chkKeepRt.Location = New-Object System.Drawing.Point(26, 452)
        $chkKeepRt.Size = New-Object System.Drawing.Size(600, 24)
        $chkKeepRt.Checked = $true
        $body.Controls.Add($chkKeepRt)
        $btnUn = New-Object System.Windows.Forms.Button
        $btnUn.Text = '选择游戏目录并卸载…'
        $btnUn.Location = New-Object System.Drawing.Point(22, 480)
        $btnUn.Size = New-Object System.Drawing.Size(190, 30)
        $btnUn.FlatStyle = 'Flat'
        $btnUn.BackColor = [System.Drawing.Color]::FromArgb(70, 44, 44)
        $btnUn.ForeColor = [System.Drawing.Color]::WhiteSmoke
        $body.Controls.Add($btnUn)
        $p0 = @{ btn = $btnUn; keep = $chkKeepRt }
        $p0.go = {
            param($c)
            $fbd = New-Object System.Windows.Forms.FolderBrowserDialog
            $fbd.Description = '选择要卸载的游戏根目录'
            if ($fbd.ShowDialog() -ne 'OK') { return }
            $dir = $fbd.SelectedPath
            $act = if ($c.keep.Checked) { '删除代理 + 配置 + 运行时' } else { '删除代理 + 配置（保留运行时）' }
            $ans = [System.Windows.Forms.MessageBox]::Show(("即将卸载：`n  $dir`n`n将{0}，并把原版 XeSS 恢复为 libxess.dll。`n`n继续吗？" -f $act), '确认卸载', 'YesNo', 'Warning')
            if ($ans -ne 'Yes') { return }
            try {
                $log = Invoke-Uninstall -GameDir $dir -KeepRuntime:(-not $c.keep.Checked)
                $head = ($log | Select-Object -First 12) -join "`n"
                [System.Windows.Forms.MessageBox]::Show(("卸载完成（共 {0} 项）：`n`n{1}" -f $log.Count, $head), '卸载完成', 'OK', 'Information') | Out-Null
            } catch {
                [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, '卸载失败', 'OK', 'Error') | Out-Null
            }
        }
        $btnUn.Add_Click({ & $p0.go $p0 }.GetNewClosure())
        Add-Text '说明：卸载只删除我们安装的文件，并把原版 XeSS 改名回来；游戏本体文件不会被动。' 22 514 626 18 'Gray'
    }

    1 {   # ---- 选择游戏目录
        Add-Title '第 2 步 · 选择游戏目录' '请选到游戏的【根目录】（里面能看到游戏 exe 的那一层）'
        $tb = New-Object System.Windows.Forms.TextBox
        $tb.Location = New-Object System.Drawing.Point(22, 110)
        $tb.Size = New-Object System.Drawing.Size(520, 26)
        $tb.Text = $S.GameDir
        $tb.BackColor = [System.Drawing.Color]::FromArgb(38, 41, 47)
        $tb.ForeColor = [System.Drawing.Color]::WhiteSmoke
        $tb.BorderStyle = 'FixedSingle'
        $body.Controls.Add($tb)
        $browse = New-Object System.Windows.Forms.Button
        $browse.Text = '浏览…'
        $browse.Location = New-Object System.Drawing.Point(552, 108)
        $browse.Size = New-Object System.Drawing.Size(96, 28)
        $browse.FlatStyle = 'Flat'
        $browse.BackColor = [System.Drawing.Color]::FromArgb(45, 49, 56)
        $browse.ForeColor = [System.Drawing.Color]::WhiteSmoke
        $body.Controls.Add($browse)
        $msg = Add-Text '' 22 148 626 46 'Silver' -Return   # 校验结果标签（必须在这里创建）
        $msg.Name = 'dirMsg'
        $p2 = @{ tb = $tb; msg = $msg; btn = $btnNext }
        $p2.run = {
            param($ctx)
            $r = Test-GameDir $ctx.tb.Text
            $ctx.msg.Text = if ($r.Ok) { "✔ $($r.Msg)" } else { "✘ $($r.Msg)" }
            $ctx.msg.ForeColor = if ($r.Ok) { [System.Drawing.Color]::LightGreen } else { [System.Drawing.Color]::OrangeRed }
            $ctx.btn.Enabled = $r.Ok
            $global:CoGpuDirOk = $r.Ok
            $global:CoGpuGameDir = $ctx.tb.Text
        }
        $p2.tb.Add_TextChanged({ & $p2.run $p2 }.GetNewClosure())
        $browse.Add_Click({
            $fbd = New-Object System.Windows.Forms.FolderBrowserDialog
            $fbd.Description = '选择游戏根目录'
            if ($fbd.ShowDialog() -eq 'OK') { $p2.tb.Text = $fbd.SelectedPath; & $p2.run $p2 }
        }.GetNewClosure())
        & $p2.run $p2
        $S.GameDir = $p2.tb.Text
        Add-Text '为什么需要它：代理 DLL 必须放在游戏根目录，才能被游戏的加载器找到。' 22 200 626 24 'Gray'
    }

    2 {   # ---- 超分功能确认
        Add-Title '第 3 步 · 这个游戏有超分功能吗？' '跨卡管线依赖游戏的超分（XeSS / DLSS / FSR），否则无法安装'
        Add-Text '请确认：游戏设置里能开启 XeSS（或 DLSS / FSR 的超分项）。' 22 108 626 24 'White'
        $rYes = New-Object System.Windows.Forms.RadioButton
        $rYes.Text = '有（我能在游戏里开启超分）'
        $rYes.Location = New-Object System.Drawing.Point(26, 148)
        $rYes.Size = New-Object System.Drawing.Size(320, 26)
        $rNo = New-Object System.Windows.Forms.RadioButton
        $rNo.Text = '没有 / 不确定'
        $rNo.Location = New-Object System.Drawing.Point(26, 182)
        $rNo.Size = New-Object System.Drawing.Size(320, 26)
        $body.Controls.Add($rYes); $body.Controls.Add($rNo)
        $hint = Add-Text '' 22 224 626 60 'Silver' -Return
        $rYes.Add_CheckedChanged({
            $S.HasUpscale = $true
            $hint.Text = '很好，可以继续。安装完成后，游戏内的上采样请选 XeSS。'
            $hint.ForeColor = [System.Drawing.Color]::LightGreen
            $btnNext.Enabled = $true
        }.GetNewClosure())
        $rNo.Add_CheckedChanged({
            $S.HasUpscale = $false
            $hint.Text = "如果游戏没有超分功能，本工具无法安装：`r`n我们的管线是「游戏超分放大 → 第二张卡降噪」，缺少超分这一步就没有可用的底图。"
            $hint.ForeColor = [System.Drawing.Color]::OrangeRed
            $btnNext.Enabled = $false
        }.GetNewClosure())
        if ($S.HasUpscale -eq $true) { $rYes.Checked = $true }
        elseif ($S.HasUpscale -eq $false) { $rNo.Checked = $true }
        else { $btnNext.Enabled = $false }
    }

    3 {   # ---- 选择显卡
        Add-Title '第 4 步 · 选择游戏卡与神经卡' '建议：神经卡用 9000 系（RX 9070 / 9070 XT），性能与显存都更合适'
        $gpus = Get-Gpus
        if (-not $gpus) {
            Add-Text '✘ 没有枚举到显卡，无法继续。' 22 110 620 24 'OrangeRed'
            $btnNext.Enabled = $false
            break
        }
        Add-Text '游戏卡（跑游戏渲染的那张）' 22 104 300 22 'White'
        $cbGame = New-Object System.Windows.Forms.ComboBox
        $cbGame.Location = New-Object System.Drawing.Point(22, 128); $cbGame.Size = New-Object System.Drawing.Size(626, 26)
        $cbGame.DropDownStyle = 'DropDownList'
        Add-Text '神经卡（跑神经网络降噪的那张）' 22 172 300 22 'White'
        $cbNr = New-Object System.Windows.Forms.ComboBox
        $cbNr.Location = New-Object System.Drawing.Point(22, 196); $cbNr.Size = New-Object System.Drawing.Size(626, 26)
        $cbNr.DropDownStyle = 'DropDownList'
        foreach ($g in $gpus) {
            $label = "{0}   [DeviceId 0x{1}]  {2} GB  {3}" -f $g.Name, $g.DeviceId, $g.VramGB, $(if (Test-Rdna4 $g.DeviceId) { '★9000系' } elseif ($g.IsAmd) { 'AMD' } else { $g.Vendor })
            $cbGame.Items.Add($label) | Out-Null
            $cbNr.Items.Add($label)   | Out-Null
        }
        $body.Controls.Add($cbGame); $body.Controls.Add($cbNr)
        $warn = Add-Text '' 22 236 626 84 'Goldenrod' -Return
        $sel = {
            $gi = $cbGame.SelectedIndex; $ni = $cbNr.SelectedIndex
            $S.GameGpu = if ($gi -ge 0) { $gpus[$gi] } else { $null }
            $S.NrGpu   = if ($ni -ge 0) { $gpus[$ni] } else { $null }
            $msgs = @()
            if ($S.NrGpu) {
                if (-not $S.NrGpu.IsAmd) { $msgs += '⚠ 神经卡不是 AMD 显卡：本工具只支持 AMD 双卡。' }
                elseif (-not (Test-Rdna4 $S.NrGpu.DeviceId)) { $msgs += '⚠ 强烈建议神经卡使用 9000 系（RDNA4）显卡：老卡跑神经网络会更慢，可能达不到 60fps。' }
            }
            if ($S.GameGpu -and $S.NrGpu -and $gi -eq $ni) { $msgs += '⚠ 游戏卡与神经卡不能是同一张。' }
            if (-not $S.GameGpu) { $msgs += '· 请选择游戏卡' }
            if (-not $S.NrGpu)   { $msgs += '· 请选择神经卡' }
            $warn.Text = ($msgs -join "`r`n")
            $warn.ForeColor = if ($msgs -and ($msgs[0] -like '*⚠*')) { [System.Drawing.Color]::Goldenrod } else { [System.Drawing.Color]::Silver }
            $btnNext.Enabled = ($S.GameGpu -and $S.NrGpu -and $gi -ne $ni)
        }
        $cbGame.Add_SelectedIndexChanged($sel.GetNewClosure())
        $cbNr.Add_SelectedIndexChanged($sel.GetNewClosure())
        if ($gpus.Count -ge 2) { $cbGame.SelectedIndex = 0; $cbNr.SelectedIndex = 1 }
        elseif ($gpus.Count -eq 1) { $cbGame.SelectedIndex = 0; $cbNr.SelectedIndex = 0 }
        & $sel
        Add-Text '说明：游戏卡由游戏自己决定使用，这里记录用于报告；神经卡会写入配置，代理启动时按 DeviceId 精确选择。' 22 330 626 44 'Gray'
        Add-Text '每帧要过卡的数据（B-4K 形态）' 22 392 626 22 'White'
        Add-Text '  4K 彩色 33.2 MB ＋ 运动矢量 8.3 MB ≈ 41.5 MB/帧，60fps 下约 2.5 GB/s' 22 414 626 22 'Silver'
        Add-Text '为什么建议神经卡用 9000 系' 22 446 626 22 'White'
        Add-Text '  · 神经网络推理吃算力和显存带宽，9000 系（RDNA4）跑 4K 降噪才够快' 22 468 626 22 'Silver'
    }

    4 {   # ---- 安装设定
        Add-Title '第 5 步 · 安装设定'
        Add-Text '默认形态' 22 104 200 22 'White'
        $rbB = New-Object System.Windows.Forms.RadioButton
        $rbB.Text = 'B-4K 形态：A 卡真 XeSS 放大到 4K，B 卡在 4K 上做 NR（算力消耗更大）'
        $rbB.Location = New-Object System.Drawing.Point(26, 128); $rbB.Size = New-Object System.Drawing.Size(620, 24)
        $rbA = New-Object System.Windows.Forms.RadioButton
        $rbA.Text = 'A 形态：NR 在 1440p 上做，再交给游戏放大出画（更省算力，画质略佳）'
        $rbA.Location = New-Object System.Drawing.Point(26, 158); $rbA.Size = New-Object System.Drawing.Size(620, 24)
        $rbB.Add_CheckedChanged(({ if ($rbB.Checked) { $S.Bform = '9' } }).GetNewClosure())
        $rbA.Add_CheckedChanged(({ if ($rbA.Checked) { $S.Bform = '0' } }).GetNewClosure())
        $body.Controls.Add($rbB); $body.Controls.Add($rbA)
        if ($S.Bform -eq '9') { $rbB.Checked = $true } else { $rbA.Checked = $true }



        $chkQ = New-Object System.Windows.Forms.CheckBox
        $chkQ.Text = '启用队列钩子（把 33.2MB 跨卡拷贝移出游戏队列 —— 强烈建议开启，关闭会掉到 45fps）'
        $chkQ.Location = New-Object System.Drawing.Point(26, 200); $chkQ.Size = New-Object System.Drawing.Size(620, 24)
        $chkQ.Add_CheckedChanged(({ $S.QueueHook = if ($chkQ.Checked) { '1' } else { '0' } }).GetNewClosure())
        $chkQ.Checked = ($S.QueueHook -eq '1')

        $body.Controls.Add($chkQ)

        Add-Text '安装时会做的事：' 22 246 300 22 'White'
        Add-Text '  1) 备份游戏目录里同名的旧文件（*.bak-setup-<时间戳>）' 22 272 620 22
        Add-Text '  2) 复制 libxess.dll（跨卡代理）与参数基线' 22 294 620 22
        Add-Text '  3) 写入 bform / queuehook / 神经卡 / 抓帧开关 四个配置文件' 22 316 620 22
        Add-Text '  4) 生成「AMD-CoGPU-使用说明.txt」与「安装报告」' 22 338 620 22
        Add-Text '注意：安装前请先关闭游戏，否则 libxess.dll 会被占用导致复制失败。' 22 380 620 24 'OrangeRed'
        Add-Text '⚠ B-4K 的限制：它的 NR 做在「A 卡超分之后的 4K 画面」上。如果游戏是在超分完成之后' 22 406 626 22 'OrangeRed'
        Add-Text '   才把 UI（准星 / HUD / 字幕 / 菜单）覆盖上去，这些 UI 也会被一起送进 NR → 文字发虚、' 22 428 626 22 'OrangeRed'
        Add-Text '   细线糊边、边缘抖动。这类游戏请选 A 形态（UI 由游戏在放大之后叠加，不受 NR 影响）。' 22 450 626 22 'OrangeRed'
    }

    5 {   # ---- 运行时（别人的做法：用时从仓库下载）
        Add-Title '第 6 步 · 运行时（神经网络降噪后端）' '运行时约 900MB，不放进安装包，安装时从仓库下载'
        $present = Test-RuntimePresent $S.GameDir
        $comps = Get-RuntimeComponents
        $info = Add-Text '' 22 104 626 44 'Silver' -Return
        if ($present) {
            $info.Text = '✔ 目标目录里已经检测到运行时，可以跳过下载。'
            $info.ForeColor = [System.Drawing.Color]::LightGreen
        } elseif ($comps.Count -eq 0) {
            $info.Text = '✘ 清单里没有运行时组件，无法下载运行时。'
            $info.ForeColor = [System.Drawing.Color]::OrangeRed
        } elseif (-not (($comps[0].selfHosted -and $comps[0].selfHosted.url) -or $comps[0].url)) {
            $info.Text = '✘ 清单里还没填可用的下载地址（selfHosted.url 或 url 都是空的）。'
            $info.ForeColor = [System.Drawing.Color]::OrangeRed
        } else {
            if ($comps[0].selfHosted -and $comps[0].selfHosted.url) {
                $info.Text = ("将下载作者自托管的运行时包（含 lmxxf 形态外壳，已验证可用）：`r`n   " + $comps[0].selfHosted.url)
            } else {
                $info.Text = ("将逐个从作者上游下载（注意：上游包不含我们代理需要的外壳文件，可能报不兼容）：`r`n   " + (($comps | ForEach-Object { "$($_.title) ← $($_.author)" }) -join "`r`n   "))
            }
        }
        $rAuto = New-Object System.Windows.Forms.RadioButton
        $rAuto.Text = '自动（推荐）：已有就跳过，没有就下载'
        $rAuto.Location = New-Object System.Drawing.Point(26, 158); $rAuto.Size = New-Object System.Drawing.Size(600, 24)
        $rDl = New-Object System.Windows.Forms.RadioButton
        $rDl.Text = '强制下载并覆盖运行时'
        $rDl.Location = New-Object System.Drawing.Point(26, 186); $rDl.Size = New-Object System.Drawing.Size(600, 24)
        $rSkip = New-Object System.Windows.Forms.RadioButton
        $rSkip.Text = '跳过（我已经装过运行时）'
        $rSkip.Location = New-Object System.Drawing.Point(26, 214); $rSkip.Size = New-Object System.Drawing.Size(600, 24)
        $body.Controls.Add($rAuto); $body.Controls.Add($rDl); $body.Controls.Add($rSkip)
        switch ($S.RuntimeMode) {
            'download' { $rDl.Checked = $true }
            'skip'     { $rSkip.Checked = $true }
            default    { $rAuto.Checked = $true }
        }
        # ★ P2 修复：按钮状态与单选联动（以前只在整页重渲染时求值一次，页内切"跳过"按钮仍是灰的）
        $localRtPath = Join-Path $script:Payload 'runtime'
        $p6 = @{
            btn      = $btnNext
            skip     = $rSkip
            hasLocal = (Test-Path $localRtPath)
            hasSelf  = ($comps.Count -gt 0 -and $comps[0].selfHosted -and $comps[0].selfHosted.url -and $comps[0].selfHosted.url -notlike '*<*')
            hasUp    = ($comps.Count -gt 0 -and $comps[0].url -and $comps[0].url -notlike '*<*')
        }
        $p6.upd = { param($c) $c.btn.Enabled = ($c.skip.Checked -or $c.hasLocal -or $c.hasSelf -or $c.hasUp) }
        $rAuto.Add_CheckedChanged(({ if ($rAuto.Checked) { $S.RuntimeMode = 'auto'; & $p6.upd $p6 } }).GetNewClosure())
        $rDl.Add_CheckedChanged(({   if ($rDl.Checked)   { $S.RuntimeMode = 'download'; & $p6.upd $p6 } }).GetNewClosure())
        $rSkip.Add_CheckedChanged(({ if ($rSkip.Checked) { $S.RuntimeMode = 'skip'; & $p6.upd $p6 } }).GetNewClosure())
        & $p6.upd $p6


        Add-Text '运行时包含：mochizuki / lmxxf 运行时 DLL、模型 pak（约 486MB）与 dlssnr-amd 模型目录' 22 254 626 22 'Gray'
        Add-Text '下载后会校验 SHA256 再解压；校验不通过会中止，不会污染游戏目录。' 22 278 626 22 'Gray'
        Add-Text '⚠ 没有运行时就无法做神经网络降噪：跳过它只适合"这台机器以前装过"的情况。' 22 310 626 22 'Goldenrod'
        Add-Text '关于运行时：属第三方（Kien/lmxxf 网络与权重、3zwr1/AMDNR 后端、mochizuki 模型，均 MIT）；本项目原样分发并附许可原文。' 22 342 626 22 'Silver'
        Add-Text '  dlssnr-amd\LICENSE-DLSSNR-AMD.txt 会随运行时一起安装到游戏目录。' 22 364 626 22 'Gray'
        Add-Text '下载内容与体积' 22 396 626 22 'White'
        Add-Text '  LmxxfNrRuntime.pak 约 486MB · dlssnr-amd 模型目录 约 395MB · 运行时 DLL 约 3MB' 22 418 626 22 'Silver'
        Add-Text '下载很慢怎么办' 22 450 626 22 'White'
        Add-Text '  可先跳过本页，手动把 runtime.zip 解压到游戏根目录，再重跑安装（选"自动"会自动跳过）。' 22 472 626 22 'Silver'

        # （旧的一次性求值已由上面的 $p6.upd 取代）
    }

    6 {   # ---- 安装
        Add-Title '第 7 步 · 正在安装'
        $script:LogBox = New-Object System.Windows.Forms.TextBox
        $script:LogBox.Multiline = $true
        $script:LogBox.ScrollBars = 'Vertical'
        $script:LogBox.ReadOnly = $true
        $script:LogBox.Location = New-Object System.Drawing.Point(22, 100)
        $script:LogBox.Size = New-Object System.Drawing.Size(626, 340)
        $script:LogBox.BackColor = [System.Drawing.Color]::FromArgb(18, 20, 24)
        $script:LogBox.ForeColor = [System.Drawing.Color]::LightGray
        $script:LogBox.Font = New-Object System.Drawing.Font('Consolas', 9)
        $body.Controls.Add($script:LogBox)
        $btnNext.Enabled = $false

        $plan = @{
            GameDir      = $S.GameDir
            Bform        = $S.Bform
            QueueHook    = $S.QueueHook
            NrDeviceId   = $S.NrGpu.DeviceId
            GameGpuName  = $S.GameGpu.Name
            GameDeviceId = $S.GameGpu.DeviceId
            NrGpuName    = $S.NrGpu.Name
        }
        try {
            $btnNext.Text = '安装中…'; $btnNext.Enabled = $false; $btnBack.Enabled = $false; $btnCancel.Enabled = $false
            Write-Log "开始安装到 $($plan.GameDir)"
            if (Get-Process -Name 're2' -ErrorAction SilentlyContinue) { throw '检测到游戏正在运行，请先关闭游戏再安装' }
            $items = Invoke-Install -Plan $plan

            # ---- 运行时：逐个组件（已有就跳过；没有就按清单来源下载）----
            $comps = Get-RuntimeComponents
            $localRt = Join-Path $script:Payload 'runtime'
            $doneList = @()
            if ($S.RuntimeMode -eq 'skip') {
                Write-Log '按用户选择：跳过运行时'
                $S.RuntimeDone = '跳过'
                $items += [pscustomobject]@{ Action = '运行时'; File = '(跳过)'; Note = '用户选择跳过' }
            } elseif ($comps.Count -eq 0) {
                Write-Log '清单里没有运行时组件，跳过' 'WARN'
                $S.RuntimeDone = '清单为空'
            } else {
                foreach ($c in $comps) {
                    $present = Test-ComponentPresent -Comp $c -Dir $plan.GameDir
                    if ($present -and $S.RuntimeMode -ne 'download') {
                        Write-Log ("组件 {0}：目标目录已有 → 跳过" -f $c.id)
                        $doneList += ("{0}=已有" -f $c.id)
                        $items += [pscustomobject]@{ Action = '运行时'; File = $c.id; Note = '目标已有，跳过' }
                        continue
                    }
                    if ((Test-Path $localRt) -and $S.RuntimeMode -ne 'download') {
                        Write-Log '安装包内自带 payload\runtime → 用本地副本（离线安装）'
                        $rf = Get-ChildItem $localRt -Recurse -File
                        foreach ($f in $rf) {
                            $rel = $f.FullName.Substring($localRt.Length).TrimStart('\')
                            $dst = Join-Path $plan.GameDir $rel
                            $dd = Split-Path $dst -Parent
                            if (-not (Test-Path $dd)) { New-Item -ItemType Directory -Path $dd -Force | Out-Null }
                            Copy-Item $f.FullName $dst -Force
                        }
                        Write-Log ("本地运行时复制完成：{0} 个文件" -f $rf.Count) 'OK'
                        $doneList += ("{0}=本地副本" -f $c.id)
                        $items += [pscustomobject]@{ Action = '运行时'; File = $c.id; Note = ('本地副本 ' + $rf.Count + ' 个文件') }
                        continue
                    }
                    Install-Component -Comp $c -GameDir $plan.GameDir -OnProgress {
                        param($pct, $got, $total)
                        if ($pct -ge 0) { Write-Log ("下载中 {0}%  ({1}/{2} MB)" -f $pct, $got, $total) }
                        else { Write-Log ("下载中 {0} MB" -f $got) }
                    }
                    $doneList += ("{0}=下载({1})" -f $c.id, $c.author)
                    $items += [pscustomobject]@{ Action = '运行时'; File = $c.id; Note = ('从 ' + $c.author + ' 上游下载') }
                }
                $S.RuntimeDone = ($doneList -join '; ')
            }
            $S.UsagePath = New-UsageDoc -Plan $plan -GameDir $plan.GameDir
            Write-Log "已生成使用说明：$(Split-Path $S.UsagePath -Leaf)"

            $rep = Join-Path $plan.GameDir ("AMD-CoGPU-安装报告-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
            $rt = @()
            $rt += "AMD CoGPU 安装报告"
            $rt += "版本      ：$($script:Version)"
            $rt += "时间      ：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
            $rt += "游戏目录  ：$($plan.GameDir)"
            $rt += "游戏卡    ：$($plan.GameGpuName)（DeviceId 0x$($plan.GameDeviceId)）"
            $rt += "神经卡    ：$($plan.NrGpuName)（DeviceId 0x$($plan.NrDeviceId)）"
            $rt += "默认形态  ：$(if ($plan.Bform -eq '9') { 'B-4K（4K 后置）' } else { 'A 形态' })"
            $rt += ("队列钩子  ：" + $(if ($plan.QueueHook -eq '1') { '开' } else { '关' }))
            $rt += ("运行时    ：" + $S.RuntimeDone)
            $rt += "---- 动作明细 ----"
            foreach ($it in $items) { $rt += ("{0,-4} {1,-24} {2}" -f $it.Action, $it.File, $it.Note) }
            $rt += ""
            $rt += "---- 安装后文件校验 ----"
            foreach ($n in 'libxess.dll', 'xess_proxy.bform.txt', 'xess_proxy.queuehook.txt', 'xess_proxy.gpu.txt', 'xess_proxy.dumpshot.txt', 'xess_proxy.saved.ini') {
                $p = Join-Path $plan.GameDir $n
                if (Test-Path $p) {
                    $i = Get-Item $p
                    $h = if ($i.Length -gt 0) { (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0, 16) } else { '-' }
                    $rt += ("{0,-26} {1,10} B  SHA256前16 {2}" -f $n, $i.Length, $h)
                } else { $rt += ("{0,-26} 缺失" -f $n) }
            }
            Set-Content -Path $rep -Value ($rt -join "`r`n") -Encoding UTF8
            $S.ReportPath = $rep
            Write-Log "已生成安装报告：$(Split-Path $rep -Leaf)"
            $S.Installed = $true
            Write-Log '安装完成' 'OK'
            $btnNext.Text = '下一步（安装完成）'; $btnCancel.Enabled = $true
            $btnNext.Enabled = $true
        } catch {
            Write-Log $_.Exception.Message 'ERROR'
            $btnNext.Text = '下一步（看日志）'; $btnCancel.Enabled = $true
            $S.Installed = $false
            $btnNext.Enabled = $true
        }
    }

    7 {   # ---- 完成
        if ($S.Installed) {
            Add-Title '安装完成 ✔' '接下来进游戏：设置里把上采样选成 XeSS，然后直接按 F10 就会出 NR 画面（不用先开菜单）'
            Add-Text '已安装到：' 22 104 200 22 'White'
            Add-Text $S.GameDir 22 126 626 22 'LightGreen'
            Add-Text ('运行时      ：' + $S.RuntimeDone) 22 150 620 22 'Silver'
            Add-Text '请记住三个热键：' 22 178 300 22 'White'
            Add-Text '  Del            参数菜单' 22 202 620 22
            Add-Text '  F10            神经降噪 开 / 关' 22 224 620 22
            Add-Text '  小键盘 1 / End  A 形态 ↔ B-4K 形态（NumLock 关闭；无小键盘者按 End）' 22 246 626 22
            Add-Text '生成的文件：' 22 266 300 22 'White'
            Add-Text '  AMD-CoGPU-使用说明.txt' 22 288 620 22 'Silver'
            Add-Text ('  ' + (Split-Path $S.ReportPath -Leaf)) 22 310 620 22 'Silver'
            Add-Text '出问题看这里：游戏目录下 xess_proxy.log（搜 "worker: 使用" 确认神经卡选对）' 22 348 626 24 'Gray'
            $open = New-Object System.Windows.Forms.Button
            $open.Text = '打开游戏目录'
            $open.Location = New-Object System.Drawing.Point(22, 390)
            $open.Size = New-Object System.Drawing.Size(130, 30)
            $open.FlatStyle = 'Flat'
            $open.BackColor = [System.Drawing.Color]::FromArgb(45, 49, 56)
            $open.ForeColor = [System.Drawing.Color]::WhiteSmoke
            $open.Add_Click({ Start-Process explorer.exe $S.GameDir })
            $body.Controls.Add($open)
        } else {
            Add-Title '安装未完成' '请查看上一页的日志末尾，按提示处理后重试'
            Add-Text '常见原因：游戏正在运行（DLL 被占用）、目录无写权限、安装包缺少 payload\libxess.dll' 22 120 626 44 'OrangeRed'
            if ($script:LogLines.Count -gt 0) {
                Add-Text ('最后一条日志：' + $script:LogLines[$script:LogLines.Count - 1]) 22 180 626 24 'Silver'
            }
        }
    }
    }
}

$back = {
    if ($script:State.Page -gt 0) { $script:State.Page--; if ($script:State.Page -eq 6) { $script:LogBox = $null }; Show-Page }
}
$next = {
    $p = $script:State.Page
    if ($p -eq 7) { $form.Close(); return }
    if ($p -eq 1) { $dd = if ($global:CoGpuGameDir) { $global:CoGpuGameDir } else { $script:State.GameDir }; $script:State.GameDir = $dd; $r = Test-GameDir $dd; if (-not $r.Ok) { [System.Windows.Forms.MessageBox]::Show($r.Msg, '目录无效'); return } }
    if ($p -eq 2 -and $script:State.HasUpscale -ne $true) {
        [System.Windows.Forms.MessageBox]::Show("这个游戏没有超分功能，无法安装本工具。`r`n`r`n我们的管线依赖『游戏超分放大后的底图』，没有超分就没有可降噪的画面源。", '无法安装')
        $form.Close(); return
    }
    if ($p -eq 3) {
        if (-not $script:State.GameGpu -or -not $script:State.NrGpu) { [System.Windows.Forms.MessageBox]::Show('请把两张卡都选上', '提示'); return }
        if ($script:State.GameGpu.DeviceId -eq $script:State.NrGpu.DeviceId) { [System.Windows.Forms.MessageBox]::Show('游戏卡与神经卡不能是同一张', '提示'); return }
        if (-not (Test-Rdna4 $script:State.NrGpu.DeviceId)) {
            $r = [System.Windows.Forms.MessageBox]::Show("强烈建议神经卡使用 9000 系显卡。`r`n当前选择：$($script:State.NrGpu.Name)（DeviceId 0x$($script:State.NrGpu.DeviceId)）`r`n`r`n仍要继续吗？", '建议更换神经卡', 'YesNo', 'Warning')
            if ($r -ne 'Yes') { return }
        }
    }
    $script:State.Page++
    Show-Page
}
$btnBack.Add_Click($back)
$btnNext.Add_Click($next)
$btnCancel.Add_Click({ $form.Close() })

if ($StartPage -ge 0 -and $StartPage -le 7) { $script:State.Page = $StartPage }

if ($UiSmokeTest) {
    Write-Host '==== 界面冒烟测试：逐页构建 + 戳控件 ===='
    $fail = 0
    $sandbox = Join-Path $env:TEMP ('cogpu-smoke-' + [guid]::NewGuid().ToString('N').Substring(0,8))
    New-Item -ItemType Directory -Path $sandbox -Force | Out-Null
    Set-Content (Join-Path $sandbox 're2.exe') 'stub' -Encoding ASCII
    $script:State.GameDir = $sandbox   # ★ 用沙箱目录，绝不碰真实游戏目录
    $names = @('用法','游戏目录','超分确认','选择显卡','安装设定','运行时','安装','完成')
    foreach ($pg in 0..7) {
        if ($pg -eq 6) { Write-Host '  -- 页6（安装）有落盘副作用，冒烟测试跳过 --' -ForegroundColor DarkGray; continue }
        $script:State.Page = $pg   # ★ 关键：不设置页号，Show-Page 永远渲染第 0 页
        try {
            Show-Page
            $poked = 0
            foreach ($c in $body.Controls) {
                try {
                    if ($c -is [System.Windows.Forms.TextBox]) { $c.Text = $script:State.GameDir; $poked++ }
                    elseif ($c -is [System.Windows.Forms.RadioButton]) { $c.Checked = $true; $c.Checked = $false; $poked++ }
                    elseif ($c -is [System.Windows.Forms.CheckBox]) { $c.Checked = -not $c.Checked; $c.Checked = -not $c.Checked; $poked++ }
                    elseif ($c -is [System.Windows.Forms.ComboBox] -and $c.Items.Count -gt 0) { $c.SelectedIndex = 0; $poked++ }
                } catch {
                    Write-Host ("  X 页{0} 控件 {1} 触发异常：{2}" -f $pg, $c.GetType().Name, $_.Exception.Message) -ForegroundColor Red
                    $fail++
                }
            }
            $types = ($body.Controls | ForEach-Object { $_.GetType().Name }) -join ','
            # ★ P2 断言：第 6 页三个单选切换后，"下一步"按钮状态必须跟着变
            # 安装页（pg=6）被跳过，这里单独断言：底栏文案逻辑里不该再出现"开始安装"
            if ($pg -eq 7) {
                $allText = (Get-Content $PSCommandPath -Raw -Encoding UTF8)
                if ($allText -match "\{ '开始安装' \}") { Write-Host '    X 仍存在"开始安装"按钮文案（安装页是自动执行的，不该要求再点）' -ForegroundColor Red; $fail++ }
                else { Write-Host '    按钮文案检查：无"开始安装"残留 OK' -ForegroundColor DarkGray }
            }
            if ($pg -eq 5) {
                $radios = @($body.Controls | Where-Object { $_ -is [System.Windows.Forms.RadioButton] })
                if ($radios.Count -eq 3) {
                    $radios[2].Checked = $true   # 选"跳过"
                    $skipOk = $btnNext.Enabled
                    $radios[1].Checked = $true   # 选"强制下载"
                    $dlOk = $btnNext.Enabled
                    Write-Host ("    P2: 选跳过→下一步={0}  选强制下载→下一步={1}" -f $skipOk, $dlOk) -ForegroundColor DarkGray
                    if (-not $skipOk) { Write-Host '    X P2 未修复：选"跳过"时"下一步"仍不可用' -ForegroundColor Red; $fail++ }
                } else { Write-Host ("    X 第6页单选数量异常：{0}" -f $radios.Count) -ForegroundColor Red; $fail++ }
            }
            Write-Host ("  OK 页{0}（{1}）控件: {2}" -f $pg, $names[$pg], $types) -ForegroundColor Green
        } catch {
            Write-Host ("  X 页{0}（{1}）构建失败：{2}" -f $pg, $names[$pg], $_.Exception.Message) -ForegroundColor Red
            $fail++
        }
    }
    Write-Host ''

    # ★ 装→卸 往返验证（沙箱）
    Write-Host '  -- 装→卸 往返验证（沙箱）--'
    try {
        $rt2 = Join-Path $sandbox 'rt'
        New-Item -ItemType Directory -Path $rt2 -Force | Out-Null
        Set-Content (Join-Path $sandbox 'LmxxfNrRuntime.pak') 'stub' -Encoding ASCII
        Set-Content (Join-Path $sandbox 'LmxxfNrRuntime.dll') 'stub' -Encoding ASCII
        New-Item -ItemType Directory -Path (Join-Path $sandbox 'dlssnr-amd') -Force | Out-Null
        Set-Content (Join-Path $sandbox 'dlssnr-amd\x.txt') 'stub' -Encoding ASCII
        Copy-Item (Join-Path $script:Payload 'libxess.dll') (Join-Path $sandbox 'libxess_orig.dll') -Force
        Copy-Item (Join-Path $script:Payload 'libxess.dll') (Join-Path $sandbox 'libxess.dll') -Force
        Copy-Item (Join-Path $script:Payload 'libxess.dll') (Join-Path $sandbox 'libxess.dll') -Force
        Set-Content (Join-Path $sandbox 'xess_proxy.bform.txt') '9' -NoNewline -Encoding ASCII
        $log = Invoke-Uninstall -GameDir $sandbox -KeepRuntime:(-not $true) -Quiet
        $left = @('xess_proxy.bform.txt','LmxxfNrRuntime.pak','dlssnr-amd','licenses') | Where-Object { Test-Path (Join-Path $sandbox $_) }
        $dllOk = (Test-Path (Join-Path $sandbox 'libxess.dll')) -and -not (Test-Path (Join-Path $sandbox 'libxess_orig.dll'))
        Write-Host ("     卸载 {0} 项；残留={1}；原版恢复={2}" -f $log.Count, $(if($left){$left -join ','}else{'无'}), $dllOk) -ForegroundColor DarkGray
        if ($left) { Write-Host '     X 卸载后仍有残留' -ForegroundColor Red; $fail++ }
        if (-not $dllOk) { Write-Host '     X 原版 libxess.dll 未正确恢复' -ForegroundColor Red; $fail++ }
    } catch { Write-Host ("     X 装卸往返异常：" + $_.Exception.Message) -ForegroundColor Red; $fail++ }

    if ($fail -eq 0) { Write-Host '==== 冒烟测试通过：8 页均可构建、事件处理无异常 ====' -ForegroundColor Green; exit 0 }
    else { Write-Host ("==== 冒烟测试失败：{0} 处异常 ====" -f $fail) -ForegroundColor Red; exit 1 }
}

Show-Page
[void]$form.ShowDialog()
