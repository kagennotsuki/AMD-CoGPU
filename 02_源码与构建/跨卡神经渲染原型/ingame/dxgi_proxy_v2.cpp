// dxgi.dll 代理 v2 —— 关键修正：原地补丁共享 vtable，而不是替换对象 vptr
//
// 为什么必须这样：REFramework（dinput8.dll）和 ReShade 一类工具都是通过
// 「改共享 vtable 里的 Present」来挂钩的。如果本代理把交换链对象的 vptr
// 指向自己复制的一份 vtable，游戏调用 Present 就不会再走 REFramework 的钩子，
// REFramework 的菜单/功能会整体失效（v1 的实测现象）。
//
// 原地补丁后，两边对同一张 vtable 逐个槽位替换，并且各自保存「替换前的原始指针」，
// 于是无论谁先谁后，钩子都能串起来。
//
// 配置：同目录 crossgpu.ini
//   Log=1     写 crossgpu.log
//   Invert=1  写回时反色（可视证据）
//   Off=1     完全不挂钩（安全旁路）

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

// 由 SDK 头文件解析得到的槽位
static const int SLOT_CreateSwapChain = 10;
static const int SLOT_CreateSwapChainForHwnd = 15;
static const int SLOT_CreateSwapChainForComposition = 24;
static const int SLOT_Present = 8;
static const int SLOT_Present1 = 22;
static const int SLOT_ResizeBuffers = 13;
static const int kMaxSlot = 64;

// 前置声明（全局原始指针逻辑要用到）
static HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* self, UINT sync, UINT flags);
static HRESULT STDMETHODCALLTYPE Detour_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                                const DXGI_PRESENT_PARAMETERS* params);
static HRESULT STDMETHODCALLTYPE Detour_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h,
                                                     DXGI_FORMAT format, UINT flags);
static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                                       DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out);
static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
    IDXGIOutput* restrict, IDXGISwapChain1** out);
static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict, IDXGISwapChain1** out);

static bool g_log = false;
static bool g_invert = false;
static bool g_off = false;
static bool g_writeback = true;
static int g_interval = 1;
static int g_maxFrames = 0;
static FILE* g_logFile = nullptr;

// ---------- 日志 ----------
static char g_selfAnchor = 0;   // 用它的地址定位本模块

static HMODULE SelfModule() {
    static HMODULE hm = nullptr;
    if (!hm) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&g_selfAnchor, &hm);
    }
    return hm;
}

static std::wstring SelfDir() {
    wchar_t path[MAX_PATH]{};
    HMODULE hm = SelfModule();
    if (!hm) return std::wstring();
    if (!GetModuleFileNameW(hm, path, MAX_PATH)) return std::wstring();
    std::wstring p(path);
    size_t s = p.find_last_of(L'\\');
    return (s == std::wstring::npos) ? std::wstring() : p.substr(0, s);
}

// 临时引导日志：写到 %TEMP%，用于定位"模块目录算错"这类问题
static void BootLog(const char* fmt, ...) {
    wchar_t tmp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    std::wstring p = std::wstring(tmp) + L"crossgpu_boot.log";
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static void LogInit() {
    g_log = false; g_invert = false; g_off = false;
    std::wstring dir = SelfDir();
    BootLog("SelfDir=%ls", dir.c_str());
    std::wstring ini = dir + L"\\crossgpu.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, ini.c_str(), L"r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            int v = atoi(eq + 1);
            if (_stricmp(line, "Log") == 0) g_log = v != 0;
            else if (_stricmp(line, "Invert") == 0) g_invert = v != 0;
            else if (_stricmp(line, "Off") == 0) g_off = v != 0;
            else if (_stricmp(line, "WriteBack") == 0) g_writeback = v != 0;
            else if (_stricmp(line, "Interval") == 0) g_interval = v > 0 ? v : 1;
            else if (_stricmp(line, "MaxFrames") == 0) g_maxFrames = v > 0 ? v : 0;
        }
        fclose(f);
    }
    if (g_log) _wfopen_s(&g_logFile, (dir + L"\\crossgpu.log").c_str(), L"w");
}

static void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

