// luid_dump —— 打印每个 DXGI 适配器的序号、名称、LUID，用于把 GPU 计数器对到具体显卡
#include <windows.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

int main() {
    SetConsoleOutputCP(65001);
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) { printf("factory 失败\n"); return 1; }
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> ad;
        if (fac->EnumAdapters1(i, ad.GetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        printf("[%u] %-42ls LUID=0x%08X%08X  显存=%llu MB\n", i, d.Description,
               (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart,
               (unsigned long long)(d.DedicatedVideoMemory / (1024 * 1024)));

        // 这个适配器带了哪些输出（显示器）及其桌面坐标
        for (UINT o = 0; o < 8; ++o) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, out.GetAddressOf()) != S_OK) break;
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(out->GetDesc(&od))) {
                printf("      输出[%u] %-24ls 桌面=(%ld,%ld)-(%ld,%ld) attached=%d\n", o, od.DeviceName,
                       od.DesktopCoordinates.left, od.DesktopCoordinates.top,
                       od.DesktopCoordinates.right, od.DesktopCoordinates.bottom,
                       (int)od.AttachedToDesktop);
            }
        }
    }
    return 0;
}
