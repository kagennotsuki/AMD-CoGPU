// dxgi.dll 代理 —— RE2 游戏内集成 M1
//
// 原理：
//   · 本 DLL 放在游戏目录，替代系统 dxgi.dll；所有未实现的导出转发到 dxgi_orig.dll
//   · 实现 CreateDXGIFactory1/2，拿到工厂后替换其 vtable 里的建交换链函数
//   · 交换链建好后替换 vtable 里的 Present/Present1/ResizeBuffers
//   · Present 里执行：后备缓冲 → 回读 → CPU → 上传 → 写回后备缓冲
//     这一步就是跨卡管道的宿主侧骨架，M2 会把 CPU 环节换成 9070 XT 推理
//
// 环境变量：
//   CROSSGPU_LOG=1     写日志到游戏目录 crossgpu.log
//   CROSSGPU_INVERT=1  写回时做反色（可视证据）
//   CROSSGPU_OFF=1     完全旁路

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <cstdlib>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

// ---- 由 SDK 头文件解析得到的槽位 ----
static const int SLOT_CreateSwapChain = 10;
static const int SLOT_CreateSwapChainForHwnd = 15;
static const int SLOT_CreateSwapChainForComposition = 24;
static const int SLOT_Present = 8;
static const int SLOT_Present1 = 22;
static const int SLOT_ResizeBuffers = 13;
static const int kCopySlots = 48;

static bool g_log = false;
static bool g_invert = false;
static bool g_off = false;
static FILE* g_logFile = nullptr;

// 读同目录 crossgpu.ini（游戏会自重启，环境变量传不下去，所以用文件配置）
static int IniInt(const char* key, int def) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW((HMODULE)&IniInt, path, MAX_PATH);   // 本模块所在目录
    std::wstring p(path);
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return def;
    p = p.substr(0, slash) + L"\\crossgpu.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"r") != 0 || !f) return def;
    char line[256];
    int result = def;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        if ((size_t)(eq - line) != klen) continue;
        if (_strnicmp(line, key, klen) != 0) continue;
        result = atoi(eq + 1);
    }
    fclose(f);
    return result;
}

static void LogInit() {
    char buf[8];
    g_log = IniInt("Log", 0) != 0 || GetEnvironmentVariableA("CROSSGPU_LOG", buf, sizeof(buf)) > 0;
    g_invert = IniInt("Invert", 0) != 0 || GetEnvironmentVariableA("CROSSGPU_INVERT", buf, sizeof(buf)) > 0;
    g_off = IniInt("Off", 0) != 0 || GetEnvironmentVariableA("CROSSGPU_OFF", buf, sizeof(buf)) > 0;
    if (g_log) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p(path);
        p = p.substr(0, p.find_last_of(L'\\')) + L"\\crossgpu.log";
        _wfopen_s(&g_logFile, p.c_str(), L"w");
    }
}

static void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

// ---- 真实 dxgi ----
static HMODULE RealDxgi() {
    static HMODULE m = nullptr;
    if (!m) {
        m = LoadLibraryW(L"dxgi_orig.dll");
        Log("LoadLibraryW(dxgi_orig.dll) -> %p (err %lu)", m, GetLastError());
    }
    return m;
}

template <typename Fn>
static Fn RealProc(const char* name) {
    HMODULE m = RealDxgi();
    if (!m) return nullptr;
    return (Fn)GetProcAddress(m, name);
}

// ---- 转发跳板的函数指针表（forwarders.asm 按索引 jmp 到这里）----
extern "C" void* fwdTargets[18] = {};
static const char* kFwdNames[18] = {
#include "fwd_names.inc"
};

static void InitForwarders() {
    HMODULE m = RealDxgi();
    for (int i = 0; i < 18; ++i) {
        fwdTargets[i] = m ? (void*)GetProcAddress(m, kFwdNames[i]) : nullptr;
        if (!fwdTargets[i]) Log("forwarder missing: %s", kFwdNames[i]);
    }
}

// ---- vtable 拷贝与替换 ----
static void** CopyVtbl(void* obj) {
    void** vtbl = *(void***)obj;
    void** copy = (void**)malloc(sizeof(void*) * kCopySlots);
    memcpy(copy, vtbl, sizeof(void*) * kCopySlots);
    return copy;
}

static void SwapVptr(void* obj, void** newVtbl) {
    DWORD old = 0;
    if (VirtualProtect(obj, sizeof(void*), PAGE_READWRITE, &old)) {
        *(void***)obj = newVtbl;
        VirtualProtect(obj, sizeof(void*), old, &old);
    } else {
        Log("VirtualProtect(obj) failed %lu", GetLastError());
    }
}