// ---------- 真实 dxgi ----------
static HMODULE RealDxgi() {
    static HMODULE m = nullptr;
    if (m) return m;
    std::wstring dir = SelfDir();
    std::wstring full = dir.empty() ? L"dxgi_orig.dll" : dir + L"\\dxgi_orig.dll";
    m = LoadLibraryW(full.c_str());
    DWORD e1 = m ? 0 : GetLastError();
    if (!m) { m = LoadLibraryW(L"dxgi_orig.dll"); }
    BootLog("RealDxgi: path=%ls handle=%p err=%lu", full.c_str(), m, e1);
    Log("dxgi_orig -> %p (err %lu)", m, e1);
    return m;
}

template <typename Fn> static Fn RealProc(const char* name) {
    HMODULE m = RealDxgi();
    return m ? (Fn)GetProcAddress(m, name) : nullptr;
}

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

// ---------- 原地补丁表 ----------
struct VtblRec {
    void** vtbl = nullptr;
    void* orig[kMaxSlot]{};
    bool patched[kMaxSlot]{};
};
static std::vector<VtblRec*> g_recs;

static VtblRec* RecFor(void** vtbl) {
    for (VtblRec* r : g_recs) if (r->vtbl == vtbl) return r;
    VtblRec* r = new VtblRec();
    r->vtbl = vtbl;
    g_recs.push_back(r);
    return r;
}

static void RememberReal(int slot, void* current);

static void PatchSlot(void** vtbl, int slot, void* detour) {
    VtblRec* r = RecFor(vtbl);
    if (!r->patched[slot]) {
        r->orig[slot] = vtbl[slot];
        r->patched[slot] = true;
        RememberReal(slot, vtbl[slot]);
    }
    DWORD old = 0;
    if (VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old)) {
        vtbl[slot] = detour;
        VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    } else {
        Log("VirtualProtect(vtbl slot %d) failed %lu", slot, GetLastError());
    }
}

// 全局保存的「真实函数」——不依赖对象当前 vptr，
// 这样即使别的注入器（REFramework 等）替换了对象 vptr，我们仍能调到真正的原函数
static void* g_realPresent = nullptr;
static void* g_realPresent1 = nullptr;
static void* g_realResizeBuffers = nullptr;
static void* g_realCreateSwapChain = nullptr;
static void* g_realCreateSwapChainForHwnd = nullptr;
static void* g_realCreateSwapChainForComposition = nullptr;

static bool IsOurDetour(void* p) {
    return p == (void*)&Detour_Present || p == (void*)&Detour_Present1 ||
           p == (void*)&Detour_ResizeBuffers || p == (void*)&Detour_CreateSwapChain ||
           p == (void*)&Detour_CreateSwapChainForHwnd || p == (void*)&Detour_CreateSwapChainForComposition;
}

static void RememberReal(int slot, void* current) {
    if (!current || IsOurDetour(current)) return;
    switch (slot) {
        case SLOT_Present: if (!g_realPresent) g_realPresent = current; break;
        case SLOT_Present1: if (!g_realPresent1) g_realPresent1 = current; break;
        case SLOT_ResizeBuffers: if (!g_realResizeBuffers) g_realResizeBuffers = current; break;
        case SLOT_CreateSwapChain: if (!g_realCreateSwapChain) g_realCreateSwapChain = current; break;
        case SLOT_CreateSwapChainForHwnd: if (!g_realCreateSwapChainForHwnd) g_realCreateSwapChainForHwnd = current; break;
        case SLOT_CreateSwapChainForComposition: if (!g_realCreateSwapChainForComposition) g_realCreateSwapChainForComposition = current; break;
        default: break;
    }
}

static void* OrigOf(void* /*self*/, int slot) {
    switch (slot) {
        case SLOT_Present: return g_realPresent;
        case SLOT_Present1: return g_realPresent1;
        case SLOT_ResizeBuffers: return g_realResizeBuffers;
        case SLOT_CreateSwapChain: return g_realCreateSwapChain;
        case SLOT_CreateSwapChainForHwnd: return g_realCreateSwapChainForHwnd;
        case SLOT_CreateSwapChainForComposition: return g_realCreateSwapChainForComposition;
        default: return nullptr;
    }
}

