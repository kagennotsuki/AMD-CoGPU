// dxgi.dll 代理 v3 —— 关键修正：Present 内绝不阻塞等待
//
// v2 的教训：在 Present 里 Signal 之后忙等 GPU 完成，会和游戏自己的队列时序
// 互相等待（游戏队列尾部常有"等 CPU 在 Present 后 signal 的 fence"），
// 结果是 TDR → DXGI_ERROR_DEVICE_HUNG。实测两次都是这个现象。
//
// v3 做法：两槽轮转 + 非阻塞收割
//   帧 N：把后备缓冲拷进 readback[slot]，Signal(fence, v)，立刻返回（不等）
//   帧 N+1：先检查 readback[other] 的 fence 是否已完成，完成就 Map 读走
// 这样 Present 内部没有任何等待，且天然形成"一帧延迟"，正好是流水线要的形状。
//
// 配置：游戏目录 crossgpu.ini
//   Log=1        写 crossgpu.log
//   Off=1        完全不挂钩
//   Interval=N   每 N 帧抓一次（默认 1）
//   MaxFrames=N  最多抓 N 帧（默认 0 = 不限）
//   DumpBin=1    抓到的帧写入 frame_<n>.bin（体积大，默认关）

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

static const int SLOT_CreateSwapChain = 10;
static const int SLOT_CreateSwapChainForHwnd = 15;
static const int SLOT_CreateSwapChainForComposition = 24;
static const int SLOT_Present = 8;
static const int SLOT_Present1 = 22;
static const int SLOT_ResizeBuffers = 13;
static const int kMaxSlot = 64;

static bool g_log = false;
static bool g_off = false;
static bool g_invert = false;
static bool g_dumpBin = false;
static int g_interval = 1;
static int g_maxFrames = 0;
static bool g_writeback = false;
static FILE* g_logFile = nullptr;

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

static char g_selfAnchor = 0;

static HMODULE SelfModule() {
    static HMODULE hm = nullptr;
    if (!hm)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&g_selfAnchor, &hm);
    return hm;
}

static std::wstring SelfDir() {
    wchar_t path[MAX_PATH]{};
    HMODULE hm = SelfModule();
    if (!hm || !GetModuleFileNameW(hm, path, MAX_PATH)) return std::wstring();
    std::wstring p(path);
    size_t s = p.find_last_of(L'\\');
    return (s == std::wstring::npos) ? std::wstring() : p.substr(0, s);
}

static void LogInit() {
    g_log = false; g_off = false; g_invert = false; g_dumpBin = false;
    g_interval = 1; g_maxFrames = 0;
    std::wstring dir = SelfDir();
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\crossgpu.ini").c_str(), L"r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            int v = atoi(eq + 1);
            if (_stricmp(line, "Log") == 0) g_log = v != 0;
            else if (_stricmp(line, "Off") == 0) g_off = v != 0;
            else if (_stricmp(line, "Invert") == 0) g_invert = v != 0;
            else if (_stricmp(line, "DumpBin") == 0) g_dumpBin = v != 0;
            else if (_stricmp(line, "Interval") == 0) g_interval = (v > 0) ? v : 1;
            else if (_stricmp(line, "MaxFrames") == 0) g_maxFrames = (v > 0) ? v : 0;
            else if (_stricmp(line, "WriteBack") == 0) g_writeback = v != 0;
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

static HMODULE RealDxgi() {
    static HMODULE m = nullptr;
    if (m) return m;
    std::wstring dir = SelfDir();
    m = LoadLibraryW((dir + L"\\dxgi_orig.dll").c_str());
    if (!m) m = LoadLibraryW(L"dxgi_orig.dll");
    Log("dxgi_orig -> %p (err %lu)", m, m ? 0 : GetLastError());
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

// ---------- 原地补丁 ----------
struct VtblRec { void** vtbl = nullptr; void* orig[kMaxSlot]{}; bool patched[kMaxSlot]{}; };
static std::vector<VtblRec*> g_recs;

static void* g_realPresent = nullptr;
static void* g_realPresent1 = nullptr;
static void* g_realResizeBuffers = nullptr;
static void* g_realCreateSwapChain = nullptr;
static void* g_realCreateSwapChainForHwnd = nullptr;
static void* g_realCreateSwapChainForComposition = nullptr;

static bool IsOurDetour(void* p);

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

static VtblRec* RecFor(void** vtbl) {
    for (VtblRec* r : g_recs) if (r->vtbl == vtbl) return r;
    VtblRec* r = new VtblRec();
    r->vtbl = vtbl;
    g_recs.push_back(r);
    return r;
}

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
    }
}