// ================= 管线（M1：CPU 中转 + 可选反色） =================
struct FrameCtx {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fv = 0;

    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> backbuffer;

    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT64 rowPitch = 0, sliceBytes = 0;
    std::vector<uint8_t> host;

    bool ready = false;
    int frames = 0;
    double sumMs = 0, maxMs = 0;
    bool loggedInit = false;
};

static FrameCtx g_ctx;

static void WaitGpu() {
    if (!g_ctx.queue || !g_ctx.fence) return;
    g_ctx.queue->Signal(g_ctx.fence.Get(), ++g_ctx.fv);
    while (g_ctx.fence->GetCompletedValue() < g_ctx.fv) Sleep(0);
}

static bool AcquireDevice(IDXGISwapChain* sc) {
    if (g_ctx.device) return true;
    ComPtr<IUnknown> unk;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&unk))) || !unk) {
        Log("swapchain->GetDevice failed");
        return false;
    }
    // D3D12 的交换链是用命令队列创建的，GetDevice 返回该队列
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(unk.As(&q)) || !q) {
        Log("swapchain device is not ID3D12CommandQueue -> 游戏可能是 D3D11 模式");
        return false;
    }
    g_ctx.queue = q;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&g_ctx.device)))) {
        Log("queue->GetDevice failed");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (FAILED(g_ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_ctx.alloc)))) return false;
    if (FAILED(g_ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ctx.alloc.Get(), nullptr,
                                              IID_PPV_ARGS(&g_ctx.list)))) return false;
    g_ctx.list->Close();
    if (FAILED(g_ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_ctx.fence)))) return false;
    if (!g_ctx.loggedInit) {
        DXGI_ADAPTER_DESC ad{};
        ComPtr<IDXGIDevice> dxdev;
        if (SUCCEEDED(g_ctx.device.As(&dxdev))) {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxdev->GetAdapter(&adapter)) && adapter) adapter->GetDesc(&ad);
        }
        Log("device acquired: %ls  (LUID %08X:%08X)", ad.Description, ad.AdapterLuid.HighPart, ad.AdapterLuid.LowPart);
        g_ctx.loggedInit = true;
    }
    return true;
}

static bool AcquireBuffers(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    {
        ComPtr<IDXGISwapChain1> sc1;
        bool got = false;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
            got = SUCCEEDED(sc1->GetDesc1(&desc));
        }
        if (!got) {
            DXGI_SWAP_CHAIN_DESC d0{};
            if (FAILED(sc->GetDesc(&d0))) return false;
            desc.Width = d0.BufferDesc.Width;
            desc.Height = d0.BufferDesc.Height;
            desc.Format = d0.BufferDesc.Format;
        }
    }
    if (g_ctx.ready && g_ctx.width == desc.Width && g_ctx.height == desc.Height && g_ctx.format == desc.Format)
        return true;

    g_ctx.readback.Reset();
    g_ctx.upload.Reset();
    g_ctx.backbuffer.Reset();
    g_ctx.ready = false;

    ComPtr<ID3D12Resource> bb;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) { Log("GetBuffer(0) failed"); return false; }
    D3D12_RESOURCE_DESC rd = bb->GetDesc();
    g_ctx.backbuffer = bb;
    g_ctx.width = (UINT)rd.Width;
    g_ctx.height = rd.Height;
    g_ctx.format = rd.Format;

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    rbHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;   // AMD 必须 UNKNOWN
    rbHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    rbHeap.CreationNodeMask = 1;
    rbHeap.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES upHeap = rbHeap;
    upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC stagingDesc = rd;
    stagingDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    stagingDesc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    stagingDesc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    stagingDesc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    if (FAILED(g_ctx.device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g_ctx.readback)))) {
        Log("readback texture failed");
        return false;
    }
    if (FAILED(g_ctx.device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &stagingDesc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&g_ctx.upload)))) {
        Log("upload texture failed");
        return false;
    }

    // 尺寸：按上传纹理的 footprint 算
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    g_ctx.device->GetCopyableFootprints(&stagingDesc, 0, 1, 0, &fp, &rows, &rowSize, &total);
    g_ctx.rowPitch = fp.Footprint.RowPitch;
    g_ctx.sliceBytes = total;
    g_ctx.host.resize((size_t)total);

    Log("staging ready: %ux%u fmt=%d rowPitch=%llu total=%llu",
        g_ctx.width, g_ctx.height, (int)g_ctx.format,
        (unsigned long long)g_ctx.rowPitch, (unsigned long long)total);
    g_ctx.ready = true;
    return true;
}

