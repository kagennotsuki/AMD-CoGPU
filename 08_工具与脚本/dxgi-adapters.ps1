<#
  dxgi-adapters.ps1  —  以 OpenNR 安装器同样的方式枚举显卡，并模拟它的"挑卡"规则。

  用法： powershell -ExecutionPolicy Bypass -File E:\AI\dxgi-adapters.ps1
#>

$code = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class DxgiProbe
{
    [DllImport("dxgi.dll")]
    static extern int CreateDXGIFactory1(ref Guid riid, out IntPtr factory);
    static IntPtr Slot(IntPtr p, int i) { return Marshal.ReadIntPtr(Marshal.ReadIntPtr(p), i * IntPtr.Size); }
    delegate int EnumAdapters1Del(IntPtr self, uint index, out IntPtr adapter);
    delegate int GetDesc1Del(IntPtr self, IntPtr desc);
    delegate uint ReleaseDel(IntPtr self);

    public static List<string> Run()
    {
        var res = new List<string>();
        Guid iid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387");
        IntPtr factory;
        if (CreateDXGIFactory1(ref iid, out factory) < 0) { res.Add("ERR|CreateDXGIFactory1 失败"); return res; }
        var enumA = (EnumAdapters1Del)Marshal.GetDelegateForFunctionPointer(Slot(factory, 12), typeof(EnumAdapters1Del));
        for (uint i = 0; i < 16; i++)
        {
            IntPtr ad;
            if (enumA(factory, i, out ad) < 0) break;
            IntPtr buf = Marshal.AllocHGlobal(512);
            try
            {
                for (int k = 0; k < 512; k++) Marshal.WriteByte(buf, k, 0);
                var getDesc = (GetDesc1Del)Marshal.GetDelegateForFunctionPointer(Slot(ad, 10), typeof(GetDesc1Del));
                if (getDesc(ad, buf) < 0) continue;
                string name = Marshal.PtrToStringUni(buf, 128).TrimEnd('\0');
                uint vendor = (uint)Marshal.ReadInt32(buf, 256);
                uint device = (uint)Marshal.ReadInt32(buf, 260);
                uint subsys = (uint)Marshal.ReadInt32(buf, 264);
                ulong vram = (ulong)Marshal.ReadInt64(buf, 272);
                uint flags = (uint)Marshal.ReadInt32(buf, 304);
                res.Add(string.Format("{0}|{1}|{2}|{3}|{4}|{5}", name, vendor, device, subsys, vram, flags));
            }
            finally
            {
                Marshal.FreeHGlobal(buf);
                var rel = (ReleaseDel)Marshal.GetDelegateForFunctionPointer(Slot(ad, 2), typeof(ReleaseDel));
                rel(ad);
            }
        }
        return res;
    }
}
'@

if (-not ('DxgiProbe' -as [type])) { Add-Type -TypeDefinition $code -Language CSharp -ErrorAction Stop }

$rows = [DxgiProbe]::Run()
$rdna4Ids = @(30032, 30033, 30096, 30097)   # 0x7550 0x7551 0x7590 0x7591 —— 安装器内部的白名单

Write-Host "`n== DXGI 枚举到的适配器（安装器看到的就是这些）==" -ForegroundColor Cyan
$amd = @()
$i = 0
foreach ($r in $rows) {
    if ($r -like 'ERR|*') { Write-Host $r -ForegroundColor Red; return }
    $f = $r -split '\|'
    $name = $f[0]; $ven = [uint32]$f[1]; $dev = [uint32]$f[2]; $vram = [uint64]$f[4]; $flags = [uint32]$f[5]
    $soft = if (($flags -band 2) -ne 0) { "软件适配器(会被跳过)" } else { "" }
    Write-Host ("  [{0}] {1,-32} vendor=0x{2:X4} dev=0x{3:X4}  显存={4:N1} GB  {5}" -f $i, $name, $ven, $dev, ($vram / 1GB), $soft)
    if ($ven -eq 0x1002 -and ($flags -band 2) -eq 0) {
        $amd += [pscustomobject]@{ Name = $name; Device = $dev; Vram = $vram }
    }
    $i++
}

Write-Host "`n== 安装器的挑卡规则（反编译自 OpenNR-Installer 0.8.5 GpuService.Read）==" -ForegroundColor Cyan
Write-Host "  在 vendor=0x1002(AMD) 的非软件适配器里，取 DedicatedVideoMemory 最大的那张。"
if ($amd.Count -eq 0) { Write-Host "  没有找到 AMD 显卡。" -ForegroundColor Red; return }

$pick = $amd | Sort-Object Vram -Descending | Select-Object -First 1
Write-Host ("  => 会选中： {0}  ({1:N1} GB, dev=0x{2:X4})" -f $pick.Name, ($pick.Vram / 1GB), $pick.Device) -ForegroundColor Yellow

$isRdna4 = ($rdna4Ids -contains [int]$pick.Device) -or ($pick.Name -match '\bRX\s*9\d{3}') -or ($pick.Name -match '\bAI\s*PRO\s*R9\d{3}')
Write-Host ("  => RDNA4 判定（决定界面上的 lmxxf / mochizuki / WrongGPU 等 RDNA4 专属项）：{0}" -f $(if ($isRdna4) { "是" } else { "否" })) -ForegroundColor Yellow
Write-Host "     （判定条件：dev ∈ {0x7550,0x7551,0x7590,0x7591}，或名字匹配 RX 9xxx / AI PRO R9xx）"
Write-Host ""
