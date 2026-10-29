// dxgi.dll 代理 v4 —— 跨卡神经渲染的真正集成
// dxgi.dll 代理 v4 —— 跨卡神经渲染的真正集成
//
// 架构：
//   Present 线程（绝不阻塞）
//     1) 若后台已产出结果 → 记一次"贴回"（1080p 结果 → 放大 → 后备缓冲）
//     2) 抓当前后备缓冲 → 4K 下采样到 1080p → 拷进回读缓冲 → Signal
//     3) 若上一帧回读已完成 → Map + memcpy → 交给工作线程（非阻塞）
//   工作线程
//     把 1080p 帧传到 9070 XT → 跑 lmxxf → 回读结果 → 存为"最新结果"
//
// 这样 Present 内只有 GPU 提交（数十微秒）和一次可能的 memcpy（约 1.4ms），
// 推理的 ~10ms 完全藏在后台，临界路径由推理决定。
//
// 配置（游戏目录 crossgpu.ini）：
//   Log=1       写 crossgpu.log
//   Off=1       完全不挂钩
//   EnableNR=1  启用 9070 XT 推理（0 = 只做 4K↔1080 缩放直通，便于排错）
//   Invert=1    反色（调试用）

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "LmxxfNrApi.h"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

// ---------- 与运行时匹配的 ABI（160 字节 / 19 个函数指针）----------
struct ApiTable {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (*QueryCapabilities)(LmxxfNrCapabilities*);
    int32_t (*Create)(const LmxxfNrCreateInfo*, void**);
    int32_t (*Destroy)(void*);
    int32_t (*PrepareSession)(void*);
    int32_t (*PrepareFrame)(void*, const LmxxfNrFrameInfo*, LmxxfNrJob*);
    int32_t (*RecordInputs)(void*, void*, void*);
    int32_t (*EnqueueHip)(void*, void*, void*);
    int32_t (*RecordOutputs)(void*, void*, void*);
    int32_t (*ExecuteAfterProducer)(void*, void*, void*);
    int32_t (*CancelUnsubmitted)(void*, void*);
    int32_t (*Poll)(void*, void*, uint32_t*);
    int32_t (*Retire)(void*, void*);
    int32_t (*ResetHistory)(void*);
    int32_t (*Drain)(void*);
    int32_t (*GetStatus)(void*, char*, uint32_t);
    int32_t (*GetLastError)(char*, uint32_t);
    int32_t (*GetTimings)(void*, LmxxfNrTimings*);
    void* extra0;
    void* extra1;
};

struct FrameInfo120 {
    LmxxfNrFrameInfo base;
    uint64_t spare0;
    uint64_t spare1;
};
static_assert(sizeof(FrameInfo120) == 120, "frame info must be 120 bytes");

// ---------- 着色器 ----------
static const char* kHLSL = R"(
Texture2D<float4>   gSrc : register(t0);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);

[numthreads(8, 8, 1)]
void DownscaleCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint2 s = id.xy * 2;
    float4 c = gSrc.Load(int3(s.x,     s.y,     0))
             + gSrc.Load(int3(s.x + 1, s.y,     0))
             + gSrc.Load(int3(s.x,     s.y + 1, 0))
             + gSrc.Load(int3(s.x + 1, s.y + 1, 0));
    gDst[id.xy] = c * 0.25;
}

[numthreads(8, 8, 1)]
void UpscaleCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    // 双线性放大（原来是最近邻，导致块状与锯齿）
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    gDst[id.xy] = gSrc.SampleLevel(gSmp, uv, 0);
}
)";

// ---------- 全局 ----------
static const int SLOT_CreateSwapChain = 10;
static const int SLOT_CreateSwapChainForHwnd = 15;
static const int SLOT_CreateSwapChainForComposition = 24;
static const int SLOT_Present = 8;
static const int SLOT_Present1 = 22;
static const int SLOT_ResizeBuffers = 13;
static const int kMaxSlot = 64;

static bool g_log = false;
static bool g_off = false;
static bool g_enableNR = true;
static bool g_invert = false;
static int g_interval = 1;
static FILE* g_logFile = nullptr;
static HWND g_targetHwnd = nullptr;   // 只接管这一个窗口的交换链

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
    g_log = false; g_off = false; g_enableNR = true; g_invert = false; g_interval = 1;
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
            else if (_stricmp(line, "EnableNR") == 0) g_enableNR = v != 0;
            else if (_stricmp(line, "Invert") == 0) g_invert = v != 0;
            else if (_stricmp(line, "Interval") == 0) g_interval = (v > 0) ? v : 1;
        }
        fclose(f);
    }
    if (g_log) g_logFile = _wfsopen((dir + L"\\crossgpu.log").c_str(), L"w", _SH_DENYNO);
}

static Clock::time_point g_t0 = Clock::now();
static void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - g_t0).count();
    fprintf(g_logFile, "[%9.1f ms] ", ms);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

static volatile bool g_nrOn = true;      // F10 实时开关（A/B 对比用）
static bool g_hotkeyStarted = false;

static DWORD WINAPI HotkeyThread(LPVOID) {
    bool prev = false;
    while (true) {
        bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (down && !prev) {
            g_nrOn = !g_nrOn;
            Log("*** F10: NR %s ***", g_nrOn ? "ON（应用 NR 结果）" : "OFF（原样显示游戏画面）");
        }
        prev = down;
        Sleep(40);
    }
    return 0;
}

// ---------- 真实 dxgi / 转发 ----------
static HMODULE RealDxgi() {
    static HMODULE m = nullptr;
    if (m) return m;
    std::wstring dir = SelfDir();
    m = LoadLibraryW((dir + L"\\dxgi_orig.dll").c_str());
    if (!m) m = LoadLibraryW(L"dxgi_orig.dll");
    return m;
}

template <typename Fn> static Fn RealProc(const char* name) {
    HMODULE m = RealDxgi();
    return m ? (Fn)GetProcAddress(m, name) : nullptr;
}

// 从真实的 d3d12_orig.dll 取函数（不要走我们自己的 d3d12 代理，避免自引用）
static HMODULE RealD3D12() {
    static HMODULE m = nullptr;
    if (m) return m;
    std::wstring dir = SelfDir();
    m = LoadLibraryW((dir + L"\\d3d12_orig.dll").c_str());
    if (!m) m = LoadLibraryW(L"d3d12_orig.dll");
    return m;
}

