// 探针2：搞清楚"9070 XT 呈现"到底哪条路可行
//   测试1：窗口放在 9070 XT 的显示器上 → 用 9070 XT 的交换链 Present（同适配器，应该行）
//   测试2：同一个窗口 → 用 7900 XT 的交换链 Present（跨适配器，这是"游戏渲染在 7900 XT"的情形）
// 结果与所有 HRESULT 写入 probe_report.txt
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cwchar>

using Microsoft::WRL::ComPtr;

static FILE* g_rep = nullptr;
static void Logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout); fflush(stdout);
    if (g_rep) { fputs(buf, g_rep); fflush(g_rep); }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void Pump() { MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }

struct AdInfo {
    ComPtr<IDXGIAdapter1> ad;
    wchar_t name[128];
    int outputs = 0;
    RECT monRect{};
    bool hasMon = false;
};

// 在指定适配器上用指定颜色 Present 若干帧
static HRESULT PresentColor(AdInfo& ai, HWND hwnd, float r, float g, float b, int frames, const char* tag) {
    ComPtr<ID3D12Device> dev;
    HRESULT hr = D3D12CreateDevice(ai.ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    Logf("[%s] D3D12CreateDevice 0x%08X\n", tag, (unsigned)hr);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D12CommandQueue> q;
    D3D12_COMMAND_QUEUE_DESC qd{};
    hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    Logf("[%s] CreateCommandQueue 0x%08X\n", tag, (unsigned)hr);

    ComPtr<IDXGIFactory6> fac;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));

    RECT rc{}; GetClientRect(hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)(rc.right - rc.left);
    sd.Height = (UINT)(rc.bottom - rc.top);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> s1;
    hr = fac->CreateSwapChainForHwnd(q.Get(), hwnd, &sd, nullptr, nullptr, &s1);
    Logf("[%s] CreateSwapChainForHwnd 0x%08X\n", tag, (unsigned)hr);
    if (FAILED(hr)) return hr;

    ComPtr<IDXGISwapChain3> sc;
    s1.As(&sc);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 3;
    ComPtr<ID3D12DescriptorHeap> heap;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> bb[3];
    for (UINT i = 0; i < 3; ++i) {
        sc->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * inc;
        dev->CreateRenderTargetView(bb[i].Get(), nullptr, h);
    }
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ComPtr<ID3D12Fence> fe;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fe));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    HRESULT lastPresent = 0;

    for (int f = 0; f < frames; ++f) {
        Pump();
        UINT idx = sc->GetCurrentBackBufferIndex();
        al->Reset();
        cl->Reset(al.Get(), nullptr);
        D3D12_RESOURCE_BARRIER br{};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = bb[idx].Get();
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        br.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &br);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)idx * inc;
        float col[4] = { r, g, b, 1.0f };
        cl->ClearRenderTargetView(rtv, col, 0, nullptr);
        br.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        br.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &br);
        cl->Close();
        ID3D12CommandList* l[] = { cl.Get() };
        q->ExecuteCommandLists(1, l);
        q->Signal(fe.Get(), ++fv);
        if (fe->GetCompletedValue() < fv) { fe->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 2000); }
        lastPresent = sc->Present(1, 0);
        if (f == 0) Logf("[%s] 首次 Present 0x%08X\n", tag, (unsigned)lastPresent);
    }
    Logf("[%s] 共 Present %d 帧，最后返回 0x%08X\n", tag, frames, (unsigned)lastPresent);
    return lastPresent;
}

int main() {
    SetConsoleOutputCP(65001);
    { char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp); strcat_s(tmp, "probe_report.txt"); fopen_s(&g_rep, tmp, "w"); }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Probe2Wnd";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    ComPtr<IDXGIFactory6> fac;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));

    AdInfo infos[8]; int n = 0;
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        infos[n].ad = a;
        wcsncpy_s(infos[n].name, d.Description, _TRUNCATE);
        ComPtr<IDXGIOutput> o;
        for (UINT k = 0; k < 8; ++k) {
            ComPtr<IDXGIOutput> oo;
            if (a->EnumOutputs(k, &oo) != S_OK) break;
            if (infos[n].outputs == 0) {
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(oo->GetDesc(&od))) { infos[n].monRect = od.DesktopCoordinates; infos[n].hasMon = true; }
            }
            infos[n].outputs++;
        }
        Logf("适配器 %d: %ls  输出=%d  monitor=(%ld,%ld)-(%ld,%ld)\n", i, d.Description, infos[n].outputs,
          infos[n].monRect.left, infos[n].monRect.top, infos[n].monRect.right, infos[n].monRect.bottom);
        n++;
    }

    // 找到 9070 XT 与 7900 XT
    int i9070 = -1, i7900 = -1;
    for (int i = 0; i < n; ++i) {
        if (wcsstr(infos[i].name, L"9070") && infos[i].outputs > 0) i9070 = i;
        if (wcsstr(infos[i].name, L"7900") && infos[i].outputs > 0) i7900 = i;
    }
    Logf("\n9070XT(有输出)=%d  7900XT(有输出)=%d\n", i9070, i7900);
    if (i9070 < 0) { Logf("找不到 9070 XT 的输出，退出\n"); return 1; }

    // 窗口放到 9070 XT 的显示器中间
    RECT mr = infos[i9070].monRect;
    int w = 900, h = 600;
    int x = mr.left + ((mr.right - mr.left) - w) / 2;
    int y = mr.top + ((mr.bottom - mr.top) - h) / 2;
    Logf("窗口位置 (%d,%d) 尺寸 %dx%d（应在 9070 XT 的屏幕上）\n", x, y, w, h);
    HWND hwnd = CreateWindowExW(0, L"Probe2Wnd", L"探针2：先绿后蓝", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Logf("建窗口失败\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    Pump();

    Logf("\n===== 测试1：9070 XT 的交换链，绿色 8 秒 =====\n");
    PresentColor(infos[i9070], hwnd, 0.0f, 1.0f, 0.0f, 480, "9070XT");

    Logf("\n===== 测试2：7900 XT 的交换链（跨适配器），蓝色 8 秒 =====\n");
    if (i7900 >= 0) PresentColor(infos[i7900], hwnd, 0.0f, 0.0f, 1.0f, 480, "7900XT");
    else Logf("7900 XT 没有输出，跳过测试2\n");

    Logf("\n===== 结束 =====\n");
    if (g_rep) fclose(g_rep);
    return 0;
}