static void* OrigOf(int slot) {
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

static bool IsOurDetour(void* p) {
    return p == (void*)&Detour_Present || p == (void*)&Detour_Present1 ||
           p == (void*)&Detour_ResizeBuffers || p == (void*)&Detour_CreateSwapChain ||
           p == (void*)&Detour_CreateSwapChainForHwnd || p == (void*)&Detour_CreateSwapChainForComposition;
}

// ---------- 抓帧上下文（两槽轮转，Present 内不等待）----------
struct CapSlot {
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Resource> upload;      // 写回用（CPU 写入，GPU 拷到后备缓冲）
    void* uploadMapped = nullptr;
    UINT64 fenceValue = 0;
    bool pending = false;
    bool hasUpload = false;
};

struct FrameCtx {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceCounter = 0;

    ComPtr<ID3D12Resource> backbuffer;          // 兼容旧路径
    ComPtr<IDXGISwapChain> chain;               // 用于 GetBuffer(index)
    ComPtr<IDXGISwapChain3> sc3;                // 用于 GetCurrentBackBufferIndex
    ComPtr<ID3D12Resource> bbs[4];              // 按索引缓存后备缓冲
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT64 sliceBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};

    CapSlot slots[2];
    std::vector<uint8_t> host;

    bool ready = false;
    int captured = 0;
    int harvested = 0;
    int skipCounter = 0;
    bool loggedInit = false;
    double sumHarvestMs = 0;
    double sumCopyMs = 0;
};

static FrameCtx g_ctx;

// 合成交换链不抓（实测会 DEVICE_HUNG）
static std::vector<void*> g_compositionSwapChains;
static bool IsCompositionSwapChain(void* sc) {
    for (void* p : g_compositionSwapChains) if (p == sc) return true;
    return false;
}
static void MarkCompositionSwapChain(void* sc) {
    if (!IsCompositionSwapChain(sc)) g_compositionSwapChains.push_back(sc);
}

static ID3D12CommandQueue* FindGameQueue(ID3D12Device* dev) {
    std::wstring full = SelfDir() + L"\\d3d12.dll";
    HMODULE d3 = GetModuleHandleW(full.c_str());
    if (!d3) d3 = GetModuleHandleW(L"d3d12.dll");
    if (!d3) { Log("d3d12 proxy not loaded"); return nullptr; }
    auto fnCount = (int32_t(*)())GetProcAddress(d3, "CrossGpuQueueCount");
    auto fnGet = (void*(*)(int32_t))GetProcAddress(d3, "CrossGpuGetQueue");
    if (!fnCount || !fnGet) { Log("no CrossGpu exports in d3d12 proxy"); return nullptr; }
    int n = fnCount();
    Log("d3d12 proxy queues=%d", n);
    for (int i = 0; i < n; ++i) {
        ID3D12CommandQueue* q = (ID3D12CommandQueue*)fnGet(i);
        if (!q) continue;
        ID3D12Device* qd = nullptr;
        bool same = false;
        if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qd))) && qd) { same = (qd == dev); qd->Release(); }
        D3D12_COMMAND_QUEUE_DESC desc = q->GetDesc();
        if (same) Log("  queue #%d type=%d sameDevice=1", i, (int)desc.Type);
        if (same && desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) return q;
    }
    return nullptr;
}