template <typename Fn> static Fn D3D12Proc(const char* name) {
    HMODULE m = RealD3D12();
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

// ---------- 工作线程：9070 XT 侧 ----------
struct Worker {
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> inBuf;     // 待处理（1080p）
    std::vector<uint8_t> outBuf;    // 最新结果（1080p）
    bool hasIn = false;
    bool hasOut = false;
    bool quit = false;
    int processed = 0;
    double lastMs = 0;
    std::atomic<int> pending{ 0 };

    // 9070 XT 侧资源
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fe;
    UINT64 fv = 0;
    ComPtr<ID3D12Resource> color, up, rb;
    HMODULE mod = nullptr;
    ApiTable api{};
    void* ctx = nullptr;
    UINT64 bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT w = 0, h = 0;
    bool ok = false;

    // 9070 XT 自己出画：合成交换链 + DComp overlay
    HWND hwnd = nullptr;
    UINT reqW = 0, reqH = 0;      // 代理每帧更新的客户端尺寸
    UINT pw = 0, ph = 0;
    ComPtr<IDCompositionDevice> dc;
    ComPtr<IDCompositionTarget> dtarget;
    ComPtr<IDCompositionVisual> dvisual;
    ComPtr<IDXGISwapChain3> psc;
    ComPtr<ID3D12Resource> pbb[3];
    ComPtr<ID3D12DescriptorHeap> prtv;
    UINT prtvInc = 0;
    ComPtr<ID3D12Resource> bigOut;
    D3D12_RESOURCE_STATES bigOutState = D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12RootSignature> prs;
    ComPtr<ID3D12PipelineState> psoUp;
    ComPtr<ID3D12DescriptorHeap> pheap;
};

static Worker g_worker;

static bool WorkerInit(UINT w, UINT h) {
    if (g_worker.ok && g_worker.w == w && g_worker.h == h) return true;
    if (g_worker.ok) { Log("worker: size changed %ux%u -> %ux%u, rebuild", g_worker.w, g_worker.h, w, h); }
    Worker& W = g_worker;
    W.w = w; W.h = h;
    W.ok = false;
    W.color.Reset(); W.up.Reset(); W.rb.Reset(); W.cl.Reset(); W.al.Reset(); W.fe.Reset(); W.q.Reset(); W.dev.Reset();

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> ad;
    if (factory->EnumAdapters1(1, &ad) != S_OK) { Log("worker: adapter 1 missing"); return false; }
    using CreateDevFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto createDev = D3D12Proc<CreateDevFn>("D3D12CreateDevice");
    if (!createDev) { Log("worker: no D3D12CreateDevice"); return false; }
    {
        DXGI_ADAPTER_DESC adesc{};
        if (SUCCEEDED(ad->GetDesc(&adesc))) Log("worker: adapter = %ls (LUID %08X:%08X)",
            adesc.Description, adesc.AdapterLuid.HighPart, adesc.AdapterLuid.LowPart);
    }
    if (FAILED(createDev(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&W.dev)))) { Log("worker: device failed"); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (FAILED(W.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&W.q)))) return false;
    if (FAILED(W.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&W.al)))) return false;
    if (FAILED(W.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, W.al.Get(), nullptr, IID_PPV_ARGS(&W.cl)))) return false;
    W.cl->Close();
    if (FAILED(W.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&W.fe)))) return false;

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1;
    dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(W.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&W.color)))) { Log("worker: color failed"); return false; }

    D3D12_RESOURCE_DESC sd = td;
    sd.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    W.dev->GetCopyableFootprints(&sd, 0, 1, 0, &W.fp, &rows, &rowSize, &total);
    W.bytes = total;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&W.up)))) return false;
    if (FAILED(W.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&W.rb)))) return false;

    W.inBuf.assign((size_t)total, 0);
    W.outBuf.assign((size_t)total, 0);
    {
        D3D12_RANGE rr{ 0, (SIZE_T)total };
        void* m = nullptr;
        if (FAILED(W.rb->Map(0, &rr, &m))) { Log("worker: rb map failed"); return false; }
        W.rb->Unmap(0, nullptr);
    }

    // lmxxf 运行时
    if (g_enableNR) {
        const wchar_t* kAssets =
            L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";
        std::wstring dllPath(kAssets);
        dllPath = dllPath.substr(0, dllPath.find_last_of(L'\\')) + L"\\LmxxfNrRuntime.dll";
        W.mod = LoadLibraryW(dllPath.c_str());
        if (!W.mod) { Log("worker: lmxxf load failed %lu", GetLastError()); return false; }
        using GetApiFn = int32_t(*)(uint32_t, void*);
        auto getApi = (GetApiFn)GetProcAddress(W.mod, "LmxxfNrGetApi");
        W.api.struct_size = (uint32_t)sizeof(ApiTable);
        if (!getApi || getApi(1, &W.api) != 0) { Log("worker: GetApi failed"); return false; }
        LmxxfNrCreateInfo ci{};
        ci.struct_size = (uint32_t)sizeof(ci);
        ci.device = W.dev.Get();
        ci.queue = W.q.Get();
        ci.assets_directory = kAssets;
        if (W.api.Create(&ci, &W.ctx) != 0) { Log("worker: Create failed"); return false; }
        if (W.api.PrepareSession(W.ctx) != 0) { Log("worker: PrepareSession failed"); return false; }
        Log("worker: lmxxf session ready (%ux%u)", w, h);
    } else {
        Log("worker: NR disabled, 只做缩放直通");
    }

    W.ok = true;
    Log("worker: ready %ux%u, bytes=%llu rowPitch=%u", w, h, (unsigned long long)total, W.fp.Footprint.RowPitch);
    return true;
}

// 工作线程主体
// ---------- 9070 XT 侧：自己出画 ----------
// 合成交换链（CreateSwapChainForComposition，不绑 HWND）通过 DirectComposition
// 盖在游戏窗口上。这样完全不需要写进游戏的后备缓冲，也不用把结果传回渲染卡。
static const char* kPresentHLSL = R"(
Texture2D<float4>   gSrc : register(t0);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);

[numthreads(8, 8, 1)]
void UpCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    gDst[id.xy] = gSrc.SampleLevel(gSmp, uv, 0);
}
)";

