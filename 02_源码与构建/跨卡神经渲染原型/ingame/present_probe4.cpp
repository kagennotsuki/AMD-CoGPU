// 探针4：MGPU 的 DcompOverlay 路线
//   窗口在 9070 XT 的显示器上
//   A：7900 XT 的交换链（模拟游戏，持续 Present 灰色）
//   B：9070 XT 的 **合成交换链**（CreateSwapChainForComposition，不绑 HWND）
//      通过 DirectComposition 作为 overlay 盖在窗口上，Present 绿色
// 若窗口显示绿色（盖住灰）→ 方案成立
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <dcomp.h>
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

// 建一个交换链并返回其命令对象，供逐帧清屏
struct Canvas {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> bb[3];
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fe;
    HANDLE ev = nullptr;
    UINT inc = 0;
    UINT64 fv = 0;

    bool InitBackbuffers() {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 3;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return false;
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (UINT i = 0; i < 3; ++i) {
            if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&bb[i])))) return false;
            D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * inc;
            dev->CreateRenderTargetView(bb[i].Get(), nullptr, h);
        }
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl));
        cl->Close();
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fe));
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void Frame(float r, float g, float b) {
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
        sc->Present(0, 0);   // 合成交换链不能带 vsync 参数限制，用 0
    }
};

int main() {
    SetConsoleOutputCP(65001);
    { char tmp[MAX_PATH]; GetTempPathA(MAX_PATH, tmp); strcat_s(tmp, "probe4_report.txt"); fopen_s(&g_rep, tmp, "w"); }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Probe4Wnd";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { Logf("factory 失败\n"); return 1; }

    ComPtr<IDXGIAdapter1> ad7900, ad9070;
    RECT mon9070{};
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        ComPtr<IDXGIOutput> o;
        bool hasOut = (a->EnumOutputs(0, &o) == S_OK);
        if (hasOut) {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(o->GetDesc(&od)) && wcsstr(d.Description, L"9070")) mon9070 = od.DesktopCoordinates;
        }
        if (hasOut && !ad7900 && wcsstr(d.Description, L"7900")) ad7900 = a;
        if (hasOut && !ad9070 && wcsstr(d.Description, L"9070")) ad9070 = a;
    }
    if (!ad7900 || !ad9070) { Logf("找不到两张卡\n"); return 1; }

    int w = 900, h = 600;
    int x = mon9070.left + ((mon9070.right - mon9070.left) - w) / 2;
    int y = mon9070.top + ((mon9070.bottom - mon9070.top) - h) / 2;
    HWND hwnd = CreateWindowExW(0, L"Probe4Wnd", L"探针4：DComp overlay", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Logf("建窗口失败\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); Pump();
    Logf("窗口在 9070 XT 屏 (%d,%d) %dx%d\n\n", x, y, w, h);

    // ---------- A：7900 XT 的普通交换链（模拟游戏，持续 Present 深灰）----------
    Canvas A;
    HRESULT hr = D3D12CreateDevice(ad7900.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&A.dev));
    Logf("[A] dev 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    A.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&A.q));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sA;
    hr = fac->CreateSwapChainForHwnd(A.q.Get(), hwnd, &sd, nullptr, nullptr, &sA);
    Logf("[A] CreateSwapChainForHwnd 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;
    sA.As(&A.sc);
    if (!A.InitBackbuffers()) { Logf("[A] 后备缓冲失败\n"); return 1; }
    // 先画一帧深灰，证明 A 在出画
    for (int i = 0; i < 3; ++i) { A.Frame(0.15f, 0.15f, 0.15f); Pump(); }
    Logf("[A] 已 Present 深灰 3 帧（窗口现在应是深灰）\n\n");

    // ---------- B：9070 XT 的合成交换链 + DComp overlay ----------
    Canvas B;
    hr = D3D12CreateDevice(ad9070.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&B.dev));
    Logf("[B] dev 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;
    B.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&B.q));

    ComPtr<IDCompositionDevice> dc;
    hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&dc));
    Logf("[B] DCompositionCreateDevice 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;
    ComPtr<IDCompositionTarget> target;
    hr = dc->CreateTargetForHwnd(hwnd, TRUE, &target);
    Logf("[B] CreateTargetForHwnd 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;
    ComPtr<IDCompositionVisual> visual;
    hr = dc->CreateVisual(&visual);
    Logf("[B] CreateVisual 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;
    hr = target->SetRoot(visual.Get());
    Logf("[B] SetRoot 0x%08X\n", (unsigned)hr);

    DXGI_SWAP_CHAIN_DESC1 cd{};
    cd.Width = w; cd.Height = h; cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cd.SampleDesc.Count = 1; cd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    cd.BufferCount = 3; cd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    cd.AlphaMode = DXGI_ALPHA_MODE_IGNORE; cd.Scaling = DXGI_SCALING_STRETCH;
    ComPtr<IDXGISwapChain1> sB;
    hr = fac->CreateSwapChainForComposition(B.q.Get(), &cd, nullptr, &sB);
    Logf("[B] CreateSwapChainForComposition 0x%08X  <-- 关键\n", (unsigned)hr);
    if (FAILED(hr)) { Logf("★ 合成交换链创建失败\n"); return 1; }
    sB.As(&B.sc);
    if (!B.InitBackbuffers()) { Logf("[B] 后备缓冲失败\n"); return 1; }
    hr = visual->SetContent(sB.Get());
    Logf("[B] visual->SetContent 0x%08X\n", (unsigned)hr);
    hr = dc->Commit();
    Logf("[B] Commit 0x%08X\n\n", (unsigned)hr);

    Logf(">>> 现在 B 持续 Present 绿色 10 秒；窗口应从深灰变绿 = 方案成立 <<<\n");
    for (int f = 0; f < 600; ++f) {
        A.Frame(0.15f, 0.15f, 0.15f);   // 模拟游戏一直在出画
        B.Frame(0.0f, 1.0f, 0.0f);      // overlay 出绿
        Pump();
        if (f == 0) Logf("首帧完成\n");
    }
    Logf("\n完成。窗口若显示绿色 → DComp overlay 方案成立\n");
    if (g_rep) fclose(g_rep);
    return 0;
}