// ---------- 管线 ----------
struct FrameCtx {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fv = 0;
    ComPtr<ID3D12Resource> readback, upload, backbuffer;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT64 sliceBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    std::vector<uint8_t> host;
    bool ready = false;
    int frames = 0;
    double sumMs = 0, maxMs = 0;
    bool loggedInit = false;
};
static FrameCtx g_ctx;
static int g_skipCounter = 0;

// 合成交换链（CreateSwapChainForComposition）的后备缓冲不能按普通窗口交换链抓，
// 实测会导致 DXGI_ERROR_DEVICE_HUNG，因此只记录不接管。
static std::vector<void*> g_compositionSwapChains;
static bool IsCompositionSwapChain(void* sc) {
    for (void* p : g_compositionSwapChains) if (p == sc) return true;
    return false;
}
static void MarkCompositionSwapChain(void* sc) {
    if (!IsCompositionSwapChain(sc)) g_compositionSwapChains.push_back(sc);
}

static void WaitGpu() {
    if (!g_ctx.queue || !g_ctx.fence) return;
    g_ctx.queue->Signal(g_ctx.fence.Get(), ++g_ctx.fv);
    while (g_ctx.fence->GetCompletedValue() < g_ctx.fv) Sleep(0);
}

static bool AcquireDevice(IDXGISwapChain* sc) {
    if (g_ctx.device) return true;
    ComPtr<IUnknown> unk;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&unk))) || !unk) { Log("GetDevice failed"); return false; }

    // 两条路径都试：D3D12 交换链的 GetDevice 可能返回命令队列，也可能返回设备本身
    ComPtr<ID3D12CommandQueue> q;
    if (SUCCEEDED(unk.As(&q)) && q) {
        g_ctx.queue = q;
        if (FAILED(q->GetDevice(IID_PPV_ARGS(&g_ctx.device)))) { Log("queue->GetDevice failed"); return false; }
        Log("device via swapchain queue");
    } else {
        ComPtr<ID3D12Device> d;
        if (SUCCEEDED(unk.As(&d)) && d) {
            g_ctx.device = d;
            Log("device via swapchain device (no queue from GetDevice)");
        } else {
            Log("swapchain device is neither ID3D12CommandQueue nor ID3D12Device (D3D11 mode?)");
            return false;
        }
    }
    // 必须用「创建交换链的那个队列」。自建队列会 DXGI_ERROR_DEVICE_REMOVED（已实测）。
    if (!g_ctx.queue) {
        std::wstring full = SelfDir() + L"\\d3d12.dll";
        HMODULE d3 = GetModuleHandleW(full.c_str());
        if (!d3) d3 = GetModuleHandleW(L"d3d12.dll");
        if (!d3) { Log("d3d12 proxy not loaded -> skip pipeline"); return false; }
        auto fnCount = (int32_t(*)())GetProcAddress(d3, "CrossGpuQueueCount");
        auto fnGet = (void*(*)(int32_t))GetProcAddress(d3, "CrossGpuGetQueue");
        if (!fnCount || !fnGet) { Log("d3d12 proxy has no CrossGpu exports -> skip pipeline"); return false; }
        int n = fnCount();
        Log("d3d12 proxy queues=%d", n);
        for (int i = 0; i < n; ++i) {
            ID3D12CommandQueue* q = (ID3D12CommandQueue*)fnGet(i);
            if (!q) continue;
            ID3D12Device* qd = nullptr;
            bool same = false;
            if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qd))) && qd) {
                same = (qd == g_ctx.device.Get());
                qd->Release();
            }
            D3D12_COMMAND_QUEUE_DESC desc = q->GetDesc();
            Log("  queue #%d type=%d sameDevice=%d", i, (int)desc.Type, (int)same);
            if (same && desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { g_ctx.queue = q; break; }
        }
        if (!g_ctx.queue) {
            for (int i = 0; i < n && !g_ctx.queue; ++i) {
                ID3D12CommandQueue* q = (ID3D12CommandQueue*)fnGet(i);
                if (q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_ctx.queue = q;
            }
        }
        if (!g_ctx.queue) { Log("no DIRECT queue found -> skip pipeline"); return false; }
        Log("using game queue (type=DIRECT)");
    }
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
        Log("device: %ls (LUID %08X:%08X)", ad.Description, ad.AdapterLuid.HighPart, ad.AdapterLuid.LowPart);
        g_ctx.loggedInit = true;
    }
    return true;
}