static bool WorkerPresentInit(UINT w, UINT h) {
    Worker& W = g_worker;
    if (!W.hwnd || !w || !h) return false;
    if (W.psc && W.pw == w && W.ph == h) return true;

    W.psc.Reset(); W.bigOut.Reset(); W.prs.Reset(); W.psoUp.Reset(); W.pheap.Reset(); W.prtv.Reset();
    for (auto& b : W.pbb) b.Reset();
    W.bigOutState = D3D12_RESOURCE_STATE_COMMON;

    if (!W.dc) {
        if (FAILED(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&W.dc)))) { Log("worker: DCompositionCreateDevice 失败"); return false; }
        if (FAILED(W.dc->CreateTargetForHwnd(W.hwnd, TRUE, &W.dtarget))) { Log("worker: CreateTargetForHwnd 失败"); return false; }
        if (FAILED(W.dc->CreateVisual(&W.dvisual))) { Log("worker: CreateVisual 失败"); return false; }
        if (FAILED(W.dtarget->SetRoot(W.dvisual.Get()))) { Log("worker: SetRoot 失败"); return false; }
    }

    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) return false;
    DXGI_SWAP_CHAIN_DESC1 cd{};
    cd.Width = w; cd.Height = h; cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cd.SampleDesc.Count = 1; cd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    cd.BufferCount = 3; cd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    cd.AlphaMode = DXGI_ALPHA_MODE_IGNORE; cd.Scaling = DXGI_SCALING_STRETCH;
    ComPtr<IDXGISwapChain1> s1;
    HRESULT hr = fac->CreateSwapChainForComposition(W.q.Get(), &cd, nullptr, &s1);
    if (FAILED(hr)) { Log("worker: 合成交换链失败 0x%08X", (unsigned)hr); return false; }
    s1.As(&W.psc);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 3;
    if (FAILED(W.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&W.prtv)))) return false;
    W.prtvInc = W.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 3; ++i) {
        if (FAILED(W.psc->GetBuffer(i, IID_PPV_ARGS(&W.pbb[i])))) return false;
        D3D12_CPU_DESCRIPTOR_HANDLE hh = W.prtv->GetCPUDescriptorHandleForHeapStart();
        hh.ptr += (SIZE_T)i * W.prtvInc;
        W.dev->CreateRenderTargetView(W.pbb[i].Get(), nullptr, hh);
    }

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(W.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&W.bigOut)))) {
        Log("worker: bigOut 创建失败"); return false;
    }

    D3D12_DESCRIPTOR_RANGE srvR{}, uavR{};
    srvR.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srvR.NumDescriptors = 1; srvR.BaseShaderRegister = 0;
    uavR.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavR.NumDescriptors = 1; uavR.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &srvR;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &uavR;
    D3D12_STATIC_SAMPLER_DESC sm{};
    sm.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sm.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sm.MaxLOD = D3D12_FLOAT32_MAX;
    sm.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &sm;
    ComPtr<ID3DBlob> sig, err;
    using SerializeFn2 = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto serialize2 = D3D12Proc<SerializeFn2>("D3D12SerializeRootSignature");
    if (!serialize2 || FAILED(serialize2(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { Log("worker: 根签名失败"); return false; }
    if (FAILED(W.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&W.prs)))) return false;
    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kPresentHLSL, strlen(kPresentHLSL), "p.hlsl", nullptr, nullptr, "UpCS", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) { Log("worker: 放大 shader 编译失败"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = W.prs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(W.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&W.psoUp)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC shd{};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shd.NumDescriptors = 2;
    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(W.dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&W.pheap)))) return false;
    UINT di = W.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c0 = W.pheap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd2{};
    sd2.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd2.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd2.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd2.Texture2D.MipLevels = 1;
    W.dev->CreateShaderResourceView(W.color.Get(), &sd2, c0);
    D3D12_CPU_DESCRIPTOR_HANDLE c1 = c0; c1.ptr += di;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    W.dev->CreateUnorderedAccessView(W.bigOut.Get(), nullptr, &ud, c1);

    if (FAILED(W.dvisual->SetContent(W.psc.Get()))) { Log("worker: SetContent 失败"); return false; }
    if (FAILED(W.dc->Commit())) { Log("worker: Commit 失败"); return false; }
    W.pw = w; W.ph = h;
    Log("worker: 呈现层就绪 %ux%u（合成交换链 + DComp overlay）", w, h);
    return true;
}