static bool AcquireDevice(IDXGISwapChain* sc) {
    if (g_ctx.device && g_ctx.queue) return true;
    ComPtr<IUnknown> unk;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&unk))) || !unk) { Log("GetDevice failed"); return false; }
    if (!g_ctx.device) {
        ComPtr<ID3D12Device> d;
        if (SUCCEEDED(unk.As(&d)) && d) g_ctx.device = d;
        else {
            ComPtr<ID3D12CommandQueue> q;
            if (SUCCEEDED(unk.As(&q)) && q) {
                if (FAILED(q->GetDevice(IID_PPV_ARGS(&g_ctx.device)))) return false;
                g_ctx.queue = q;
            }
        }
    }
    if (!g_ctx.device) { Log("no D3D12 device from swapchain"); return false; }

    if (!g_ctx.queue) {
        ID3D12CommandQueue* q = FindGameQueue(g_ctx.device.Get());
        if (!q) { Log("no matching DIRECT queue -> skip"); return false; }
        g_ctx.queue = q;
        Log("using game queue");
    }
    if (!g_ctx.alloc) {
        if (FAILED(g_ctx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_ctx.alloc)))) return false;
        if (FAILED(g_ctx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ctx.alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&g_ctx.list)))) return false;
        g_ctx.list->Close();
        if (FAILED(g_ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_ctx.fence)))) return false;
    }
    if (!g_ctx.loggedInit) {
        Log("capture ctx ready");
        g_ctx.loggedInit = true;
    }
    return true;
}

static void ReleaseSlots() {
    for (auto& s : g_ctx.slots) { s.readback.Reset(); s.pending = false; s.fenceValue = 0; }
    for (auto& b : g_ctx.bbs) b.Reset();
    g_ctx.backbuffer.Reset();
    g_ctx.ready = false;
}

// 取「当前」后备缓冲（flip 模型下索引会轮转，写错槽会 DXGI_ERROR_DEVICE_HUNG）
static ID3D12Resource* CurrentBackBuffer(UINT& indexOut) {
    indexOut = 0;
    if (g_ctx.sc3) indexOut = g_ctx.sc3->GetCurrentBackBufferIndex();
    if (indexOut >= 4) indexOut = 0;
    if (!g_ctx.bbs[indexOut] && g_ctx.chain) {
        ComPtr<ID3D12Resource> r;
        if (SUCCEEDED(g_ctx.chain->GetBuffer(indexOut, IID_PPV_ARGS(&r))) && r)
            g_ctx.bbs[indexOut] = r;
    }
    return g_ctx.bbs[indexOut].Get();
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
            desc.Width = d0.BufferDesc.Width; desc.Height = d0.BufferDesc.Height; desc.Format = d0.BufferDesc.Format;
        }
    }
    if (g_ctx.ready && g_ctx.width == desc.Width && g_ctx.height == desc.Height && g_ctx.format == desc.Format)
        return true;

    ReleaseSlots();

    if (!g_ctx.chain) {
        ComPtr<IDXGISwapChain> chain;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&chain))) && chain) g_ctx.chain = chain;
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3) g_ctx.sc3 = sc3;
        Log("swapchain3 available=%d", g_ctx.sc3 ? 1 : 0);
    }

    UINT curIdx = 0;
    ComPtr<ID3D12Resource> bb;
    if (g_ctx.sc3) curIdx = g_ctx.sc3->GetCurrentBackBufferIndex();
    if (FAILED(sc->GetBuffer(curIdx, IID_PPV_ARGS(&bb)))) {
        Log("GetBuffer(%u) failed", curIdx);
        return false;
    }
    D3D12_RESOURCE_DESC rd = bb->GetDesc();
    g_ctx.backbuffer = bb;
    g_ctx.width = (UINT)rd.Width;
    g_ctx.height = rd.Height;
    g_ctx.format = rd.Format;

    D3D12_RESOURCE_DESC texDesc = rd;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    g_ctx.device->GetCopyableFootprints(&texDesc, 0, 1, 0, &g_ctx.fp, &rows, &rowSize, &total);
    g_ctx.sliceBytes = total;

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    rbHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    rbHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    rbHeap.CreationNodeMask = 1;
    rbHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(g_ctx.device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_ctx.slots[i].readback)))) {
            Log("readback slot %d failed", i);
            return false;
        }
    }
    if (g_writeback) {
        D3D12_HEAP_PROPERTIES upHeap = rbHeap;
        upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        for (int i = 0; i < 2; ++i) {
            if (FAILED(g_ctx.device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_ctx.slots[i].upload)))) {
                Log("upload slot %d failed", i);
                return false;
            }
            D3D12_RANGE nr{ 0, 0 };
            if (FAILED(g_ctx.slots[i].upload->Map(0, &nr, &g_ctx.slots[i].uploadMapped))) {
                Log("upload slot %d map failed", i);
                return false;
            }
        }
        Log("writeback enabled: 2 upload buffers (one-frame-late)");
    }

    g_ctx.ready = true;
    Log("capture buffers: %ux%u fmt=%d rowPitch=%u total=%llu (2 slots)", g_ctx.width, g_ctx.height,
        (int)g_ctx.format, g_ctx.fp.Footprint.RowPitch, (unsigned long long)total);
    return true;
}

