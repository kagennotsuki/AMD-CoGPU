// 探针3：真实场景 —— 同一个窗口上同时存在两个交换链
//   A：7900 XT 的交换链（模拟游戏，创建后**从不 Present**）
//   B：9070 XT 的交换链（我们的呈现层，Present 绿色）
// 若窗口变绿 → "游戏渲染在 7900 XT、由 9070 XT 呈现" 方案成立
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>

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

int main() {
    SetConsoleOutputCP(65001);
    { char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp); strcat_s(tmp, "probe3_report.txt"); fopen_s(&g_rep, tmp, "w"); }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Probe3Wnd";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { Logf("CreateDXGIFactory2 失败\n"); return 1; }

    ComPtr<IDXGIAdapter1> ad7900, ad9070;
    RECT mon9070{};
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        bool hasOut = false;
        ComPtr<IDXGIOutput> o;
        if (a->EnumOutputs(0, &o) == S_OK) {
            hasOut = true;
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(o->GetDesc(&od))) {
                if (wcsstr(d.Description, L"9070")) mon9070 = od.DesktopCoordinates;
            }
        }
        Logf("适配器 %d: %ls 输出=%d\n", i, d.Description, hasOut ? 1 : 0);
        if (hasOut && !ad7900 && wcsstr(d.Description, L"7900")) ad7900 = a;
        if (hasOut && !ad9070 && wcsstr(d.Description, L"9070")) ad9070 = a;
    }
    if (!ad7900 || !ad9070) { Logf("找不到两张输出卡，退出\n"); return 1; }

    int w = 900, h = 600;
    int x = mon9070.left + ((mon9070.right - mon9070.left) - w) / 2;
    int y = mon9070.top + ((mon9070.bottom - mon9070.top) - h) / 2;
    HWND hwnd = CreateWindowExW(0, L"Probe3Wnd", L"探针3：同时两个交换链", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Logf("建窗口失败\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    Pump();
    Logf("窗口在 9070 XT 屏 (%d,%d)\n\n", x, y);

    // ---------- A：7900 XT 的交换链（模拟游戏，存活但从不 Present）----------
    ComPtr<ID3D12Device> devA;
    ComPtr<ID3D12CommandQueue> qA;
    ComPtr<IDXGISwapChain1> scA;
    HRESULT hr = D3D12CreateDevice(ad7900.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devA));
    Logf("[A/7900XT] D3D12CreateDevice 0x%08X\n", (unsigned)hr);
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (SUCCEEDED(hr)) devA->CreateCommandQueue(&qd, IID_PPV_ARGS(&qA));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = fac->CreateSwapChainForHwnd(qA.Get(), hwnd, &sd, nullptr, nullptr, &scA);
    Logf("[A/7900XT] CreateSwapChainForHwnd 0x%08X  <-- A 保持存活，不 Present\n\n", (unsigned)hr);
    if (FAILED(hr)) return 1;

    // ---------- B：9070 XT 的交换链（我们的呈现层）----------
    ComPtr<ID3D12Device> devB;
    ComPtr<ID3D12CommandQueue> qB;
    ComPtr<IDXGISwapChain1> s1;
    hr = D3D12CreateDevice(ad9070.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devB));
    Logf("[B/9070XT] D3D12CreateDevice 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;
    devB->CreateCommandQueue(&qd, IID_PPV_ARGS(&qB));
    hr = fac->CreateSwapChainForHwnd(qB.Get(), hwnd, &sd, nullptr, nullptr, &s1);
    Logf("[B/9070XT] CreateSwapChainForHwnd 0x%08X  <-- 同窗口第二个交换链\n", (unsigned)hr);
    if (FAILED(hr)) { Logf("★ 同一窗口不能有两个交换链（方案不成立）\n"); return 1; }

    ComPtr<IDXGISwapChain3> scB;
    s1.As(&scB);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 3;
    ComPtr<ID3D12DescriptorHeap> heap;
    devB->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    UINT inc = devB->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> bb[3];
    for (UINT i = 0; i < 3; ++i) {
        scB->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE hh = heap->GetCPUDescriptorHandleForHeapStart();
        hh.ptr += (SIZE_T)i * inc;
        devB->CreateRenderTargetView(bb[i].Get(), nullptr, hh);
    }
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    devB->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    devB->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ComPtr<ID3D12Fence> fe;
    devB->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fe));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;

    Logf("\n>>> 开始从 B(9070XT) Present 绿色 10 秒。窗口变绿=成功 <<<\n");
    for (int f = 0; f < 600; ++f) {
        Pump();
        UINT idx = scB->GetCurrentBackBufferIndex();
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
        float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        cl->ClearRenderTargetView(rtv, green, 0, nullptr);
        br.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        br.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &br);
        cl->Close();
        ID3D12CommandList* l[] = { cl.Get() };
        qB->ExecuteCommandLists(1, l);
        qB->Signal(fe.Get(), ++fv);
        if (fe->GetCompletedValue() < fv) { fe->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 2000); }
        HRESULT hp = scB->Present(1, 0);
        if (f == 0) Logf("[B/9070XT] 首次 Present 0x%08X\n", (unsigned)hp);
        if (f == 300) Logf("[B/9070XT] 300 帧时 Present 仍返回 0x%08X\n", (unsigned)hp);
    }
    Logf("\n完成。窗口若为绿色 → 方案成立（A 存活但未 Present，B 正常出画）\n");
    if (g_rep) fclose(g_rep);
    return 0;
}