static void WorkerPresentFrame() {
    Worker& W = g_worker;
    if (!W.psc || !W.bigOut) return;
    UINT idx = W.psc->GetCurrentBackBufferIndex();
    W.al->Reset();
    W.cl->Reset(W.al.Get(), nullptr);

    D3D12_RESOURCE_BARRIER b0{};
    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b0.Transition.pResource = W.bigOut.Get();
    b0.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b0.Transition.StateBefore = W.bigOutState;
    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    W.cl->ResourceBarrier(1, &b0);

    ID3D12DescriptorHeap* hs[] = { W.pheap.Get() };
    W.cl->SetDescriptorHeaps(1, hs);
    W.cl->SetComputeRootSignature(W.prs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE g0 = W.pheap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE g1 = g0;
    g1.ptr += W.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    W.cl->SetComputeRootDescriptorTable(0, g0);
    W.cl->SetComputeRootDescriptorTable(1, g1);
    W.cl->SetPipelineState(W.psoUp.Get());
    W.cl->Dispatch((W.pw + 7) / 8, (W.ph + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition.pResource = W.bigOut.Get();
    b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    W.cl->ResourceBarrier(1, &b1);
    W.bigOutState = D3D12_RESOURCE_STATE_COPY_SOURCE;

    D3D12_RESOURCE_BARRIER b2{};
    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2.Transition.pResource = W.pbb[idx].Get();
    b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    W.cl->ResourceBarrier(1, &b2);

    D3D12_TEXTURE_COPY_LOCATION dd{};
    dd.pResource = W.pbb[idx].Get();
    dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dd.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION ss{};
    ss.pResource = W.bigOut.Get();
    ss.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    ss.SubresourceIndex = 0;
    W.cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);

    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    W.cl->ResourceBarrier(1, &b2);
    W.cl->Close();
    { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
    W.q->Signal(W.fe.Get(), ++W.fv);
    while (W.fe->GetCompletedValue() < W.fv) Sleep(0);
    W.psc->Present(0, 0);
}

static void WorkerThreadMain() {
    Worker& W = g_worker;
    std::vector<uint8_t> local;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(W.m);
            W.cv.wait(lk, [&] { return W.quit || W.hasIn; });
            if (W.quit) break;
            local = W.inBuf;
            W.hasIn = false;
            W.pending = 0;
        }
        if (g_invert) for (size_t i = 0; i < local.size(); ++i) local[i] = (uint8_t)(255 - local[i]);

        auto t0 = Clock::now();
        bool okRun = false;
        if (W.ok) {
            // 传到 B 的 color 纹理
            D3D12_RANGE nr{ 0, 0 };
            void* m = nullptr;
                HRESULT hm = W.up->Map(0, &nr, &m);
                if (W.processed < 4) Log("worker: Map 0x%08X m=%p", (unsigned)hm, m);
                if (FAILED(hm)) Log("worker: 设备丢失原因 0x%08X", (unsigned)W.dev->GetDeviceRemovedReason());
                if (SUCCEEDED(hm) && m) {
                memcpy(m, local.data(), local.size());
                D3D12_RANGE wr{ 0, (SIZE_T)local.size() };
                W.up->Unmap(0, &wr);

                W.al->Reset();
                W.cl->Reset(W.al.Get(), nullptr);
                D3D12_TEXTURE_COPY_LOCATION d{};
                d.pResource = W.color.Get();
                d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                d.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION s{};
                s.pResource = W.up.Get();
                s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                s.PlacedFootprint = W.fp;
                W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                W.cl->Close();
                { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                W.q->Signal(W.fe.Get(), ++W.fv);
                while (W.fe->GetCompletedValue() < W.fv) Sleep(0);

                if (g_enableNR && W.api.PrepareFrame) {
                    LmxxfNrJob job{};
                    job.struct_size = (uint32_t)sizeof(job);
                    FrameInfo120 fi120{};
                    LmxxfNrFrameInfo& fi = fi120.base;
                    fi.struct_size = (uint32_t)sizeof(FrameInfo120);
                    fi.session_id = 1;
                    fi.frame_id = (uint64_t)W.processed + 1;
                    fi.list_generation = 1;
                    fi.command_list = W.cl.Get();
                    fi.color_width = W.w;
                    fi.color_height = W.h;
                    fi.color = W.color.Get();
                    fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
                    fi.transfer_strength = 1.0f;
                    fi.color_strength = 1.0f;
                    fi.model_scale = 1.0f;
                    fi.pre_exposure = 1.0f;
                    fi.exposure_scale = 1.0f;

                    int32_t rcPF = W.api.PrepareFrame(W.ctx, &fi, &job);
                    if (rcPF != 0 && W.processed < 6) {
                        char eb[256] = { 0 };
                        if (W.api.GetLastError) W.api.GetLastError(eb, 256);
                        Log("worker: PrepareFrame rc=%d err=%s", (int)rcPF, eb);
                    }
                    if (rcPF == 0) {
                        W.al->Reset();
                        W.cl->Reset(W.al.Get(), nullptr);
                        if (W.api.RecordInputs(W.ctx, job.handle, W.cl.Get()) == 0) {
                            W.cl->Close();
                            { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                            W.q->Signal(W.fe.Get(), ++W.fv);
                            while (W.fe->GetCompletedValue() < W.fv) Sleep(0);
                            if (W.api.ExecuteAfterProducer(W.ctx, job.handle, W.q.Get()) == 0) {
                                W.al->Reset();
                                W.cl->Reset(W.al.Get(), nullptr);
                                if (W.api.RecordOutputs(W.ctx, job.handle, W.cl.Get()) == 0) {
                                    W.cl->Close();
                                    { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                                    W.q->Signal(W.fe.Get(), ++W.fv);
                                    while (W.fe->GetCompletedValue() < W.fv) Sleep(0);
                                    W.api.Retire(W.ctx, job.handle);
                                    okRun = true;
                                }
                            }
                        }
                    }
                } else {
                    okRun = true;    // 只做缩放直通
                }

                // 9070 XT 自己出画：放大到窗口尺寸 → 合成交换链 → DComp overlay
                if (W.reqW && W.reqH) {
                    if (WorkerPresentInit(W.reqW, W.reqH)) {
                        static bool lastVis = true;
                        bool want = (bool)g_nrOn;
                        if (want != lastVis && W.dvisual) {
                            lastVis = want;
                            W.dvisual->SetContent(want ? (IUnknown*)W.psc.Get() : nullptr);
                            W.dc->Commit();
                            Log("overlay: %s", want ? "显示（NR 结果）" : "隐藏（游戏原生画面）");
                        }
                        if (lastVis) WorkerPresentFrame();
                    }
                }
            }
        }
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(W.m);
            W.outBuf = local;
            W.hasOut = true;
            W.processed++;
            W.lastMs = ms;
            if (W.processed % 60 == 0 || W.processed <= 3)
                Log("worker: frame %d done in %.2f ms (nr=%d)", W.processed, ms, (int)okRun);
        }
    }
}

// ---------- 游戏侧（7900 XT）----------
struct CapSlot {
    ComPtr<ID3D12Resource> readback;
    UINT64 fenceValue = 0;
    bool pending = false;
};

struct GameCtx {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceCounter = 0;

    ComPtr<IDXGISwapChain> chain;
    ComPtr<IDXGISwapChain3> sc3;
    ComPtr<ID3D12Resource> bbs[4];

    UINT w4k = 0, h4k = 0;
    UINT w1k = 0, h1k = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    // 缩放相关
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> psoDown, psoUp;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> small1080;   // 下采样目标 / UAV
    ComPtr<ID3D12Resource> smallUp;     // 放大源 / SRV
    ComPtr<ID3D12Resource> bigOut;      // 4K 放大结果 / UAV、SRV
    ComPtr<ID3D12Resource> rb1080;      // 1080p 回读
    ComPtr<ID3D12Resource> up1080;      // 1080p 上传
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp1k{};
    UINT64 bytes1k = 0;
    UINT inc = 0;

    CapSlot slots[2];
    std::vector<uint8_t> host;
    std::vector<uint8_t> pendingWb;     // 待贴回的结果
    bool hasWb = false;

    bool ready = false;
    int captured = 0;
    int skipCounter = 0;
    bool loggedInit = false;
    double sumSubmit = 0;
    double sumHarvest = 0;
    int harvests = 0;
    int frames = 0;
    int closeFails = 0;
    int writebacks = 0;
    int blendLogs = 0;
    int feedCount = 0;
    int outCount = 0;
    Clock::time_point tStart{};
};

static GameCtx G;

// 合成交换链不抓
static bool IsCompositionSwapChain(void* scv) {
    if (!scv) return true;
    IDXGISwapChain* sc = (IDXGISwapChain*)scv;
    ComPtr<IDXGISwapChain1> s1;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&s1))) || !s1) return false;
    HWND hw = nullptr;
    if (FAILED(s1->GetHwnd(&hw))) return true;
    return hw == nullptr;
}

static ID3D12Resource* CurrentBackBuffer(UINT& indexOut) {
    indexOut = 0;
    if (G.sc3) indexOut = G.sc3->GetCurrentBackBufferIndex();
    if (indexOut >= 4) indexOut = 0;
    if (!G.bbs[indexOut] && G.chain) {
        ComPtr<ID3D12Resource> r;
        if (SUCCEEDED(G.chain->GetBuffer(indexOut, IID_PPV_ARGS(&r))) && r) G.bbs[indexOut] = r;
    }
    return G.bbs[indexOut].Get();
}

