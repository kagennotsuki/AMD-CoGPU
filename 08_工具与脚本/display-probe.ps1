$src = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class DisplayProbe
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
    public struct ADVANCED_COLOR_INFO
    {
        public DEVICE_INFO_HEADER header;
        public uint value; public uint colorEncoding; public uint bitsPerColorChannel;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DISPLAY_DEVICE
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion; public short dmDriverVersion; public short dmSize; public short dmDriverExtra;
        public int dmFields; public int dmPositionX; public int dmPositionY;
        public int dmDisplayOrientation; public int dmDisplayFixedOutput;
        public short dmColor; public short dmDuplex; public short dmYResolution; public short dmTTOption; public short dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels; public int dmBitsPerPel; public int dmPelsWidth; public int dmPelsHeight;
        public int dmDisplayFlags; public int dmDisplayFrequency;
        public int dmICMMethod; public int dmICMIntent; public int dmMediaType; public int dmDitherType;
        public int dmReserved1; public int dmReserved2; public int dmPanningWidth; public int dmPanningHeight;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettingsEx(string lpszDeviceName, int iModeNum, ref DEVMODE lpDevMode, uint dwFlags);

    [DllImport("user32.dll")]
    static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPathArrayElements, out uint numModeInfoArrayElements);

    [DllImport("user32.dll")]
    static extern int QueryDisplayConfig(uint flags, ref uint numPathArrayElements, IntPtr pathArray,
        ref uint numModeInfoArrayElements, IntPtr modeInfoArray, IntPtr currentTopologyId);

    [DllImport("user32.dll")]
    static extern int DisplayConfigGetDeviceInfo(ref TARGET_DEVICE_NAME requestPacket);

    [DllImport("user32.dll")]
    static extern int DisplayConfigGetDeviceInfo(ref ADVANCED_COLOR_INFO requestPacket);

    const uint QDC_ALL_PATHS = 0x1;
    const uint GET_TARGET_NAME = 2;
    const uint GET_ADVANCED_COLOR_INFO = 9;

    public static List<string> Run()
    {
        var outp = new List<string>();
        outp.Add("sizeof PATH_INFO=" + Marshal.SizeOf(typeof(PATH_INFO)) + " DEVMODE=" + Marshal.SizeOf(typeof(DEVMODE)) + " TARGET_DEVICE_NAME=" + Marshal.SizeOf(typeof(TARGET_DEVICE_NAME)) + " ADVANCED_COLOR_INFO=" + Marshal.SizeOf(typeof(ADVANCED_COLOR_INFO)));

        // 1) classic EnumDisplayDevices + current mode
        var dd = new DISPLAY_DEVICE(); dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
        for (uint i = 0; EnumDisplayDevices(null, i, ref dd, 0); i++)
        {
            if ((dd.StateFlags & 0x1) == 0) { dd = new DISPLAY_DEVICE(); dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE)); continue; }
            var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
            if (EnumDisplaySettingsEx(dd.DeviceName, -1, ref dm, 0))
            {
                string primary = (dd.StateFlags & 0x4) != 0 ? " PRIMARY" : "";
                outp.Add(string.Format("GDI|{0}|{1} @ {2},{3} {4}x{5} {6}Hz {7}bpp{8}",
                    dd.DeviceName, "adapter", dm.dmPositionX, dm.dmPositionY, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmBitsPerPel, primary));
            }
            var mon = new DISPLAY_DEVICE(); mon.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
            if (EnumDisplayDevices(dd.DeviceName, 0, ref mon, 0))
                outp.Add("MONITOR|" + dd.DeviceName + "|" + mon.DeviceString + "|" + mon.DeviceID);
            dd = new DISPLAY_DEVICE(); dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
        }

        // 2) DisplayConfig: per-target name + HDR state
        uint nPath, nMode;
        int hr = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, out nPath, out nMode);
        if (hr != 0) { outp.Add("ERR GetDisplayConfigBufferSizes hr=" + hr); return outp; }
        IntPtr paths = Marshal.AllocHGlobal((int)nPath * Marshal.SizeOf(typeof(PATH_INFO)));
        IntPtr modes = Marshal.AllocHGlobal((int)nMode * 64);
        try
        {
            hr = QueryDisplayConfig(QDC_ALL_PATHS, ref nPath, paths, ref nMode, modes, IntPtr.Zero);
            if (hr != 0) { outp.Add("ERR QueryDisplayConfig hr=" + hr); return outp; }
            int pathSize = Marshal.SizeOf(typeof(PATH_INFO));
            for (int i = 0; i < nPath; i++)
            {
                var p = (PATH_INFO)Marshal.PtrToStructure(paths + i * pathSize, typeof(PATH_INFO));
                if (p.targetInfo.targetAvailable == 0) continue;
                var tn = new TARGET_DEVICE_NAME();
                tn.header.type = GET_TARGET_NAME;
                tn.header.size = (uint)Marshal.SizeOf(typeof(TARGET_DEVICE_NAME));
                tn.header.adapterId = p.targetInfo.adapterId;
                tn.header.id = p.targetInfo.id;
                string name = "?";
                if (DisplayConfigGetDeviceInfo(ref tn) == 0) name = tn.monitorFriendlyDeviceName;

                var ac = new ADVANCED_COLOR_INFO();
                ac.header.type = GET_ADVANCED_COLOR_INFO;
                ac.header.size = (uint)Marshal.SizeOf(typeof(ADVANCED_COLOR_INFO));
                ac.header.adapterId = p.targetInfo.adapterId;
                ac.header.id = p.targetInfo.id;
                string hdr = "n/a";
                if (DisplayConfigGetDeviceInfo(ref ac) == 0)
                {
                    bool supported = (ac.value & 1) != 0;
                    bool enabled = (ac.value & 2) != 0;
                    bool wideForced = (ac.value & 4) != 0;
                    bool forceDisabled = (ac.value & 8) != 0;
                    hdr = string.Format("supported={0} enabled={1} wideColorEnforced={2} forceDisabled={3} encoding={4} bitsPerChannel={5}",
                        supported, enabled, wideForced, forceDisabled, ac.colorEncoding, ac.bitsPerColorChannel);
                }
                outp.Add(string.Format("TARGET|{0}|refreshrate={1}/{2}Hz|scaling={3}|rotation={4}|outtech={5}|HDR {6}",
                    name, p.targetInfo.refreshRate.Numerator, p.targetInfo.refreshRate.Denominator,
                    p.targetInfo.scaling, p.targetInfo.rotation, p.targetInfo.outputTechnology, hdr));
            }
        }
        finally
        {
            Marshal.FreeHGlobal(paths);
            Marshal.FreeHGlobal(modes);
        }
        return outp;
    }
}
'@

if (-not ('DisplayProbe' -as [type])) { Add-Type -TypeDefinition $src -Language CSharp -ErrorAction Stop }
[DisplayProbe]::Run() | ForEach-Object { Write-Output $_ }