// 非阻塞收割：把已完成的槽读走
static void Harvest() {
    for (int i = 0; i < 2; ++i) {
        CapSlot& s = g_ctx.slots[i];
        if (!s.pending || !s.readback) continue;
        if (g_ctx.fence->GetCompletedValue() < s.fenceValue) continue;

        auto t0 = Clock::now();
        D3D12_RANGE rr{ 0, (SIZE_T)g_ctx.sliceBytes };
        void* m = nullptr;
        if (FAILED(s.readback->Map(0, &rr, &m)) || !m) {
            Log("slot %d Map failed 0x%08X", i, (unsigned)g_ctx.device->GetDeviceRemovedReason());
            s.pending = false;
            return;
        }
        g_ctx.host.resize((size_t)g_ctx.sliceBytes);
        memcpy(g_ctx.host.data(), m, (size_t)g_ctx.sliceBytes);
        D3D12_RANGE nw{ 0, 0 };
        s.readback->Unmap(0, &nw);
        s.pending = false;

        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        g_ctx.harvested++;
        g_ctx.sumHarvestMs += ms;

        // 采样亮度，证明拿到的是真实画面
        const uint8_t* p = g_ctx.host.data();
        size_t step = (size_t)g_ctx.fp.Footprint.RowPitch;
        uint64_t sum = 0; size_t count = 0;
        for (size_t y = 0; y < g_ctx.height; y += 16) {
            const uint8_t* row = p + y * step;
            for (size_t x = 0; x < step; x += 64) { sum += row[x]; count++; }
        }
        double mean = count ? (double)sum / count : 0.0;

        if (g_writeback && s.uploadMapped) {
            // 一帧延迟：把这一帧（可选反色）写进上传缓冲，下一帧 Present 前贴回
            if (g_invert) for (size_t k = 0; k < g_ctx.host.size(); ++k) g_ctx.host[k] = (uint8_t)(255 - g_ctx.host[k]);
            memcpy(s.uploadMapped, g_ctx.host.data(), (size_t)g_ctx.sliceBytes);
            s.hasUpload = true;
        }

        if (g_dumpBin) {
            wchar_t name[64];
            swprintf_s(name, L"frame_%05d.bin", g_ctx.harvested);
            FILE* fp2 = nullptr;
            std::wstring path = SelfDir() + L"\\" + name;
            if (_wfopen_s(&fp2, path.c_str(), L"wb") == 0 && fp2) {
                fwrite(g_ctx.host.data(), 1, (size_t)g_ctx.sliceBytes, fp2);
                fclose(fp2);
            }
        }
        Log("harvest #%d: slot=%d map+memcpy %.2f ms, mean byte=%.1f, %ux%u",
            g_ctx.harvested, i, ms, mean, g_ctx.width, g_ctx.height);
    }
}