static ID3D12CommandQueue* FindGameQueue(ID3D12Device* dev) {
    std::wstring full = SelfDir() + L"\\d3d12.dll";
    HMODULE d3 = GetModuleHandleW(full.c_str());
    if (!d3) d3 = GetModuleHandleW(L"d3d12.dll");
    if (!d3) return nullptr;
    auto fnCount = (int32_t(*)())GetProcAddress(d3, "CrossGpuQueueCount");
    auto fnGet = (void*(*)(int32_t))GetProcAddress(d3, "CrossGpuGetQueue");
    if (!fnCount || !fnGet) return nullptr;
    int n = fnCount();
    for (int i = 0; i < n; ++i) {
        ID3D12CommandQueue* q = (ID3D12CommandQueue*)fnGet(i);
        if (!q) continue;
        ID3D12Device* qd = nullptr;
        bool same = false;
        if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qd))) && qd) { same = (qd == dev); qd->Release(); }
        if (same && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) return q;
    }
    return nullptr;
}

static bool BuildScalePipeline() {
    if (G.rs) return true;
    ComPtr<ID3DBlob> err;

    // 编译
    ComPtr<ID3DBlob> csDown, csUp;
    if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "inline.hlsl", nullptr, nullptr, "DownscaleCS", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csDown, &err))) {
        Log("compile down failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "inline.hlsl", nullptr, nullptr, "UpscaleCS", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csUp, &err))) {
        Log("compile up failed");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1; srvRange.BaseShaderRegister = 0; srvRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1; uavRange.BaseShaderRegister = 0; uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    ComPtr<ID3DBlob> sig;
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                        ID3DBlob**, ID3DBlob**);
    auto serialize = D3D12Proc<SerializeFn>("D3D12SerializeRootSignature");
    if (!serialize || FAILED(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Log("root sig failed");
        return false;
    }
    if (FAILED(G.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&G.rs)))) return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = G.rs.Get();
    pd.CS.pShaderBytecode = csDown->GetBufferPointer();
    pd.CS.BytecodeLength = csDown->GetBufferSize();
    if (FAILED(G.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&G.psoDown)))) return false;
    pd.CS.pShaderBytecode = csUp->GetBufferPointer();
    pd.CS.BytecodeLength = csUp->GetBufferSize();
    if (FAILED(G.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&G.psoUp)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 16;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(G.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&G.heap)))) return false;
    G.inc = G.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Log("scale pipeline ready");
    return true;
}

static void ReleaseOnResize() {
    for (auto& s : G.slots) { s.readback.Reset(); s.pending = false; s.fenceValue = 0; }
    for (auto& b : G.bbs) b.Reset();
    G.small1080.Reset(); G.smallUp.Reset(); G.bigOut.Reset(); G.rb1080.Reset(); G.up1080.Reset();
    G.ready = false;
    G.hasWb = false;
}

static void ResetAll() {
    ReleaseOnResize();
    G.alloc.Reset(); G.list.Reset(); G.fence.Reset();
    G.queue.Reset(); G.dev.Reset();
    G.sc3.Reset(); G.chain.Reset();
    G.fenceCounter = 0;
    G.ready = false;
    G.loggedInit = false;
}

static bool AcquireReady(IDXGISwapChain* sc) {
    // 每帧校验：游戏会重建 D3D12 设备，缓存的分配器/列表若属旧设备，
    // 提交到新队列上会静默失败 → fence 永不回来 → 抓帧卡死（已实测）。
    {
        ComPtr<IUnknown> unk;
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&unk))) && unk) {
            ComPtr<ID3D12Device> cur;
            if (SUCCEEDED(unk.As(&cur)) && cur) {
                if (G.dev && G.dev.Get() != cur.Get()) {
                    Log("device changed %p -> %p, rebuilding everything", G.dev.Get(), cur.Get());
                    ResetAll();
                }
            }
        }
    }
    if (G.dev && G.queue && G.ready) return true;
    if (!G.dev) {
        ComPtr<IUnknown> unk;
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&unk))) || !unk) { Log("GetDevice failed"); return false; }
        ComPtr<ID3D12Device> d;
        if (SUCCEEDED(unk.As(&d)) && d) G.dev = d;
        else {
            ComPtr<ID3D12CommandQueue> q;
            if (SUCCEEDED(unk.As(&q)) && q) { if (FAILED(q->GetDevice(IID_PPV_ARGS(&G.dev)))) return false; G.queue = q; }
        }
        if (!G.dev) { Log("no D3D12 device"); return false; }
    }
    if (!G.queue) {
        ID3D12CommandQueue* q = FindGameQueue(G.dev.Get());
        if (!q) { Log("no matching queue"); return false; }
        G.queue = q;
        Log("using game queue");
    }
    if (!G.alloc) {
        if (FAILED(G.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&G.alloc)))) return false;
        if (FAILED(G.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, G.alloc.Get(), nullptr, IID_PPV_ARGS(&G.list)))) return false;
        G.list->Close();
        if (FAILED(G.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&G.fence)))) return false;
    }
    if (!G.chain) {
        ComPtr<IDXGISwapChain> c;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&c))) && c) G.chain = c;
        ComPtr<IDXGISwapChain3> s3;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&s3))) && s3) G.sc3 = s3;
    }

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
    if (G.ready && G.w4k == desc.Width && G.h4k == desc.Height) return true;

    ReleaseOnResize();
    G.w4k = desc.Width;
    G.h4k = desc.Height;
    G.format = desc.Format;
    G.w1k = 1920;
    G.h1k = 1080;
    // 输入尺寸不能超过运行时的上限
    if (G.w4k <= 1920 && G.h4k <= 1080) { G.w1k = G.w4k; G.h1k = G.h4k; }

    if (!BuildScalePipeline()) return false;

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1;
    dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    auto makeTex = [&](UINT w, UINT h, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out, DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = fmt; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(G.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&out)));
    };
    if (!makeTex(G.w1k, G.h1k, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, G.small1080)) { Log("small1080 failed"); return false; }
    if (!makeTex(G.w1k, G.h1k, D3D12_RESOURCE_STATE_COPY_DEST, G.smallUp)) { Log("smallUp failed"); return false; }
    if (!makeTex(G.w4k, G.h4k, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, G.bigOut, G.format)) { Log("bigOut failed"); return false; }   // 必须与交换链同格式

    D3D12_RESOURCE_DESC d1k{};
    d1k.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d1k.Width = G.w1k; d1k.Height = G.h1k; d1k.DepthOrArraySize = 1; d1k.MipLevels = 1;
    d1k.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d1k.SampleDesc.Count = 1;
    d1k.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    G.dev->GetCopyableFootprints(&d1k, 0, 1, 0, &G.fp1k, &rows, &rowSize, &total);
    G.bytes1k = total;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(G.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&G.rb1080)))) { Log("rb1080 failed"); return false; }
    if (FAILED(G.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&G.up1080)))) { Log("up1080 failed"); return false; }

    for (int i = 0; i < 2; ++i) {
        if (FAILED(G.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&G.slots[i].readback)))) {
            Log("readback slot %d failed", i);
            return false;
        }
    }
    G.host.resize((size_t)G.bytes1k);
    G.pendingWb.resize((size_t)G.bytes1k);

    // 描述符
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = G.heap->GetCPUDescriptorHandleForHeapStart();
    auto srv = [&](ID3D12Resource* r, UINT slot, DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += (SIZE_T)slot * G.inc;
        G.dev->CreateShaderResourceView(r, &sd, h);
    };
    auto uav = [&](ID3D12Resource* r, UINT slot, DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = fmt;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += (SIZE_T)slot * G.inc;
        G.dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
    };
    // 一次性建好，之后每帧不再重建（重建正在使用的描述符会判非法）
    for (UINT i = 0; i < 4; ++i) {
        ID3D12Resource* b = nullptr;
        if (G.chain) {
            ComPtr<ID3D12Resource> r;
            if (SUCCEEDED(G.chain->GetBuffer(i, IID_PPV_ARGS(&r))) && r) { G.bbs[i] = r; b = r.Get(); }
        }
        if (b) srv(b, i, G.format);   // 必须用交换链真实格式（R10G10B10A2 等）
    }
    srv(G.smallUp.Get(), 4, DXGI_FORMAT_R8G8B8A8_UNORM);     // 放大源
    uav(G.small1080.Get(), 5);   // 下采样目标
    uav(G.bigOut.Get(), 6, G.format);   // 放大目标：与交换链同格式

    // 启动工作线程
    if (!g_hotkeyStarted) { g_hotkeyStarted = true; CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr); Log("热键线程已启动：F10 切换 NR"); }
    if (!g_worker.th.joinable()) {
        if (!WorkerInit(G.w1k, G.h1k)) { Log("worker init failed"); return false; }
        g_worker.hwnd = g_targetHwnd;
        {
            RECT rc{};
            if (g_targetHwnd && GetClientRect(g_targetHwnd, &rc)) {
                g_worker.reqW = (UINT)(rc.right - rc.left);
                g_worker.reqH = (UINT)(rc.bottom - rc.top);
                Log("worker: 窗口 HWND=%p 客户区 %ux%u（9070 XT 自己出画）", g_targetHwnd, g_worker.reqW, g_worker.reqH);
            }
        }
        g_worker.th = std::thread(WorkerThreadMain);
        // 预热：喂一帧全零，把 lmxxf 的首次权重上传提前到加载阶段
        {
            std::lock_guard<std::mutex> lk(g_worker.m);
            std::vector<uint8_t> zeros(g_worker.inBuf.size(), 0);
            g_worker.inBuf = zeros;
            g_worker.hasIn = true;
            Log("worker: primed with a dummy frame (warmup)");
            g_worker.cv.notify_one();
        }
    }

    G.ready = true;
    Log("ready: swapchain %ux%u fmt=%d, net %ux%u, staging %llu bytes",
        G.w4k, G.h4k, (int)G.format, G.w1k, G.h1k, (unsigned long long)G.bytes1k);
    return true;
}