static bool AcquireBuffers(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    {
        ComPtr<IDXGISwapChain1> sc1;
        bool got = false;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) got = SUCCEEDED(sc1->GetDesc1(&desc));
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

    g_ctx.readback.Reset(); g_ctx.upload.Reset(); g_ctx.backbuffer.Reset(); g_ctx.ready = false;

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

    D3D12_RESOURCE_DESC texDesc = rd;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // staging 用 BUFFER（READBACK 堆配纹理会被 AMD 驱动拒绝，实测 E_INVALIDARG）
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    g_ctx.device->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, &rows, &rowSize, &total);
    g_ctx.fp = fp;
    g_ctx.sliceBytes = total;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = total;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hrRb = g_ctx.device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_ctx.readback));
    if (FAILED(hrRb)) { Log("readback buffer create failed hr=0x%08X", (unsigned)hrRb); return false; }
    HRESULT hrUp = g_ctx.device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_ctx.upload));
    if (FAILED(hrUp)) { Log("upload buffer create failed hr=0x%08X", (unsigned)hrUp); return false; }

    g_ctx.host.resize((size_t)total);
    Log("staging buffer %ux%u fmt=%d rowPitch=%u total=%llu", g_ctx.width, g_ctx.height, (int)g_ctx.format,
        fp.Footprint.RowPitch, (unsigned long long)total);
    g_ctx.ready = true;
    return true;
}