static void RunPipeline(IDXGISwapChain* sc) {
    if (g_off) return;
    if (!AcquireDevice(sc) || !AcquireBuffers(sc)) return;

    auto t0 = Clock::now();

    // 1) 后备缓冲 -> 回读
    g_ctx.alloc->Reset();
    g_ctx.list->Reset(g_ctx.alloc.Get(), nullptr);
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_ctx.backbuffer.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_ctx.list->ResourceBarrier(1, &b);
    }
    g_ctx.list->CopyResource(g_ctx.readback.Get(), g_ctx.backbuffer.Get());
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_ctx.backbuffer.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_ctx.list->ResourceBarrier(1, &b);
    }
    g_ctx.list->Close();
    {
        ID3D12CommandList* l[] = { g_ctx.list.Get() };
        g_ctx.queue->ExecuteCommandLists(1, l);
    }
    WaitGpu();

    // 2) CPU 读回（+ 可选反色）
    {
        D3D12_RANGE rr{ 0, (SIZE_T)g_ctx.sliceBytes };
        void* m = nullptr;
        if (SUCCEEDED(g_ctx.readback->Map(0, &rr, &m)) && m) {
            memcpy(g_ctx.host.data(), m, (size_t)g_ctx.sliceBytes);
            D3D12_RANGE nw{ 0, 0 };
            g_ctx.readback->Unmap(0, &nw);
        } else {
            Log("readback Map failed");
            return;
        }
    }
    if (g_invert) {
        for (size_t i = 0; i < g_ctx.host.size(); ++i) g_ctx.host[i] = (uint8_t)(255 - g_ctx.host[i]);
    }

    // 3) CPU 写上传 + 上传 -> 后备缓冲
    {
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (FAILED(g_ctx.upload->Map(0, &nr, &m)) || !m) { Log("upload Map failed"); return; }
        memcpy(m, g_ctx.host.data(), (size_t)g_ctx.sliceBytes);
        D3D12_RANGE w{ 0, (SIZE_T)g_ctx.sliceBytes };
        g_ctx.upload->Unmap(0, &w);
    }

    g_ctx.alloc->Reset();
    g_ctx.list->Reset(g_ctx.alloc.Get(), nullptr);
    g_ctx.list->CopyResource(g_ctx.backbuffer.Get(), g_ctx.upload.Get());
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_ctx.backbuffer.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_ctx.list->ResourceBarrier(1, &b);
    }
    g_ctx.list->Close();
    {
        ID3D12CommandList* l[] = { g_ctx.list.Get() };
        g_ctx.queue->ExecuteCommandLists(1, l);
    }
    WaitGpu();

    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    g_ctx.frames++;
    g_ctx.sumMs += ms;
    if (ms > g_ctx.maxMs) g_ctx.maxMs = ms;
    if (g_ctx.frames % 120 == 0) {
        Log("frames=%d  avg=%.2f ms  max=%.2f ms  (%ux%u fmt=%d)",
            g_ctx.frames, g_ctx.sumMs / g_ctx.frames, g_ctx.maxMs,
            g_ctx.width, g_ctx.height, (int)g_ctx.format);
    }
}

// ================= 交换链钩子 =================
static void** g_swapOrig = nullptr;
static void** g_swapNew = nullptr;

static HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* self, UINT sync, UINT flags);
static HRESULT STDMETHODCALLTYPE Detour_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                                const DXGI_PRESENT_PARAMETERS* params);
static HRESULT STDMETHODCALLTYPE Detour_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h,
                                                     DXGI_FORMAT format, UINT flags);

static void HookSwapChain(IDXGISwapChain* sc) {
    if (g_swapNew) { SwapVptr(sc, g_swapNew); return; }
    g_swapOrig = CopyVtbl(sc);
    g_swapNew = (void**)malloc(sizeof(void*) * kCopySlots);
    memcpy(g_swapNew, g_swapOrig, sizeof(void*) * kCopySlots);
    g_swapNew[SLOT_Present] = (void*)&Detour_Present;
    g_swapNew[SLOT_Present1] = (void*)&Detour_Present1;
    g_swapNew[SLOT_ResizeBuffers] = (void*)&Detour_ResizeBuffers;
    SwapVptr(sc, g_swapNew);
    Log("swapchain hooked (Present@%d Present1@%d ResizeBuffers@%d)", SLOT_Present, SLOT_Present1, SLOT_ResizeBuffers);
}

static HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    RunPipeline(self);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    return ((Fn)g_swapOrig[SLOT_Present])(self, sync, flags);
}