static void HarvestGame() {
    for (int i = 0; i < 2; ++i) {
        CapSlot& s = G.slots[i];
        if (!s.pending || !s.readback) continue;
        if (G.fence->GetCompletedValue() < s.fenceValue) continue;

        auto t0 = Clock::now();
        D3D12_RANGE rr{ 0, (SIZE_T)G.bytes1k };
        void* m = nullptr;
        if (FAILED(s.readback->Map(0, &rr, &m)) || !m) { s.pending = false; return; }
        memcpy(G.host.data(), m, (size_t)G.bytes1k);
        D3D12_RANGE nw{ 0, 0 };
        s.readback->Unmap(0, &nw);
        s.pending = false;
        G.sumHarvest += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        G.harvests++;

        // 有新结果就取走备用（贴回用）
        {
            std::lock_guard<std::mutex> lk(g_worker.m);
            if (g_worker.hasOut) {
                memcpy(G.pendingWb.data(), g_worker.outBuf.data(), (size_t)G.bytes1k);
                g_worker.hasOut = false;
                // 客观校验：NR 输出与输入的差异（小=合理修正，大=可能乱码）
                {
                    const uint8_t* a = g_worker.outBuf.data();
                    const uint8_t* bb2 = G.host.data();
                    uint64_t acc = 0; size_t cnt = 0;
                    for (size_t q = 0; q + 4 < G.host.size(); q += 4096) {
                        acc += (uint64_t)abs((int)a[q] - (int)bb2[q]);
                        acc += (uint64_t)abs((int)a[q + 1] - (int)bb2[q + 1]);
                        acc += (uint64_t)abs((int)a[q + 2] - (int)bb2[q + 2]);
                        cnt += 3;
                    }
                    double mad = cnt ? (double)acc / cnt : 0.0;
                    // 自证：抓回来的画面（此时后备缓冲应已是 NR 结果）与 NR 输出的平均值
                    uint64_t sa = 0, sb = 0; size_t sc2 = 0;
                    for (size_t q = 0; q + 2 < G.host.size(); q += 4093) { sa += a[q]; sb += bb2[q]; sc2++; }
                    double meanNr = sc2 ? (double)sa / sc2 : 0.0;
                    double meanCap = sc2 ? (double)sb / sc2 : 0.0;
                    if (G.blendLogs < 8 || G.blendLogs % 120 == 0)
                        Log("nr-diff #%d: 通道差 %.2f | 抓回画面均值 %.1f | NR 输出均值 %.1f | 差 %.1f",
                            G.blendLogs, mad, meanCap, meanNr, fabs(meanCap - meanNr));
                    G.blendLogs++;
                }
                G.hasWb = true;
            }
            // 把这一帧交给工作线程（若空闲）
            if (!g_worker.hasIn && g_worker.ok) {
                memcpy(g_worker.inBuf.data(), G.host.data(), (size_t)G.bytes1k);
                g_worker.hasIn = true;
                g_worker.cv.notify_one();
            }
        }
    }
}

static void RebuildCommands() {
    G.list.Reset(); G.alloc.Reset(); G.fence.Reset();
    for (auto& sl : G.slots) { sl.pending = false; }
    if (SUCCEEDED(G.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&G.alloc))) &&
        SUCCEEDED(G.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, G.alloc.Get(), nullptr, IID_PPV_ARGS(&G.list)))) {
        HRESULT hc = G.list->Close();
        if (SUCCEEDED(G.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&G.fence)))) {
            G.fenceCounter = 0;
            Log("命令对象已重建 (Close=0x%08X)", (unsigned)hc);
            return;
        }
    }
    Log("命令对象重建失败");
}