static void RunPipeline(IDXGISwapChain* sc) {
    if (g_off) return;
    if (g_maxFrames > 0 && g_ctx.frames >= g_maxFrames) return;
    if (g_interval > 1 && (++g_skipCounter % g_interval) != 0) return;
    if (!AcquireDevice(sc) || !AcquireBuffers(sc)) return;
    auto t0 = Clock::now();

    g_ctx.alloc->Reset();
    g_ctx.list->Reset(g_ctx.alloc.Get(), nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_ctx.backbuffer.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_ctx.list->ResourceBarrier(1, &b);
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_ctx.readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = g_ctx.fp;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = g_ctx.backbuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        g_ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_ctx.list->ResourceBarrier(1, &b);
    g_ctx.list->Close();
    { ID3D12CommandList* l[] = { g_ctx.list.Get() }; g_ctx.queue->ExecuteCommandLists(1, l); }
    WaitGpu();

    {
        HRESULT rr = g_ctx.device->GetDeviceRemovedReason();
        if (FAILED(rr)) {
            Log("DEVICE REMOVED after copy-out: 0x%08X", (unsigned)rr);

            return;
        }
    }

    {
        D3D12_RANGE rr{ 0, (SIZE_T)g_ctx.sliceBytes };
        void* m = nullptr;
        HRESULT hrMap = g_ctx.readback->Map(0, &rr, &m);
        if (FAILED(hrMap) || !m) { Log("readback Map failed hr=0x%08X m=%p", (unsigned)hrMap, m); return; }
        memcpy(g_ctx.host.data(), m, (size_t)g_ctx.sliceBytes);
        D3D12_RANGE nw{ 0, 0 };
        g_ctx.readback->Unmap(0, &nw);
    }
    if (g_invert) for (size_t i = 0; i < g_ctx.host.size(); ++i) g_ctx.host[i] = (uint8_t)(255 - g_ctx.host[i]);

    {
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        HRESULT hrMap2 = g_ctx.upload->Map(0, &nr, &m);
        if (FAILED(hrMap2) || !m) { Log("upload Map failed hr=0x%08X m=%p", (unsigned)hrMap2, m); return; }
        memcpy(m, g_ctx.host.data(), (size_t)g_ctx.sliceBytes);
        D3D12_RANGE w{ 0, (SIZE_T)g_ctx.sliceBytes };
        g_ctx.upload->Unmap(0, &w);
    }

    if (g_writeback) {
    g_ctx.alloc->Reset();
    g_ctx.list->Reset(g_ctx.alloc.Get(), nullptr);
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_ctx.backbuffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = g_ctx.upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = g_ctx.fp;
        g_ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_ctx.list->ResourceBarrier(1, &b);
    g_ctx.list->Close();
    { ID3D12CommandList* l[] = { g_ctx.list.Get() }; g_ctx.queue->ExecuteCommandLists(1, l); }
    WaitGpu();
    }   // g_writeback
    else {
        // 只抓不写：仍需把后备缓冲恢复到 PRESENT
        g_ctx.alloc->Reset();
        g_ctx.list->Reset(g_ctx.alloc.Get(), nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_ctx.list->ResourceBarrier(1, &b);
        g_ctx.list->Close();
        { ID3D12CommandList* l[] = { g_ctx.list.Get() }; g_ctx.queue->ExecuteCommandLists(1, l); }
        WaitGpu();
    }

    auto t1 = Clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    g_ctx.frames++;
    g_ctx.sumMs += ms;
    if (ms > g_ctx.maxMs) g_ctx.maxMs = ms;
    if (g_ctx.frames % 120 == 0)
        Log("frames=%d avg=%.2f ms max=%.2f ms (%ux%u fmt=%d)", g_ctx.frames,
            g_ctx.sumMs / g_ctx.frames, g_ctx.maxMs, g_ctx.width, g_ctx.height, (int)g_ctx.format);
}

// ---------- 交换链钩子 ----------
static HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    if (!IsCompositionSwapChain(self)) RunPipeline(self);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    Fn orig = (Fn)OrigOf(self, SLOT_Present);
    return orig ? orig(self, sync, flags) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE Detour_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                                 const DXGI_PRESENT_PARAMETERS* params) {
    if (!IsCompositionSwapChain(self)) RunPipeline((IDXGISwapChain*)self);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    Fn orig = (Fn)OrigOf(self, SLOT_Present1);
    return orig ? orig(self, sync, flags, params) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE Detour_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h,
                                                      DXGI_FORMAT format, UINT flags) {
    Log("ResizeBuffers(%u,%u,%u) 释放 staging", count, w, h);
    WaitGpu();
    g_ctx.readback.Reset(); g_ctx.upload.Reset(); g_ctx.backbuffer.Reset(); g_ctx.ready = false;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    Fn orig = (Fn)OrigOf(self, SLOT_ResizeBuffers);
    return orig ? orig(self, count, w, h, format, flags) : E_FAIL;
}

static void PatchSwapChainVtables(IDXGISwapChain* sc) {
    // 交换链对象可能被 QI 成多个接口，每个接口各有自己的 vtable；逐个补丁
    auto patchOne = [](void* obj) {
        if (!obj) return;
        void** vtbl = *(void***)obj;
        PatchSlot(vtbl, SLOT_Present, (void*)&Detour_Present);
        PatchSlot(vtbl, SLOT_Present1, (void*)&Detour_Present1);
        PatchSlot(vtbl, SLOT_ResizeBuffers, (void*)&Detour_ResizeBuffers);
    };
    patchOne(sc);
    ComPtr<IDXGISwapChain1> sc1; if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) patchOne(sc1.Get());
    ComPtr<IDXGISwapChain2> sc2; if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc2))) && sc2) patchOne(sc2.Get());
    ComPtr<IDXGISwapChain3> sc3; if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) patchOne(sc3.Get());
    ComPtr<IDXGISwapChain4> sc4; if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc4))) && sc4) patchOne(sc4.Get());
    Log("swapchain vtables patched (%zu 张 vtable 已记录)", g_recs.size());
}