// 抓一帧：只提交，不等待
static void Capture(IDXGISwapChain* sc) {
    if (!AcquireDevice(sc) || !AcquireBuffers(sc)) return;

    int slot = g_ctx.captured % 2;
    CapSlot& s = g_ctx.slots[slot];
    if (s.pending) { Log("capture skipped: slot %d still pending", slot); return; }

    auto t0 = Clock::now();
    HRESULT hr = g_ctx.alloc->Reset();
    if (FAILED(hr)) { Log("alloc Reset 0x%08X", (unsigned)hr); return; }
    hr = g_ctx.list->Reset(g_ctx.alloc.Get(), nullptr);
    if (FAILED(hr)) { Log("list Reset 0x%08X", (unsigned)hr); return; }

    UINT bbIndex = 0;
    ID3D12Resource* bbRes = CurrentBackBuffer(bbIndex);
    if (!bbRes) { Log("no current backbuffer"); return; }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bbRes;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // ① 贴回上一帧的结果（若开启）
    if (g_writeback && s.hasUpload && s.upload) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_ctx.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION wdst{};
        wdst.pResource = bbRes;
        wdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        wdst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION wsrc{};
        wsrc.pResource = s.upload.Get();
        wsrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        wsrc.PlacedFootprint = g_ctx.fp;
        g_ctx.list->CopyTextureRegion(&wdst, 0, 0, 0, &wsrc, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_ctx.list->ResourceBarrier(1, &b);
        s.hasUpload = false;
    } else {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_ctx.list->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = s.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = g_ctx.fp;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = bbRes;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    g_ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_ctx.list->ResourceBarrier(1, &b);

    hr = g_ctx.list->Close();
    if (FAILED(hr)) { Log("list Close 0x%08X", (unsigned)hr); return; }
    ID3D12CommandList* l[] = { g_ctx.list.Get() };
    g_ctx.queue->ExecuteCommandLists(1, l);

    s.fenceValue = ++g_ctx.fenceCounter;
    hr = g_ctx.queue->Signal(g_ctx.fence.Get(), s.fenceValue);
    if (FAILED(hr)) { Log("Signal 0x%08X", (unsigned)hr); return; }
    s.pending = true;
    g_ctx.captured++;

    double submitMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    g_ctx.sumCopyMs += submitMs;
    if (g_ctx.captured <= 3 || g_ctx.captured % 60 == 0) {
        Log("capture #%d: slot=%d bbIndex=%u submit %.3f ms (no wait)", g_ctx.captured, slot, bbIndex, submitMs);
    }
}

static void RunPipeline(IDXGISwapChain* sc) {
    if (g_off) return;
    if (g_maxFrames > 0 && g_ctx.captured >= g_maxFrames) return;
    if (g_interval > 1 && (++g_ctx.skipCounter % g_interval) != 0) return;
    Harvest();          // 先收上一帧（非阻塞）
    Capture(sc);        // 再提交这一帧（不等待）
}

// ---------- 交换链/工厂钩子 ----------
static HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    if (!IsCompositionSwapChain(self)) RunPipeline(self);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    Fn orig = (Fn)OrigOf(SLOT_Present);
    return orig ? orig(self, sync, flags) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE Detour_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                                 const DXGI_PRESENT_PARAMETERS* params) {
    if (!IsCompositionSwapChain(self)) RunPipeline((IDXGISwapChain*)self);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    Fn orig = (Fn)OrigOf(SLOT_Present1);
    return orig ? orig(self, sync, flags, params) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE Detour_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h,
                                                      DXGI_FORMAT format, UINT flags) {
    Log("ResizeBuffers(%u,%u,%u) 释放抓帧缓冲", count, w, h);
    ReleaseSlots();
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    Fn orig = (Fn)OrigOf(SLOT_ResizeBuffers);
    return orig ? orig(self, count, w, h, format, flags) : E_FAIL;
}