static HRESULT STDMETHODCALLTYPE Detour_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                                const DXGI_PRESENT_PARAMETERS* params) {
    RunPipeline(self);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    return ((Fn)g_swapOrig[SLOT_Present1])(self, sync, flags, params);
}

static HRESULT STDMETHODCALLTYPE Detour_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h,
                                                     DXGI_FORMAT format, UINT flags) {
    Log("ResizeBuffers(%u, %u, %u) -> 释放 staging", count, w, h);
    WaitGpu();
    g_ctx.readback.Reset();
    g_ctx.upload.Reset();
    g_ctx.backbuffer.Reset();
    g_ctx.ready = false;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    return ((Fn)g_swapOrig[SLOT_ResizeBuffers])(self, count, w, h, format, flags);
}

// ================= 工厂钩子 =================
static void** g_factoryOrig = nullptr;
static void** g_factoryNew = nullptr;

static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                                       DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);
static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
    IDXGIOutput* restrict, IDXGISwapChain1** out);
static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict, IDXGISwapChain1** out);

static void HookFactory(IDXGIFactory* factory) {
    if (g_factoryNew) { SwapVptr(factory, g_factoryNew); return; }
    g_factoryOrig = CopyVtbl(factory);
    g_factoryNew = (void**)malloc(sizeof(void*) * kCopySlots);
    memcpy(g_factoryNew, g_factoryOrig, sizeof(void*) * kCopySlots);
    g_factoryNew[SLOT_CreateSwapChain] = (void*)&Detour_CreateSwapChain;
    g_factoryNew[SLOT_CreateSwapChainForHwnd] = (void*)&Detour_CreateSwapChainForHwnd;
    g_factoryNew[SLOT_CreateSwapChainForComposition] = (void*)&Detour_CreateSwapChainForComposition;
    SwapVptr(factory, g_factoryNew);
    Log("factory hooked (CreateSwapChain@%d ForHwnd@%d ForComposition@%d)",
        SLOT_CreateSwapChain, SLOT_CreateSwapChainForHwnd, SLOT_CreateSwapChainForComposition);
}

static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                                       DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    HRESULT hr = ((Fn)g_factoryOrig[SLOT_CreateSwapChain])(self, device, desc, out);
    Log("CreateSwapChain -> 0x%08X", (unsigned)hr);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
    IDXGIOutput* restrict, IDXGISwapChain1** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    HRESULT hr = ((Fn)g_factoryOrig[SLOT_CreateSwapChainForHwnd])(self, device, hwnd, desc, fs, restrict, out);
    Log("CreateSwapChainForHwnd -> 0x%08X  (%ux%u fmt=%d buffers=%u)",
        (unsigned)hr, desc ? desc->Width : 0, desc ? desc->Height : 0,
        desc ? (int)desc->Format : 0, desc ? desc->BufferCount : 0);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict, IDXGISwapChain1** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    HRESULT hr = ((Fn)g_factoryOrig[SLOT_CreateSwapChainForComposition])(self, device, desc, restrict, out);
    Log("CreateSwapChainForComposition -> 0x%08X", (unsigned)hr);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}

// ================= 导出：替换这两个，其余转发到 dxgi_orig =================
extern "C" {

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    LogInit();
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    Fn real = RealProc<Fn>("CreateDXGIFactory1");
    if (!real) return E_FAIL;
    HRESULT hr = real(riid, factory);
    Log("CreateDXGIFactory1 -> 0x%08X", (unsigned)hr);
    if (SUCCEEDED(hr) && factory && *factory) {
        ComPtr<IDXGIFactory2> f2;
        if (SUCCEEDED(((IUnknown*)*factory)->QueryInterface(IID_PPV_ARGS(&f2))) && f2)
            HookFactory(f2.Get());
        else
            HookFactory((IDXGIFactory*)*factory);
    }
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    LogInit();
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    Fn real = RealProc<Fn>("CreateDXGIFactory2");
    if (!real) return E_FAIL;
    HRESULT hr = real(flags, riid, factory);
    Log("CreateDXGIFactory2(flags=0x%X) -> 0x%08X", flags, (unsigned)hr);
    if (SUCCEEDED(hr) && factory && *factory) {
        ComPtr<IDXGIFactory2> f2;
        if (SUCCEEDED(((IUnknown*)*factory)->QueryInterface(IID_PPV_ARGS(&f2))) && f2)
            HookFactory(f2.Get());
        else
            HookFactory((IDXGIFactory*)*factory);
    }
    return hr;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LogInit();
        Log("dxgi proxy attached (invert=%d off=%d)", (int)g_invert, (int)g_off);
        InitForwarders();
    }
    return TRUE;
}