// ---------- 工厂钩子 ----------
static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                                        DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    Fn orig = (Fn)OrigOf(self, SLOT_CreateSwapChain);
    HRESULT hr = orig ? orig(self, device, desc, out) : E_FAIL;
    Log("CreateSwapChain -> 0x%08X", (unsigned)hr);
    if (SUCCEEDED(hr) && out && *out) PatchSwapChainVtables(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
    IDXGIOutput* restrict, IDXGISwapChain1** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    Fn orig = (Fn)OrigOf(self, SLOT_CreateSwapChainForHwnd);
    HRESULT hr = orig ? orig(self, device, hwnd, desc, fs, restrict, out) : E_FAIL;
    Log("CreateSwapChainForHwnd -> 0x%08X (%ux%u fmt=%d buffers=%u)", (unsigned)hr,
        desc ? desc->Width : 0, desc ? desc->Height : 0, desc ? (int)desc->Format : 0,
        desc ? desc->BufferCount : 0);
    if (SUCCEEDED(hr) && out && *out) PatchSwapChainVtables(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict, IDXGISwapChain1** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    Fn orig = (Fn)OrigOf(self, SLOT_CreateSwapChainForComposition);
    HRESULT hr = orig ? orig(self, device, desc, restrict, out) : E_FAIL;
    Log("CreateSwapChainForComposition -> 0x%08X  (跳过：合成交换链不抓)", (unsigned)hr);
    if (SUCCEEDED(hr) && out && *out) {
        MarkCompositionSwapChain(*out);
        PatchSwapChainVtables(*out);   // 仍装钩子，但 Present 时会跳过抓帧
    }
    return hr;
}

static void PatchFactoryVtables(IUnknown* factoryUnk) {
    auto patchOne = [](void* obj) {
        if (!obj) return;
        void** vtbl = *(void***)obj;
        PatchSlot(vtbl, SLOT_CreateSwapChain, (void*)&Detour_CreateSwapChain);
        PatchSlot(vtbl, SLOT_CreateSwapChainForHwnd, (void*)&Detour_CreateSwapChainForHwnd);
        PatchSlot(vtbl, SLOT_CreateSwapChainForComposition, (void*)&Detour_CreateSwapChainForComposition);
    };
    patchOne(factoryUnk);
    ComPtr<IDXGIFactory> f;   if (SUCCEEDED(factoryUnk->QueryInterface(IID_PPV_ARGS(&f))) && f) patchOne(f.Get());
    ComPtr<IDXGIFactory1> f1; if (SUCCEEDED(factoryUnk->QueryInterface(IID_PPV_ARGS(&f1))) && f1) patchOne(f1.Get());
    ComPtr<IDXGIFactory2> f2; if (SUCCEEDED(factoryUnk->QueryInterface(IID_PPV_ARGS(&f2))) && f2) patchOne(f2.Get());
    ComPtr<IDXGIFactory3> f3; if (SUCCEEDED(factoryUnk->QueryInterface(IID_PPV_ARGS(&f3))) && f3) patchOne(f3.Get());
    ComPtr<IDXGIFactory4> f4; if (SUCCEEDED(factoryUnk->QueryInterface(IID_PPV_ARGS(&f4))) && f4) patchOne(f4.Get());
    ComPtr<IDXGIFactory5> f5; if (SUCCEEDED(factoryUnk->QueryInterface(IID_PPV_ARGS(&f5))) && f5) patchOne(f5.Get());
    ComPtr<IDXGIFactory6> f6; if (SUCCEEDED(factoryUnk->QueryInterface(IID_PPV_ARGS(&f6))) && f6) patchOne(f6.Get());
    ComPtr<IDXGIFactory7> f7; if (SUCCEEDED(factoryUnk->QueryInterface(IID_PPV_ARGS(&f7))) && f7) patchOne(f7.Get());
}

// ---------- 导出 ----------
extern "C" {

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    Fn real = RealProc<Fn>("CreateDXGIFactory1");
    if (!real) return E_FAIL;
    HRESULT hr = real(riid, factory);
    Log("CreateDXGIFactory1 -> 0x%08X (off=%d)", (unsigned)hr, (int)g_off);
    if (SUCCEEDED(hr) && factory && *factory && !g_off) PatchFactoryVtables((IUnknown*)*factory);
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    Fn real = RealProc<Fn>("CreateDXGIFactory2");
    if (!real) return E_FAIL;
    HRESULT hr = real(flags, riid, factory);
    Log("CreateDXGIFactory2(0x%X) -> 0x%08X (off=%d)", flags, (unsigned)hr, (int)g_off);
    if (SUCCEEDED(hr) && factory && *factory && !g_off) PatchFactoryVtables((IUnknown*)*factory);
    return hr;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LogInit();
        Log("dxgi proxy v2 attached (log=%d invert=%d off=%d)", (int)g_log, (int)g_invert, (int)g_off);
        InitForwarders();
    }
    return TRUE;
}