static void Frame(IDXGISwapChain* sc) {
    if (g_off) return;
    G.frames++;
    if (G.tStart.time_since_epoch().count() == 0) G.tStart = Clock::now();
    if (g_interval > 1 && (++G.skipCounter % g_interval) != 0) return;
    if (!AcquireReady(sc)) return;

    HarvestGame();

    if (g_targetHwnd) {
        RECT rc{};
        if (GetClientRect(g_targetHwnd, &rc)) {
            g_worker.reqW = (UINT)(rc.right - rc.left);
            g_worker.reqH = (UINT)(rc.bottom - rc.top);
        }
    }
    UINT bbIndex = 0;
    ID3D12Resource* bb = CurrentBackBuffer(bbIndex);
    if (!bb) return;

    auto t0 = Clock::now();
    // 上一帧提交可能仍在执行：D3D12 禁止重置仍在使用中的分配器，
    // 不等就会静默失败 → 抓帧永久停摆（游戏里实测到的现象）。
    if (G.fenceCounter > 0) {
        auto tw = Clock::now();
        while (G.fence->GetCompletedValue() < G.fenceCounter) {
            if (std::chrono::duration<double, std::milli>(Clock::now() - tw).count() > 8.0) {
                if (G.frames % 300 == 0) Log("gpu wait timeout at frame %d (fence=%llu completed=%llu)", G.frames,
                    (unsigned long long)G.fenceCounter, (unsigned long long)G.fence->GetCompletedValue());
                break;
            }
            Sleep(0);
        }
    }
    {
        HRESULT hra = G.alloc->Reset();
        if (FAILED(hra)) { if (G.frames % 120 == 0) Log("frame %d: alloc Reset failed 0x%08X", G.frames, (unsigned)hra); return; }
        HRESULT hrl = G.list->Reset(G.alloc.Get(), nullptr);
        if (FAILED(hrl)) {
            // 列表进入错误状态（通常是某次 Resize 释放了它引用的资源）→ 自愈：重建整套命令对象
            if (G.frames % 60 == 0) Log("frame %d: list Reset failed 0x%08X -> 重建", G.frames, (unsigned)hrl);
            RebuildCommands();
            return;
        }
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // ① 贴回：1080p 结果 → smallUp → 放大到 bigOut → 拷进后备缓冲
    bool didWb = false;
    if (false && G.hasWb && G.up1080 && G.smallUp && G.bigOut) {   // 已改为 9070 XT 自己出画（DComp overlay）
        void* m = nullptr;
        D3D12_RANGE nr{ 0, 0 };
        if (SUCCEEDED(G.up1080->Map(0, &nr, &m)) && m) {
            memcpy(m, G.pendingWb.data(), (size_t)G.bytes1k);
            D3D12_RANGE wr{ 0, (SIZE_T)G.bytes1k };
            G.up1080->Unmap(0, &wr);

            D3D12_TEXTURE_COPY_LOCATION cd{};
            cd.pResource = G.smallUp.Get();
            cd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION cs{};
            cs.pResource = G.up1080.Get();
            cs.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            cs.PlacedFootprint = G.fp1k;
            G.list->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);

            D3D12_RESOURCE_BARRIER ub{};
            ub.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ub.Transition.pResource = G.smallUp.Get();
            ub.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ub.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            ub.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            G.list->ResourceBarrier(1, &ub);

            ID3D12DescriptorHeap* heaps[] = { G.heap.Get() };
            G.list->SetDescriptorHeaps(1, heaps);
            G.list->SetComputeRootSignature(G.rs.Get());
            D3D12_GPU_DESCRIPTOR_HANDLE srvTbl = G.heap->GetGPUDescriptorHandleForHeapStart();
            srvTbl.ptr += (SIZE_T)4 * G.inc;          // SRV(smallUp)
            D3D12_GPU_DESCRIPTOR_HANDLE uavTbl = G.heap->GetGPUDescriptorHandleForHeapStart();
            uavTbl.ptr += (SIZE_T)6 * G.inc;          // UAV(bigOut)
            G.list->SetComputeRootDescriptorTable(0, srvTbl);
            G.list->SetComputeRootDescriptorTable(1, uavTbl);
            G.list->SetPipelineState(G.psoUp.Get());
            G.list->Dispatch((G.w4k + 7) / 8, (G.h4k + 7) / 8, 1);

            D3D12_RESOURCE_BARRIER bo{};
            bo.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bo.Transition.pResource = G.bigOut.Get();
            bo.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bo.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            bo.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            G.list->ResourceBarrier(1, &bo);

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            G.list->ResourceBarrier(1, &b);

            D3D12_TEXTURE_COPY_LOCATION bd{};
            bd.pResource = bb;
            bd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION bs{};
            bs.pResource = G.bigOut.Get();
            bs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bs.SubresourceIndex = 0;
            G.list->CopyTextureRegion(&bd, 0, 0, 0, &bs, nullptr);

            D3D12_RESOURCE_BARRIER bo2{};
            bo2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bo2.Transition.pResource = G.bigOut.Get();
            bo2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bo2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            bo2.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            G.list->ResourceBarrier(1, &bo2);

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            G.list->ResourceBarrier(1, &b);
            didWb = true;
            // 保留 hasWb：没有新结果时沿用上一帧的 NR 输出，避免交替闪烁
            G.writebacks++;
        }
    }
    if (!didWb) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        G.list->ResourceBarrier(1, &b);
    }

    // ② 抓帧：后备缓冲（SRV）→ 下采样到 1080p → 拷进回读缓冲
    int slot = -1;
    for (int k = 0; k < 2; ++k) { if (!G.slots[k].pending) { slot = k; break; } }
    if (slot < 0) {
        if (G.frames % 300 == 0)
            Log("capture stalled: both slots pending (v0=%llu v1=%llu completed=%llu)",
                (unsigned long long)G.slots[0].fenceValue, (unsigned long long)G.slots[1].fenceValue,
                (unsigned long long)G.fence->GetCompletedValue());
        G.list->Close();   // 关键：不 Close 会让下一帧 Reset 失败并永久停摆
        return;
    }
    CapSlot& cs2 = G.slots[slot];
    if (G.frames % 300 == 0)
        Log("cap-debug: frames=%d slot=%d pend0=%d pend1=%d small=%p rb=%p up=%p ready=%d captured=%d",
            G.frames, slot, (int)G.slots[0].pending, (int)G.slots[1].pending,
            G.small1080.Get(), G.rb1080.Get(), G.up1080.Get(), (int)G.ready, G.captured);
    if (G.small1080 && G.rb1080) {
        // 需要后备缓冲作为 SRV：slot 用它的索引
        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = bb;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        G.list->ResourceBarrier(1, &tb);

        ID3D12DescriptorHeap* heaps[] = { G.heap.Get() };
        G.list->SetDescriptorHeaps(1, heaps);
        G.list->SetComputeRootSignature(G.rs.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE srvTbl = G.heap->GetGPUDescriptorHandleForHeapStart();
        srvTbl.ptr += (SIZE_T)bbIndex * G.inc;   // SRV(后备缓冲) 已按索引预建
        D3D12_GPU_DESCRIPTOR_HANDLE uavTbl = G.heap->GetGPUDescriptorHandleForHeapStart();
        uavTbl.ptr += (SIZE_T)5 * G.inc;         // UAV(small1080)
        G.list->SetComputeRootDescriptorTable(0, srvTbl);
        G.list->SetComputeRootDescriptorTable(1, uavTbl);
        G.list->SetPipelineState(G.psoDown.Get());
        G.list->Dispatch((G.w1k + 7) / 8, (G.h1k + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER sb{};
        sb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        sb.Transition.pResource = G.small1080.Get();
        sb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        sb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        sb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        G.list->ResourceBarrier(1, &sb);

        D3D12_TEXTURE_COPY_LOCATION rd{};
        rd.pResource = cs2.readback.Get();
        rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rd.PlacedFootprint = G.fp1k;
        D3D12_TEXTURE_COPY_LOCATION rs{};
        rs.pResource = G.small1080.Get();
        rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rs.SubresourceIndex = 0;
        G.list->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);

        D3D12_RESOURCE_BARRIER sb2{};
        sb2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        sb2.Transition.pResource = G.small1080.Get();
        sb2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        sb2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        sb2.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        G.list->ResourceBarrier(1, &sb2);

        b.Transition.StateBefore = tb.Transition.StateAfter;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        G.list->ResourceBarrier(1, &b);

        if (SUCCEEDED(G.list->Close())) {
            ID3D12CommandList* l[] = { G.list.Get() };
            G.queue->ExecuteCommandLists(1, l);
            cs2.fenceValue = ++G.fenceCounter;
            G.queue->Signal(G.fence.Get(), cs2.fenceValue);
            cs2.pending = true;
            G.captured++;
        }
    } else {
        // 没有空闲回读槽：只把后备缓冲状态恢复，本帧不抓
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        G.list->ResourceBarrier(1, &b);
        if (SUCCEEDED(G.list->Close())) {
            ID3D12CommandList* l[] = { G.list.Get() };
            G.queue->ExecuteCommandLists(1, l);
        }
    }

    {
        HRESULT rr = G.dev->GetDeviceRemovedReason();
        if (FAILED(rr)) Log("DEVICE REMOVED in Frame: 0x%08X (captured=%d)", (unsigned)rr, G.captured);
    }
    G.sumSubmit += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (G.frames % 120 == 0) {
        std::lock_guard<std::mutex> lk(g_worker.m);
        double secs = std::chrono::duration<double>(Clock::now() - G.tStart).count();
        double fps = secs > 0.1 ? G.frames / secs : 0.0;
        Log("stats: frames=%d fps=%.1f captured=%d harvests=%d writebacks=%d closeFails=%d workerFrames=%d avgSubmit=%.3fms avgHarvest=%.3fms workerLast=%.2fms",
            G.frames, fps, G.captured, G.harvests, G.writebacks, G.closeFails, g_worker.processed,
            G.sumSubmit / (G.captured ? G.captured : 1),
            G.harvests ? G.sumHarvest / G.harvests : 0.0, g_worker.lastMs);
    }
}

// ---------- 钩子 ----------
static HRESULT STDMETHODCALLTYPE Detour_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    static void* s_lastSc = nullptr;
    static int s_presentCount = 0;
    s_presentCount++;
    if (self != s_lastSc) { Log("Present on new swapchain %p (n=%d)", self, s_presentCount); s_lastSc = self; }
    bool comp = IsCompositionSwapChain(self);
    if (s_presentCount % 600 == 0)
        Log("Present heartbeat n=%d comp=%d frames=%d captured=%d wb=%d", s_presentCount,
            (int)comp, G.frames, G.captured, G.writebacks);
    if (!comp) Frame(self);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    Fn orig = (Fn)OrigOf(SLOT_Present);
    return orig ? orig(self, sync, flags) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE Detour_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                                 const DXGI_PRESENT_PARAMETERS* params) {
    if (!IsCompositionSwapChain(self)) Frame((IDXGISwapChain*)self);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    Fn orig = (Fn)OrigOf(SLOT_Present1);
    return orig ? orig(self, sync, flags, params) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE Detour_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h,
                                                      DXGI_FORMAT format, UINT flags) {
    Log("ResizeBuffers(%u,%u,%u)", count, w, h);
    ReleaseOnResize();
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    Fn orig = (Fn)OrigOf(SLOT_ResizeBuffers);
    return orig ? orig(self, count, w, h, format, flags) : E_FAIL;
}