static void PatchSwapChainVtables(IDXGISwapChain* sc) {
    auto patchOne = [](void* obj) {
        if (!obj) return;
        void** vtbl = *(void***)obj;
        PatchSlot(vtbl, SLOT_Present, (void*)&Detour_Present);
        PatchSlot(vtbl, SLOT_Present1, (void*)&Detour_Present1);
        PatchSlot(vtbl, SLOT_ResizeBuffers, (void*)&Detour_ResizeBuffers);
    };
    patchOne(sc);
    ComPtr<IDXGISwapChain1> s1; if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s1))) && s1) patchOne(s1.Get());
    ComPtr<IDXGISwapChain2> s2; if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s2))) && s2) patchOne(s2.Get());
    ComPtr<IDXGISwapChain3> s3; if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s3))) && s3) patchOne(s3.Get());
    ComPtr<IDXGISwapChain4> s4; if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s4))) && s4) patchOne(s4.Get());
}

static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChain(IDXGIFactory* self, IUnknown* device,
                                                        DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    Fn orig = (Fn)OrigOf(SLOT_CreateSwapChain);
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
    Fn orig = (Fn)OrigOf(SLOT_CreateSwapChainForHwnd);
    HRESULT hr = orig ? orig(self, device, hwnd, desc, fs, restrict, out) : E_FAIL;
    Log("CreateSwapChainForHwnd -> 0x%08X (%ux%u fmt=%d buffers=%u)", (unsigned)hr,
        desc ? desc->Width : 0, desc ? desc->Height : 0, desc ? (int)desc->Format : 0, desc ? desc->BufferCount : 0);
    if (SUCCEEDED(hr) && out && *out) PatchSwapChainVtables(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Detour_CreateSwapChainForComposition(IDXGIFactory2* self, IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* restrict, IDXGISwapChain1** out) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    Fn orig = (Fn)OrigOf(SLOT_CreateSwapChainForComposition);
    HRESULT hr = orig ? orig(self, device, desc, restrict, out) : E_FAIL;
    Log("CreateSwapChainForComposition -> 0x%08X (只标记，不抓帧)", (unsigned)hr);
    if (SUCCEEDED(hr) && out && *out) {
        MarkCompositionSwapChain(*out);
        PatchSwapChainVtables(*out);
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

extern "C" {

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    Fn real = RealProc<Fn>("CreateDXGIFactory1");
    if (!real) return E_FAIL;
    HRESULT hr = real(riid, factory);
    Log("CreateDXGIFactory1 -> 0x%08X", (unsigned)hr);
    if (SUCCEEDED(hr) && factory && *factory && !g_off) PatchFactoryVtables((IUnknown*)*factory);
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) {
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    Fn real = RealProc<Fn>("CreateDXGIFactory2");
    if (!real) return E_FAIL;
    HRESULT hr = real(flags, riid, factory);
    Log("CreateDXGIFactory2(0x%X) -> 0x%08X", flags, (unsigned)hr);
    if (SUCCEEDED(hr) && factory && *factory && !g_off) PatchFactoryVtables((IUnknown*)*factory);
    return hr;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LogInit();
        Log("dxgi proxy v3 attached (log=%d off=%d interval=%d maxFrames=%d)",
            (int)g_log, (int)g_off, g_interval, g_maxFrames);
        InitForwarders();
    }
    return TRUE;
}
