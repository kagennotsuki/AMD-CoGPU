// 探针：同一窗口，两张卡各建一个交换链
//   A 卡（无输出，模拟游戏渲染卡）：建交换链但从不 Present
//   B 卡（有输出，NR 卡）      ：建交换链并 Present 绿色
// 若窗口变绿 → "NR 卡直接呈现" 方案成立。
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
static FILE* g_rep = nullptr;
static void R(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap); va_end(ap);
    if (g_rep) { va_start(ap, fmt); vfprintf(g_rep, fmt, ap); va_end(ap); fflush(g_rep); }
    fflush(stdout);
}

using Microsoft::WRL::ComPtr;

static HWND g_hwnd;
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

int main() {
    SetConsoleOutputCP(65001);
    fopen_s(&g_rep, "E:\\AI\\跨卡神经渲染原型\\build\\probe_report.txt", "w");
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PresentProbe";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(0, L"PresentProbe", L"探针：窗口应立即变绿", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             100, 100, 900, 600, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) { R("建窗口失败\n"); return 1; }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    Pump();

    ComPtr<IDXGIFactory6> fac;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));

    // 枚举适配器 + 输出
    struct Info { ComPtr<IDXGIAdapter1> ad; int outputs; wchar_t name[128]; };
    Info infos[8]; int n = 0;
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        int outs = 0;
        { ComPtr<IDXGIOutput> o; while (a->EnumOutputs((UINT)outs, &o) == S_OK) { outs++; o.Reset(); } }
        infos[n].ad = a; infos[n].outputs = outs;
        wcsncpy_s(infos[n].name, d.Description, _TRUNCATE);
        R("适配器 %d: %ls  输出=%d\n", i, d.Description, outs);
        n++;
    }

    int renderIdx = -1, presentIdx = -1;
    for (int i = 0; i < n; ++i) if (infos[i].outputs == 0 && renderIdx < 0) renderIdx = i;
    for (int i = 0; i < n; ++i) if (infos[i].outputs > 0 && presentIdx < 0) presentIdx = i;
    if (renderIdx < 0) renderIdx = 0;
    if (presentIdx < 0) presentIdx = (renderIdx == 0) ? (n > 1 ? 1 : 0) : 0;
    R("\n渲染卡(无输出)=%d  呈现卡(有输出)=%d\n", renderIdx, presentIdx);

    // ---------- A：渲染卡上的交换链（从不 Present）----------
    ComPtr<ID3D12Device> devA;
    ComPtr<ID3D12CommandQueue> qA;
    ComPtr<IDXGISwapChain1> scA;
    HRESULT hrA = D3D12CreateDevice(infos[renderIdx].ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devA));
    R("devA 0x%08X\n", (unsigned)hrA);
    if (SUCCEEDED(hrA)) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        devA->CreateCommandQueue(&qd, IID_PPV_ARGS(&qA));
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 900; sd.Height = 600; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT h = fac->CreateSwapChainForHwnd(qA.Get(), g_hwnd, &sd, nullptr, nullptr, &scA);
        R("scA(渲染卡,不Present) 0x%08X\n", (unsigned)h);
    }

    // ---------- B：呈现卡上的交换链（Present 绿色）----------
    ComPtr<ID3D12Device> devB;
    ComPtr<ID3D12CommandQueue> qB;
    ComPtr<IDXGISwapChain3> scB;
    HRESULT hrB = D3D12CreateDevice(infos[presentIdx].ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devB));
    R("devB 0x%08X\n", (unsigned)hrB);
    if (FAILED(hrB)) { R("呈现卡设备创建失败，退出\n");  return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    devB->CreateCommandQueue(&qd, IID_PPV_ARGS(&qB));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 900; sd.Height = 600; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> s1;
    HRESULT hB = fac->CreateSwapChainForHwnd(qB.Get(), g_hwnd, &sd, nullptr, nullptr, &s1);
    R("scB(呈现卡) 0x%08X\n", (unsigned)hB);
    if (FAILED(hB)) { R("★ 同一窗口不能建第二个交换链（这就是结论）\n");  return 1; }
    s1.As(&scB);

    // RTV + allocator + list
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 3;
    ComPtr<ID3D12DescriptorHeap> heap;
    devB->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    UINT inc = devB->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> bb[3];
    for (UINT i = 0; i < 3; ++i) {
        scB->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * inc;
        devB->CreateRenderTargetView(bb[i].Get(), nullptr, h);
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

    R("\n>>> 现在开始从呈现卡 Present 绿色，持续 20 秒。窗口应该立刻变绿 <<<\n");
    for (int f = 0; f < 600; ++f) {
        Pump();
        UINT idx = scB->GetCurrentBackBufferIndex();
        al->Reset();
        cl->Reset(al.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb[idx].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)idx * inc;
        float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        cl->ClearRenderTargetView(rtv, green, 0, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* l[] = { cl.Get() };
        qB->ExecuteCommandLists(1, l);
        qB->Signal(fe.Get(), ++fv);
        if (fe->GetCompletedValue() < fv) { fe->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 1000); }
        HRESULT hp = scB->Present(1, 0);
        if (f == 0) R("首次 Present 返回 0x%08X\n", (unsigned)hp);
        if (f == 120) R("120 帧后仍在 Present（说明成功）\n");
    }
    R("\n完成。如果窗口是绿色 → 方案可行。\n");
    
    return 0;
}
