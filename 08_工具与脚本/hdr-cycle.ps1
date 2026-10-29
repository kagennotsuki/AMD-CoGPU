$src = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class HdrCycle
{
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint LowPart; public int HighPart; }

    [StructLayout(LayoutKind.Sequential)]
    public struct SOURCE_INFO { public LUID adapterId; public uint id; public uint modeInfoIdx; public uint statusFlags; }

    [StructLayout(LayoutKind.Sequential)]
    public struct RATIONAL { public uint Numerator; public uint Denominator; }

    [StructLayout(LayoutKind.Sequential)]
    public struct TARGET_INFO
    {
        public LUID adapterId; public uint id; public uint modeInfoIdx;
        public uint outputTechnology; public uint rotation; public uint scaling;
        public RATIONAL refreshRate; public uint scanLineOrdering;
        public int targetAvailable; public uint statusFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PATH_INFO { public SOURCE_INFO sourceInfo; public TARGET_INFO targetInfo; public uint flags; }

    [StructLayout(LayoutKind.Sequential)]
    public struct DEVICE_INFO_HEADER { public uint type; public uint size; public LUID adapterId; public uint id; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct TARGET_DEVICE_NAME
    {
        public DEVICE_INFO_HEADER header;
        public uint flags; public uint outputTechnology;
        public ushort edidManufactureId; public ushort edidProductCodeId; public uint connectorInstance;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string monitorFriendlyDeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string monitorDevicePath;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ADVANCED_COLOR_INFO { public DEVICE_INFO_HEADER header; public uint value; public uint colorEncoding; public uint bitsPerColorChannel; }

    [StructLayout(LayoutKind.Sequential)]
    public struct SET_ADVANCED_COLOR_STATE { public DEVICE_INFO_HEADER header; public uint value; }

    [DllImport("user32.dll")]
    static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
    [DllImport("user32.dll")]
    static extern int QueryDisplayConfig(uint flags, ref uint paths, IntPtr pathArray, ref uint modes, IntPtr modeArray, IntPtr topology);
    [DllImport("user32.dll")]
    static extern int DisplayConfigGetDeviceInfo(ref TARGET_DEVICE_NAME req);
    [DllImport("user32.dll")]
    static extern int DisplayConfigGetDeviceInfo(ref ADVANCED_COLOR_INFO req);
    [DllImport("user32.dll")]
    static extern int DisplayConfigSetDeviceInfo(ref SET_ADVANCED_COLOR_STATE req);

    const uint QDC_ALL_PATHS = 0x1;
    const uint GET_TARGET_NAME = 2;
    const uint GET_ADVANCED_COLOR_INFO = 9;
    const uint SET_ACS_TYPE = 10;

    class Target { public TARGET_INFO info; public string name; public bool supported; public bool enabled; }

    static List<Target> Collect()
    {
        uint nPath, nMode;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, out nPath, out nMode) != 0) throw new Exception("buffer sizes failed");
        IntPtr paths = Marshal.AllocHGlobal((int)nPath * Marshal.SizeOf(typeof(PATH_INFO)));
        IntPtr modes = Marshal.AllocHGlobal((int)nMode * 64);
        var list = new List<Target>();
        try
        {
            if (QueryDisplayConfig(QDC_ALL_PATHS, ref nPath, paths, ref nMode, modes, IntPtr.Zero) != 0) throw new Exception("query failed");
            int sz = Marshal.SizeOf(typeof(PATH_INFO));
            for (int i = 0; i < nPath; i++)
            {
                var p = (PATH_INFO)Marshal.PtrToStructure(paths + i * sz, typeof(PATH_INFO));
                if (p.targetInfo.targetAvailable == 0) continue;
                if (p.targetInfo.refreshRate.Numerator == 0) continue;   // 只处理生效中的输出
                var t = new Target { info = p.targetInfo };
                var tn = new TARGET_DEVICE_NAME();
                tn.header.type = GET_TARGET_NAME; tn.header.size = (uint)Marshal.SizeOf(typeof(TARGET_DEVICE_NAME));
                tn.header.adapterId = p.targetInfo.adapterId; tn.header.id = p.targetInfo.id;
                t.name = (DisplayConfigGetDeviceInfo(ref tn) == 0) ? tn.monitorFriendlyDeviceName : "?";
                var ac = new ADVANCED_COLOR_INFO();
                ac.header.type = GET_ADVANCED_COLOR_INFO; ac.header.size = (uint)Marshal.SizeOf(typeof(ADVANCED_COLOR_INFO));
                ac.header.adapterId = p.targetInfo.adapterId; ac.header.id = p.targetInfo.id;
                if (DisplayConfigGetDeviceInfo(ref ac) == 0)
                {
                    t.supported = (ac.value & 1) != 0;
                    t.enabled = (ac.value & 2) != 0;
                }
                list.Add(t);
            }
        }
        finally { Marshal.FreeHGlobal(paths); Marshal.FreeHGlobal(modes); }
        return list;
    }

    static string Set(Target t, bool on)
    {
        var s = new SET_ADVANCED_COLOR_STATE();
        s.header.type = SET_ACS_TYPE;
        s.header.size = (uint)Marshal.SizeOf(typeof(SET_ADVANCED_COLOR_STATE));
        s.header.adapterId = t.info.adapterId;
        s.header.id = t.info.id;
        s.value = on ? 1u : 0u;
        int hr = DisplayConfigSetDeviceInfo(ref s);
        return string.Format("{0}: set HDR {1} -> hr=0x{2:X8}", t.name, on ? "ON" : "OFF", hr);
    }

    public static List<string> Run()
    {
        var log = new List<string>();
        log.Add("--- before ---");
        foreach (var t in Collect())
            log.Add(string.Format("{0}: supported={1} enabled={2}", t.name, t.supported, t.enabled));

        log.Add("--- cycling HDR on/off to reset the link ---");
        foreach (var t in Collect())
        {
            if (!t.supported) { log.Add(t.name + ": HDR not supported, skipped"); continue; }
            log.Add(Set(t, true));
            Thread.Sleep(2500);
            log.Add(Set(t, false));
            Thread.Sleep(1500);
        }

        log.Add("--- after ---");
        foreach (var t in Collect())
            log.Add(string.Format("{0}: supported={1} enabled={2}", t.name, t.supported, t.enabled));
        return log;
    }
}
'@

if (-not ('HdrCycle' -as [type])) { Add-Type -TypeDefinition $src -Language CSharp -ErrorAction Stop }
[HdrCycle]::Run() | ForEach-Object { Write-Output $_ }