// g_targetHwnd 已提前到文件顶部声明

static HWND SwapChainHwnd(IDXGISwapChain* sc) {
    if (!sc) return nullptr;
    ComPtr<IDXGISwapChain1> s1;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&s1))) || !s1) return nullptr;
    HWND hw = nullptr;
    if (FAILED(s1->GetHwnd(&hw))) return nullptr;
    return hw;
}

static void PatchSwapChainVtables(IDXGISwapChain* sc) {
    // 只接管游戏主窗口的交换链：插件（libxess / PDPerfPlugin / dlssnr 各 pass）
    // 会自己建工厂和交换链，若一并接管会把我们的资源尺寸带跑并让抓帧停摆（已实测）。
    HWND hw = SwapChainHwnd(sc);
    if (!hw) { Log("swapchain %p 无窗口句柄（合成/离屏），不接管", sc); return; }
    if (!g_targetHwnd) {
        g_targetHwnd = hw;
        Log("锁定目标窗口 HWND=%p（交换链 %p）", hw, sc);
    } else if (hw != g_targetHwnd) {
        // 游戏换窗口（进出全屏等）会重建交换链：跟随切换，否则会贴在不可见的旧链上
        Log("目标窗口变化 %p -> %p：切换到新交换链 %p", g_targetHwnd, hw, sc);
        g_targetHwnd = hw;
        ResetAll();
    }
    Log("PatchSwapChainVtables(%p)", sc);
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
    Log("CreateSwapChainForComposition -> 0x%08X (不抓)", (unsigned)hr);
    if (SUCCEEDED(hr) && out && *out) { PatchSwapChainVtables(*out); }   // 合成交换链靠 GetHwnd 判定，无需登记
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
        Log("dxgi proxy v4 attached (log=%d off=%d nr=%d)", (int)g_log, (int)g_off, (int)g_enableNR);
        InitForwarders();
    }
    return TRUE;
}
