// libxess.dll 代理 v8 —— 输入侧按离线验证的结论重做
//
// 输入侧（离线四组对照的结论）：
//   pOutput(3840x2160 R10G10B10A2) → 1920x1080 RGBA8   +   MV → 1920x1080 SNORM(xy×0.862)
//   depth 不接（实测与不接逐位相同）
//
// 抓取状态假设（抓帧原理）：
//   pColor/pOutput = COMMON，MV = PIXEL_SHADER_RESOURCE（compute 读前临时转 NON_PIXEL）
//   建回读缓冲前先清掉 desc 的 ALLOW_UNORDERED_ACCESS 标志
//
// 结果来源：job.private_output（Retire 之前，COMMON→COPY_SOURCE→COMMON），实测 fi.color 逐位不变
//
// 安全：默认只测量不改画面；游戏目录出现 xess_proxy_apply.txt 才把 edit 叠回 pOutput。

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <share.h>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

extern "C" void* g_xessTab[72] = {};
static const char* kNames[72] = {
#include "xess_fwd_names.inc"
};

static HMODULE g_self = nullptr, g_real = nullptr;
static FILE* g_log = nullptr;
static volatile bool g_nrOn = true, g_apply = false, g_feedMv = true;
static volatile float g_strength = 1.0f;
// pOutput 的「假定状态」（游戏实际状态未知，用 F2 试）
static const D3D12_RESOURCE_STATES kOutStates[5] = {
    D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_RENDER_TARGET,
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
};
static const char* kOutStateNames[5] = { "COMMON", "UNORDERED_ACCESS", "RENDER_TARGET", "PIXEL_SHADER_RESOURCE", "NON_PIXEL_SR" };
static volatile int g_outStateIdx = 0;
static volatile int g_passthrough = 0;     // F3：纯往返测试（拷出再原样拷回）
static volatile int g_answerOnly = 0;      // F4：只呈现网络答案（对照模式）
static volatile int g_editSmoothR = 0;      // edit 空间平滑半径（1080p 像素，0=关）
static volatile float g_emaStatic = 0.15f, g_emaMotion = 1.0f, g_emaK = 20.0f;   // 时域平滑（F9 切换）
static bool g_emaFirst = true;
static volatile bool g_syncPresent = false;    // present 等待 NR 内容更新（xess_proxy_nowait.txt 可关）
static volatile double g_waitMs = 0;
static volatile bool g_outstanding = false;
static bool g_freezeBasePending = false;
static volatile int g_modeIdx = 1;                              // 0关/1默认/2强/3极强（左上角指示）
static volatile float g_emaDecay = 0.85f, g_emaSup = 0.0f;   // B 模式默认门限关   // 余韵衰减 / 运动抑制强度
static bool g_disabled = false;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}
static std::string ModuleDir() {
    char p[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(g_self, p, MAX_PATH);
    if (!n) return std::string();
    std::string s(p); size_t q = s.find_last_of("\\/");
    return (q == std::string::npos) ? std::string() : s.substr(0, q + 1);
}
static void OpenLog() {
    if (g_log) return;
    std::string s = ModuleDir() + "xess_proxy.log";
    g_log = _fsopen(s.c_str(), "w", _SH_DENYNO);
}
static HMODULE RealXess() {
    if (g_real) return g_real;
    std::string s = ModuleDir() + "libxess_orig.dll";
    g_real = LoadLibraryA(s.c_str());
    if (!g_real) Log("!! 加载失败 err=%lu", GetLastError());
    return g_real;
}
static void InitForwarders() {
    HMODULE m = RealXess();
    for (int i = 0; i < 72; ++i) {
        g_xessTab[i] = m ? (void*)GetProcAddress(m, kNames[i]) : nullptr;
        if (!g_xessTab[i]) Log("!! 缺少导出: %s", kNames[i]);
    }
}

// ---------------- ABI ----------------
struct ApiTable {
    uint32_t struct_size; uint32_t abi_version;
    int32_t (*QueryCapabilities)(void*);
    int32_t (*Create)(const void*, void**);
    int32_t (*Destroy)(void*);
    int32_t (*PrepareSession)(void*);
    int32_t (*PrepareFrame)(void*, const void*, void*);
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
    int32_t (*GetTimings)(void*, void*);
    void* s18; void* s19;
};
struct FrameInfo160 {
    uint32_t struct_size; uint32_t pad0;
    uint64_t session_id, frame_id, list_generation;
    void* command_list;
    uint32_t color_width, color_height;
    void* color;
    uint32_t color_state, flags;
    float transfer_strength, color_strength;
    uint32_t debug_view; float model_scale;
    void* exposure; uint32_t exposure_state;
    float pre_exposure, exposure_scale;
    void* depth; void* velocity;
    float ctlTone, ctlStructure, ctlSkin, ctlOther, ctlStyle;
    uint32_t ctlPad;
    void* historyMask; void* reserved152;
};
static_assert(sizeof(FrameInfo160) == 160, "");
struct Job { uint32_t struct_size; void* handle; void* private_output; };
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; void* reserved0; };

static const UINT kNetW = 1920, kNetH = 1080;
static UINT g_outW = 0, g_outH = 0;
static float g_mvScaleX = 0.862f, g_mvScaleY = 0.862f;

// ---------------- shader ----------------
static const char* kHLSL = R"(
cbuffer P : register(b0) { float gSx; float gSy; float gStrength; float gPad;
                            float gAStatic; float gAMotion; float gK; float gFirst;
                            float gDecay; float gSup; float gMode; float gAnswerOnly; };

Texture2D<float4>   gSrc : register(t0);
Texture2D<float4>   gEdit: register(t1);
Texture2D<float4>   gRaw : register(t0);   // EditEmaCS 用
Texture2D<float4>   gMvSrc : register(t1); // EditEmaCS 用
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);

// 下采样颜色（bilinear）
[numthreads(8,8,1)]
void DownColorCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;
    float2 uv=(float2(id.xy)+0.5)/float2(w,h);
    float4 c=gSrc.SampleLevel(gSmp,uv,0);
    gDst[id.xy]=float4(c.rgb,1.0);
}

// 下采样运动矢量：xy 乘分辨率比例，zw 清零
[numthreads(8,8,1)]
void DownMvCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;
    float2 uv=(float2(id.xy)+0.5)/float2(w,h);
    float4 v=gSrc.SampleLevel(gSmp,uv,0);
    gDst[id.xy]=float4(v.x*gSx, v.y*gSy, 0.0, 0.0);
}

// 变化差分：t0=本帧1080p，t1=上一帧1080p → u0=逐像素最大通道差
[numthreads(8,8,1)]
void DiffCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;
    float3 a = gSrc.Load(int3(id.xy,0)).rgb;
    float3 b = gEdit.Load(int3(id.xy,0)).rgb;
    float d = max(max(abs(a.r-b.r), abs(a.g-b.g)), abs(a.b-b.b));
    gDst[id.xy] = float4(d,d,d,1.0);
}

// 门限：画面变化大的地方直接把 edit 掐向中性（无记忆，不会拖尾）
//   t0 = editRaw(编码差分)， t1 = MV(1080p SNORM)， u0 = editSmooth（UAV 读改写）
[numthreads(8,8,1)]
void EditEmaCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;
    float d    = gEdit.Load(int3(id.xy,0)).r;      // t1 = 本帧 vs edit 来源帧 的差异
    float4 raw = gRaw.Load(int3(id.xy,0));         // t0 = 原始 edit
    float4 prev = gDst[id.xy];

    // EMA：变化大就快跟随，静止慢跟随（压闪烁）
    float aEff = lerp(gAStatic, 1.0, saturate(d * gK));
    if (gFirst > 0.5) aEff = 1.0;
    float3 upd = lerp(prev.rgb, raw.rgb, aEff);

    // 门限：内容已经变了，旧 edit 立刻掐掉（防残影）
    float gate = 1.0 / (1.0 + gSup * d);
    upd = lerp(float3(0.5,0.5,0.5), upd, gate);

    gDst[id.xy] = float4(upd, 1.0);
}

// 叠加：base(t0) + edit(t1 抬到本几何)
[numthreads(8,8,1)]
void CompositeCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;
    float2 uv=(float2(id.xy)+0.5)/float2(w,h);
    // 对照模式：只呈现网络答案（1080p 放大），不叠 4K 底图
    if (gAnswerOnly > 0.5) {
        gDst[id.xy] = float4(clamp(gEdit.SampleLevel(gSmp, uv, 0).rgb, 0.0, 1.0), 1.0);
        return;
    }

    float4 base=gSrc.Load(int3(id.xy,0));
    float3 e;
    float r = gPad;
    if (r < 0.01) {
        e = gEdit.SampleLevel(gSmp, uv, 0).rgb - 0.5;
    } else {
        float2 texel = float2(0.000520833, 0.000925926) * r;
        e  = (gEdit.SampleLevel(gSmp, uv + float2( 0.0,  1.0)*texel, 0).rgb - 0.5);
        e += (gEdit.SampleLevel(gSmp, uv + float2( 0.0, -1.0)*texel, 0).rgb - 0.5);
        e += (gEdit.SampleLevel(gSmp, uv + float2( 1.0,  0.0)*texel, 0).rgb - 0.5);
        e += (gEdit.SampleLevel(gSmp, uv + float2(-1.0,  0.0)*texel, 0).rgb - 0.5);
        e += (gEdit.SampleLevel(gSmp, uv + float2( 0.7,  0.7)*texel, 0).rgb - 0.5);
        e += (gEdit.SampleLevel(gSmp, uv + float2(-0.7,  0.7)*texel, 0).rgb - 0.5);
        e += (gEdit.SampleLevel(gSmp, uv + float2( 0.7, -0.7)*texel, 0).rgb - 0.5);
        e += (gEdit.SampleLevel(gSmp, uv + float2(-0.7, -0.7)*texel, 0).rgb - 0.5);
        e += (gEdit.SampleLevel(gSmp, uv, 0).rgb - 0.5);
        e /= 9.0;
    }
    e *= gStrength;
    float4 outc=float4(saturate(base.rgb+e), base.a);

    // 左上角档位指示：1 格=默认(绿) 2 格=强(黄) 3 格=极强(红)
    int lvl=(int)gMode;
    if (lvl>0 && id.y<12 && id.x<3*34) {
        int bar=id.x/34;
        if (bar<lvl) {
            float3 col = bar==0 ? float3(0.1,1.0,0.1) : (bar==1 ? float3(1.0,1.0,0.1) : float3(1.0,0.2,0.2));
            outc=float4(col,1.0);
        } else {
            outc=float4(0.03,0.03,0.03,1.0);
        }
    }
    gDst[id.xy]=outc;
}
)";

// ---------------- worker ----------------
struct Worker {
    std::thread th; std::mutex m; std::condition_variable cv; std::condition_variable cvOut;
    std::vector<uint8_t> inBuf, inMv, outBuf;
    bool hasIn = false, hasOut = false, quit = false;
    int processed = 0; double lastMs = 0, lastEditMean = 0; int lastEditMax = 0;
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al; ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fe; UINT64 fv = 0;
    ComPtr<ID3D12Resource> color, mv, up, upMv, rb;
    ComPtr<ID3D12Resource> rbPriv;                 // private_output 回读（复用）
    UINT64 rbPrivBytes = 0;
    void* upPtr = nullptr; void* upMvPtr = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}, fpMv{};
    UINT64 bytes = 0, bytesMv = 0;
    HMODULE mod = nullptr; ApiTable api{}; void* ctx = nullptr; bool ok = false;
};
static Worker g_worker;

static void WaitW() {
    Worker& W = g_worker;
    W.q->Signal(W.fe.Get(), ++W.fv);
    while (W.fe->GetCompletedValue() < W.fv) Sleep(0);
}
static ComPtr<ID3D12Resource> MakeTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
                                      D3D12_RESOURCE_STATES st, bool uav) {
    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
    return r;
}

static bool WorkerInit() {
    Worker& W = g_worker;
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) return false;
    ComPtr<IDXGIAdapter1> ad;
    {   // 按名字优先选 9070 XT：运行时要求 D3D12 设备必须与 HIP 设备同卡
        ComPtr<IDXGIAdapter1> best, fallback;
        for (UINT i = 0; i < 8; ++i) {
            ComPtr<IDXGIAdapter1> a1;
            if (fac->EnumAdapters1(i, a1.GetAddressOf()) != S_OK) break;
            DXGI_ADAPTER_DESC dd{};
            a1->GetDesc(&dd);
            Log("  worker 候选[%u] = %ls", i, dd.Description);
            if (wcsstr(dd.Description, L"9070") && !best) best = a1;
            if (i == 1 && !fallback) fallback = a1;
        }
        ad = best ? best : fallback;
    }
    if (!ad) { Log("worker: 找不到可用适配器"); return false; }
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(W.dev.GetAddressOf())))) {
        Log("worker: D3D12 设备创建失败"); return false;
    }
    {
        DXGI_ADAPTER_DESC dChosen{};
        ad->GetDesc(&dChosen);
        Log("worker: 使用 %ls  LUID=%08x:%08x", dChosen.Description,
            (unsigned)dChosen.AdapterLuid.HighPart, (unsigned)dChosen.AdapterLuid.LowPart);
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    W.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(W.q.GetAddressOf()));
    W.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(W.al.GetAddressOf()));
    W.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, W.al.Get(), nullptr, IID_PPV_ARGS(W.cl.GetAddressOf()));
    W.cl->Close();
    W.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(W.fe.GetAddressOf()));

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    W.color = MakeTex(W.dev.Get(), kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, false);
    W.mv    = MakeTex(W.dev.Get(), kNetW, kNetH, DXGI_FORMAT_R16G16B16A16_SNORM, D3D12_RESOURCE_STATE_COMMON, false);
    if (!W.color || !W.mv) { Log("worker: 纹理失败"); return false; }

    D3D12_RESOURCE_DESC cd = W.color->GetDesc();
    D3D12_RESOURCE_DESC plainC = cd; plainC.Flags = D3D12_RESOURCE_FLAG_NONE;   // 清 UAV 标志再算布局
    UINT rows = 0; UINT64 rs = 0, total = 0;
    W.dev->GetCopyableFootprints(&plainC, 0, 1, 0, &W.fp, &rows, &rs, &total);
    W.bytes = total;
    D3D12_RESOURCE_DESC md = W.mv->GetDesc();
    D3D12_RESOURCE_DESC plainM = md; plainM.Flags = D3D12_RESOURCE_FLAG_NONE;
    W.dev->GetCopyableFootprints(&plainM, 0, 1, 0, &W.fpMv, &rows, &rs, &total);
    W.bytesMv = total;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = W.bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(W.up.GetAddressOf()));
    W.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(W.rb.GetAddressOf()));
    D3D12_RESOURCE_DESC bdm = bd; bdm.Width = W.bytesMv;
    W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bdm, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(W.upMv.GetAddressOf()));
    {
        D3D12_RANGE zr{ 0, 0 };
        if (SUCCEEDED(W.up->Map(0, &zr, &W.upPtr)) && W.upPtr) Log("worker: 上传缓冲持久映射 ok");
        if (SUCCEEDED(W.upMv->Map(0, &zr, &W.upMvPtr)) && W.upMvPtr) Log("worker: MV 缓冲持久映射 ok");
    }

    W.inBuf.assign((size_t)W.bytes, 0);
    W.inMv.assign((size_t)W.bytesMv, 0);
    W.outBuf.assign((size_t)W.bytes, 0);

    std::string dir = ModuleDir();
    std::wstring pak(dir.begin(), dir.end()); pak += L"LmxxfNrRuntime.pak";
    std::wstring dllp = pak;
    size_t sl = dllp.find_last_of(L'\\');
    if (sl != std::wstring::npos) dllp = dllp.substr(0, sl + 1);
    dllp += L"LmxxfNrRuntime.dll";
    W.mod = LoadLibraryW(dllp.c_str());
    if (!W.mod) { Log("worker: lmxxf 加载失败"); return false; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(W.mod, "LmxxfNrGetApi");
    W.api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!getApi || getApi(1, &W.api) != 0) { Log("worker: GetApi 失败"); return false; }
    CreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = W.dev.Get(); ci.queue = W.q.Get(); ci.assets_directory = pak.c_str();
    if (W.api.Create(&ci, &W.ctx) != 0) { Log("worker: Create 失败"); return false; }
    if (W.api.PrepareSession(W.ctx) != 0) { Log("worker: PrepareSession 失败"); return false; }
    Log("worker: 会话就绪 (%ux%u, color %llu / mv %llu 字节)", kNetW, kNetH,
        (unsigned long long)W.bytes, (unsigned long long)W.bytesMv);
    W.ok = true;
    return true;
}

static void WorkerMain() {
    Worker& W = g_worker;
    std::vector<uint8_t> local, localMv;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(W.m);
            W.cv.wait(lk, [&] { return W.quit || W.hasIn; });
            if (W.quit) break;
            local = W.inBuf; localMv = W.inMv; W.hasIn = false;
        }
        auto t0 = Clock::now();
        bool got = false;

        // 持久映射：直接写指针，不再每帧 Map/Unmap
        if (!W.upPtr) { continue; }
        memcpy(W.upPtr, local.data(), local.size());
        void* mm = nullptr;
        if (!localMv.empty() && W.upMvPtr) {
            memcpy(W.upMvPtr, localMv.data(), localMv.size());
            mm = W.upMvPtr;
        }
        W.al->Reset(); W.cl->Reset(W.al.Get(), nullptr);
        {
            D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = W.color.Get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = W.up.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = W.fp;
            W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            if (mm) {
                D3D12_TEXTURE_COPY_LOCATION vd{}; vd.pResource = W.mv.Get();
                vd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; vd.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION vs{}; vs.pResource = W.upMv.Get();
                vs.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; vs.PlacedFootprint = W.fpMv;
                W.cl->CopyTextureRegion(&vd, 0, 0, 0, &vs, nullptr);
            }
        }
        W.cl->Close();
        { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
        WaitW();

        if (W.api.PrepareFrame) {
            Job job{};
            job.struct_size = (uint32_t)sizeof(Job);
            FrameInfo160 fi{};
            fi.struct_size = (uint32_t)sizeof(FrameInfo160);
            fi.session_id = 1; fi.frame_id = (uint64_t)W.processed + 1; fi.list_generation = 1;
            fi.command_list = W.cl.Get();
            fi.color_width = kNetW; fi.color_height = kNetH;
            fi.color = W.color.Get();
            fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
            fi.transfer_strength = 1.0f; fi.color_strength = 1.0f;
            fi.model_scale = 1.0f; fi.pre_exposure = 1.0f; fi.exposure_scale = 1.0f;
            fi.depth = nullptr;
            fi.velocity = (mm && g_feedMv) ? W.mv.Get() : nullptr;
            fi.ctlTone = 1.0f; fi.ctlStructure = 1.0f; fi.ctlSkin = 3.0f; fi.ctlOther = 2.0f; fi.ctlStyle = 0.0f;

            int32_t r = W.api.PrepareFrame(W.ctx, &fi, &job);
            if (r != 0) {
                if (W.processed < 3) {
                    char eb[256] = { 0 }; W.api.GetLastError(eb, 256);
                    Log("worker: PrepareFrame -> %d err=%s", (int)r, eb);
                }
            } else {
                W.al->Reset(); W.cl->Reset(W.al.Get(), nullptr);
                W.api.RecordInputs(W.ctx, job.handle, W.cl.Get());
                W.cl->Close();
                { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                WaitW();
                if (W.api.ExecuteAfterProducer(W.ctx, job.handle, W.q.Get()) == 0) {
                    W.al->Reset(); W.cl->Reset(W.al.Get(), nullptr);
                    W.api.RecordOutputs(W.ctx, job.handle, W.cl.Get());
                    W.cl->Close();
                    { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                    WaitW();

                    ID3D12Resource* po = (ID3D12Resource*)job.private_output;
                    if (po) {
                        D3D12_RESOURCE_DESC pd = po->GetDesc();
                        D3D12_RESOURCE_DESC plain = pd; plain.Flags = D3D12_RESOURCE_FLAG_NONE;
                        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                        UINT r2 = 0; UINT64 rs2 = 0, tot2 = 0;
                        W.dev->GetCopyableFootprints(&plain, 0, 1, 0, &fp, &r2, &rs2, &tot2);
                        D3D12_RESOURCE_DESC bd{};
                        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                        bd.Width = tot2; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
                        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
                        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                        ComPtr<ID3D12Resource>& rb2 = W.rbPriv;   // 复用，不再每帧新建 8.3MB
                        D3D12_HEAP_PROPERTIES rhRb{};
                        rhRb.Type = D3D12_HEAP_TYPE_READBACK;
                        rhRb.CreationNodeMask = 1; rhRb.VisibleNodeMask = 1;
                        if (!rb2) { W.dev->CreateCommittedResource(&rhRb, D3D12_HEAP_FLAG_NONE, &bd,
                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb2.GetAddressOf())); }
                        W.al->Reset(); W.cl->Reset(W.al.Get(), nullptr);
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = po;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        W.cl->ResourceBarrier(1, &b);
                        D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = rb2.Get();
                        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = fp;
                        D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = po;
                        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
                        W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                        W.cl->ResourceBarrier(1, &b);
                        W.cl->Close();
                        { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                        WaitW();

                        D3D12_RANGE rr{ 0, (SIZE_T)tot2 };
                        void* rm = nullptr;
                        if (SUCCEEDED(rb2->Map(0, &rr, &rm)) && rm) {
                            UINT pitch = fp.Footprint.RowPitch;
                            for (UINT y = 0; y < kNetH; ++y)
                                memcpy(&W.outBuf[(size_t)y * kNetW * 4],
                                       (unsigned char*)rm + (size_t)y * pitch, (size_t)kNetW * 4);
                            D3D12_RANGE nw{ 0, 0 };
                            rb2->Unmap(0, &nw);
                            got = true;
                        }
                    }
                    W.api.Retire(W.ctx, job.handle);
                }
            }
        }

        // 统计 edit（答案 − 喂进去的那一帧输入）
        double sum = 0; int mx = 0; size_t cnt = 0;
        if (got) {
            for (size_t i = 0; i < W.outBuf.size(); ++i) {
                if ((i & 3) == 3) continue;
                int dv = (int)W.outBuf[i] - (int)local[i];
                int ad = dv < 0 ? -dv : dv;
                sum += ad / 255.0; if (ad > mx) mx = ad; cnt++;
            }
        }
        // 运行时状态（含 passes），MV 开关变化时立刻打一条
        {
            static bool lastMv = true;
            static int lastLog = -1;
            bool mvNow = g_feedMv;
            if (mvNow != lastMv || lastLog < 0 || (int)(W.processed % 120) == 0) {
                char st[1024] = { 0 };
                if (W.api.GetStatus) W.api.GetStatus(W.ctx, st, 1024);
                Log("状态 (MV=%d, 第 %d 帧): %s", (int)mvNow, W.processed + 1, st);
                lastMv = mvNow;
                lastLog = W.processed;
            }
        }

        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(W.m);
            W.hasOut = got;
            W.cvOut.notify_all();
            W.processed++;
            W.lastMs = ms;
            W.lastEditMean = cnt ? sum / cnt : 0.0;
            W.lastEditMax = mx;
            if (W.processed <= 3 || W.processed % 120 == 0)
                Log("worker: #%d %.2f ms %s edit 平均 %.4f (%.1f/255) 峰值 %d/255",
                    W.processed, ms, got ? "" : "(无输出)", W.lastEditMean, W.lastEditMean * 255.0, W.lastEditMax);
        }
    }
}

// ---------------- 游戏侧 ----------------
static ComPtr<ID3D12Device> g_dev;
static ComPtr<ID3D12RootSignature> g_rs;
static ComPtr<ID3D12PipelineState> g_psoDownC, g_psoDownMv, g_psoComp, g_psoEma, g_psoDiff;
static ComPtr<ID3D12DescriptorHeap> g_heap;
static UINT g_inc = 0;
static ComPtr<ID3D12Resource> g_out4k, g_net, g_editSrc, g_delta, g_mv, g_edit, g_editSmooth, g_base, g_big, g_rbC, g_rbMv, g_upEdit, g_upBase;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fpC{}, g_fpMv{}, g_fpEdit{};
static UINT64 g_bytesC = 0, g_bytesMv = 0, g_bytesEdit = 0;
static std::vector<uint8_t> g_fedColor, g_fedMv, g_answer, g_editBytes;
static std::vector<uint8_t> g_fedSnapshot;   // 喂进去那一帧的内容快照（用来和答案配对）
static bool g_haveEver = false, g_pendC = false, g_pendMv = false;
static int g_frames = 0, g_applied = 0, g_slow = 0;

static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &br);
}

static void SetPass(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso,
                    UINT srv0, UINT srv1, UINT uav, UINT w, UINT h, float sx, float sy) {
    cl->SetPipelineState(pso);
    cl->SetComputeRootSignature(g_rs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE b = g_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE a1 = b; a1.ptr += (SIZE_T)srv0 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE a2 = b; a2.ptr += (SIZE_T)srv1 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE u  = b; u.ptr  += (SIZE_T)uav  * g_inc;
    cl->SetComputeRootDescriptorTable(0, a1);
    cl->SetComputeRootDescriptorTable(1, a2);
    cl->SetComputeRootDescriptorTable(2, u);
    float c[12] = { sx, sy, g_strength, (float)g_editSmoothR, g_emaStatic, g_emaMotion, g_emaK,
                    g_emaFirst ? 1.0f : 0.0f, g_emaDecay, g_emaSup, (float)g_modeIdx, (float)g_answerOnly };
    cl->SetComputeRoot32BitConstants(3, 12, c, 0);
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

static void LogAdapters(ID3D12Device* dev) {
    ComPtr<IDXGIFactory6> f2;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(f2.GetAddressOf())))) {
        for (UINT i = 0; i < 8; ++i) {
            ComPtr<IDXGIAdapter1> a1;
            if (f2->EnumAdapters1(i, a1.GetAddressOf()) != S_OK) break;
            DXGI_ADAPTER_DESC d{};
            a1->GetDesc(&d);
            Log("  适配器[%u] = %ls", i, d.Description);
        }
    }
    ComPtr<IDXGIDevice> dxdev;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)dxdev.GetAddressOf())) && dxdev) {
        ComPtr<IDXGIAdapter> adpt;
        if (SUCCEEDED(dxdev->GetAdapter(adpt.GetAddressOf())) && adpt) {
            DXGI_ADAPTER_DESC d{};
            adpt->GetDesc(&d);
            Log(">>> 游戏渲染卡 = %ls", d.Description);
        } else Log(">>> 游戏渲染卡 = GetAdapter 失败");
    } else Log(">>> 游戏渲染卡 = IDXGIDevice 失败");
}
static bool BuildResources(ID3D12Device* dev, ID3D12Resource* color, ID3D12Resource* outp) {
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    D3D12_RESOURCE_DESC cd = color->GetDesc();
    g_outW = (UINT)od.Width; g_outH = od.Height;
    g_mvScaleX = (float)kNetW / (float)cd.Width;
    g_mvScaleY = (float)kNetH / (float)cd.Height;
    {   // 核对：游戏渲染用哪张卡
        ComPtr<IDXGIDevice> dxdev;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(dxdev.GetAddressOf())))) {
            ComPtr<IDXGIAdapter> adpt;
            if (SUCCEEDED(dxdev->GetAdapter(adpt.GetAddressOf()))) {
                DXGI_ADAPTER_DESC d{};
                adpt->GetDesc(&d);
                Log("渲染卡(游戏) = %ls  LUID=%08x:%08x", d.Description,
                    (unsigned)(d.AdapterLuid.HighPart), (unsigned)(d.AdapterLuid.LowPart));
            }
        }
    }
    LogAdapters(dev);
    Log("pColor %ux%u | pOutput %ux%u fmt=%d | MV 缩放 %.4f/%.4f",
        (UINT)cd.Width, cd.Height, g_outW, g_outH, (int)od.Format, g_mvScaleX, g_mvScaleY);

    g_out4k = MakeTex(dev, g_outW, g_outH, od.Format, D3D12_RESOURCE_STATE_COPY_DEST, false);
    g_net   = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g_mv    = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R16G16B16A16_SNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g_edit  = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, false);
    g_editSmooth = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g_editSrc    = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, false);
    g_delta      = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g_base  = MakeTex(dev, g_outW, g_outH, od.Format, D3D12_RESOURCE_STATE_COPY_DEST, false);
    g_big   = MakeTex(dev, g_outW, g_outH, od.Format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    if (!g_out4k || !g_net || !g_editSrc || !g_delta || !g_mv || !g_edit || !g_editSmooth || !g_base || !g_big) { Log("纹理失败"); return false; }

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    auto foot = [&](ID3D12Resource* r, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
        D3D12_RESOURCE_DESC d = r->GetDesc();
        D3D12_RESOURCE_DESC plain = d; plain.Flags = D3D12_RESOURCE_FLAG_NONE;    // 必须清 UAV 标志
        UINT rows = 0; UINT64 rs = 0, tot = 0;
        dev->GetCopyableFootprints(&plain, 0, 1, 0, &fp, &rows, &rs, &tot);
        return tot;
    };
    g_bytesC    = foot(g_net.Get(), g_fpC);
    g_bytesMv   = foot(g_mv.Get(), g_fpMv);
    g_bytesEdit = foot(g_edit.Get(), g_fpEdit);

    auto mkbuf = [&](UINT64 n, bool upload) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = n; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(upload ? &uh : &rh, D3D12_HEAP_FLAG_NONE, &bd,
            upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        return r;
    };
    g_rbC = mkbuf(g_bytesC, false);
    g_rbMv = mkbuf(g_bytesMv, false);
    g_upEdit = mkbuf(g_bytesEdit, true);
    g_upBase = mkbuf(g_bytesEdit, true);
    g_fedColor.assign((size_t)g_bytesC, 0);
    g_fedSnapshot.assign((size_t)g_bytesC, 0);
    g_fedMv.assign((size_t)g_bytesMv, 0);
    g_answer.assign((size_t)g_bytesC, 0);
    g_editBytes.assign((size_t)g_bytesEdit, 0);

    D3D12_DESCRIPTOR_RANGE r0{}, r1{}, ru{};
    r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r0.NumDescriptors = 1; r0.BaseShaderRegister = 0;
    r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r1.NumDescriptors = 1; r1.BaseShaderRegister = 1;
    ru.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ru.NumDescriptors = 1; ru.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[4] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &r0;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &r1;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &ru;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[3].Constants.ShaderRegister = 0; rp[3].Constants.RegisterSpace = 0;
    rp[3].Constants.Num32BitValues = 12;
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sm{};
    sm.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sm.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sm.MaxLOD = D3D12_FLOAT32_MAX;
    sm.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &sm;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(g_rs.GetAddressOf())))) return false;

    struct { const char* name; ID3D12PipelineState** pso; } passes[] = {
        { "DownColorCS", &g_psoDownC }, { "DownMvCS", &g_psoDownMv }, { "CompositeCS", &g_psoComp }, { "EditEmaCS", &g_psoEma }, { "DiffCS", &g_psoDiff }
    };
    for (auto& ps : passes) {
        ComPtr<ID3DBlob> cs, e;
        if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "v8.hlsl", nullptr, nullptr, ps.name, "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e))) {
            Log("shader %s 编译失败", ps.name); return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs.Get();
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(ps.pso)))) { Log("PSO %s 失败", ps.name); return false; }
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 16;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(g_heap.GetAddressOf())))) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto mkSrv = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)slot * g_inc;
        dev->CreateShaderResourceView(r, &sd, h);
    };
    auto mkUav = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)slot * g_inc;
        dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
    };
    mkSrv(g_out4k.Get(), od.Format, 0);                    // 4K 输出拷贝（下采样源）
    mkSrv(g_net.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
    mkUav(g_net.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 2);
    mkSrv(g_mv.Get(), DXGI_FORMAT_R16G16B16A16_SNORM, 3);   // 1080p MV（EMA 用）
    mkSrv(g_editSmooth.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 9); // 平滑后的 edit（合成用）
    mkUav(g_editSmooth.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 10);
    mkUav(g_delta.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 11);
    mkSrv(g_delta.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 12);
    mkSrv(g_editSrc.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 13);
    // 槽 8：游戏 MV 的 SRV，由 EnsureMvSrv 建
    mkSrv(g_base.Get(), od.Format, 4);
    mkSrv(g_edit.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 5);
    mkUav(g_big.Get(), od.Format, 6);
    mkUav(g_mv.Get(), DXGI_FORMAT_R16G16B16A16_SNORM, 7);
    Log("游戏侧资源就绪");
    return true;
}

// MV 的 SRV 需要按游戏 MV 资源临时创建
static void EnsureMvSrv(ID3D12Device* dev, ID3D12Resource* mvRes) {
    static ID3D12Resource* cached = nullptr;
    if (cached == mvRes) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)8 * g_inc;
    dev->CreateShaderResourceView(mvRes, &sd, h);
    cached = mvRes;
    Log("MV 的 SRV 已建立在槽 8");
}

static volatile bool g_editDirty = false;
// 把「答案 − 喂进去的输入」编码成 edit 字节（行内循环，无除法），只在有新答案时调用
static void BuildEditUpload() {
    D3D12_RANGE nr{ 0, 0 };
    void* m = nullptr;
    if (FAILED(g_upEdit->Map(0, &nr, &m)) || !m) return;
    unsigned char* dst = (unsigned char*)m;
    const UINT pitch = g_fpEdit.Footprint.RowPitch;
    const UINT rowBytes = kNetW * 4;
    for (UINT y = 0; y < kNetH; ++y) {
        unsigned char* d = dst + (size_t)y * pitch;
        const unsigned char* a = &g_answer[(size_t)y * rowBytes];
        const unsigned char* f = &g_worker.inBuf[(size_t)y * rowBytes];   // 刚喂进去那一帧（同帧配对）
        for (UINT x = 0; x < rowBytes; ++x) {
            int e = g_answerOnly ? (int)a[x] : ((int)a[x] - (int)f[x] + 128);
            d[x] = (unsigned char)(e < 0 ? 0 : (e > 255 ? 255 : e));
        }
    }
    D3D12_RANGE w2{ 0, (SIZE_T)g_bytesEdit };
    g_upEdit->Unmap(0, &w2);
    g_editDirty = true;
}

extern "C" int ProxyExecute(void* ctx, void* cmdList, const void* params) {
    using Fn = int(*)(void*, void*, const void*);
    Fn real = (Fn)g_xessTab[30];
    if (!params || !cmdList) return real ? real(ctx, cmdList, params) : -1;
    if (g_disabled) return real ? real(ctx, cmdList, params) : -1;

    const unsigned __int64* q = (const unsigned __int64*)params;
    ID3D12Resource* color = (ID3D12Resource*)q[0];
    ID3D12Resource* mvRes = (ID3D12Resource*)q[1];
    ID3D12Resource* outp  = (ID3D12Resource*)q[5];
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmdList;

    static bool inited = false, gaveUp = false;
    if (gaveUp) return real ? real(ctx, cmdList, params) : -1;
    if (!inited) {
        if (!color || !outp) return real ? real(ctx, cmdList, params) : -1;
        if (FAILED(color->GetDevice(__uuidof(ID3D12Device), (void**)g_dev.GetAddressOf())) || !g_dev) { gaveUp = true; return real ? real(ctx, cmdList, params) : -1; }
        if (!BuildResources(g_dev.Get(), color, outp) || !WorkerInit()) {
            gaveUp = true; Log("!! 初始化失败，永久放弃"); return real ? real(ctx, cmdList, params) : -1;
        }
        g_worker.th = std::thread(WorkerMain);
        inited = true;
        Log("初始化完成：B 模式（底图冻结、与 edit 同帧），%s", g_apply ? "叠加开启" : "仅测量");
    }

    auto tA = Clock::now();
    ID3D12DescriptorHeap* heaps[] = { g_heap.Get() };
    cl->SetDescriptorHeaps(1, heaps);

    // ① 收上一帧回读：喂 worker + 取答案
    if (g_pendC) {
        g_pendC = false;
        D3D12_RANGE rr{ 0, (SIZE_T)g_bytesC };
        void* m = nullptr;
        if (SUCCEEDED(g_rbC->Map(0, &rr, &m)) && m) {
            UINT pitch = g_fpC.Footprint.RowPitch;
            for (UINT y = 0; y < kNetH; ++y)
                memcpy(&g_fedColor[(size_t)y * kNetW * 4], (unsigned char*)m + (size_t)y * pitch, (size_t)kNetW * 4);
            D3D12_RANGE nw{ 0, 0 }; g_rbC->Unmap(0, &nw);
        }
        // 此刻 g_net / g_out4k 里是「刚被喂进去那一帧」的内容
        //   → g_editSrc = 该帧 1080p（算 Δ / edit 基准）
        //   → g_base    = 该帧 4K（B 模式的底图，与 edit 严格同帧）
        {
            Barrier(cl, g_net.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, g_editSrc.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(g_editSrc.Get(), g_net.Get());
            Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, g_base.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(g_base.Get(), g_out4k.Get());
            Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(cl, g_net.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (g_feedMv) {                     // MV 关闭时完全不碰这 16.6MB
            g_pendMv = false;
            D3D12_RANGE rrM{ 0, (SIZE_T)g_bytesMv };
            void* mM = nullptr;
            if (SUCCEEDED(g_rbMv->Map(0, &rrM, &mM)) && mM) {
                UINT pitch = g_fpMv.Footprint.RowPitch;
                UINT rowBytes = kNetW * 8;
                for (UINT y = 0; y < kNetH; ++y)
                    memcpy(&g_fedMv[(size_t)y * rowBytes], (unsigned char*)mM + (size_t)y * pitch, rowBytes);
                D3D12_RANGE nw{ 0, 0 }; g_rbMv->Unmap(0, &nw);
            }
        }
        {
            std::unique_lock<std::mutex> lk(g_worker.m);
            g_freezeBasePending = false;   // 每帧先清，喂帧分支里会置位
        // ① 取走已完成的答案
            if (g_worker.hasOut) {
                memcpy(g_answer.data(), g_worker.outBuf.data(), (size_t)g_bytesC);
                g_worker.hasOut = false;
                g_haveEver = true;
                g_outstanding = false;
                BuildEditUpload();          // 只在有新答案时算一次（按行，无除法）
            }
            // ② 喂入本帧
            if (!g_worker.hasIn && g_worker.ok) {
                g_worker.inBuf.swap(g_fedColor);   // 零拷贝：把本帧数据换给 worker
                if (g_worker.inMv.size() == g_fedMv.size())
                    memcpy(g_worker.inMv.data(), g_fedMv.data(), (size_t)g_bytesMv);
                g_worker.hasIn = true;
                g_outstanding = true;
                g_worker.cv.notify_one();
                g_freezeBasePending = true;
            }
            // ③ 若要求同步：等这次喂入的答案（同帧内拿到，下一帧起可用）
            if (g_syncPresent && g_outstanding && !g_worker.hasOut) {
                auto w0 = Clock::now();
                bool got = g_worker.cvOut.wait_for(lk, std::chrono::milliseconds(60),
                                                  [&] { return g_worker.hasOut; });
                g_waitMs = std::chrono::duration<double, std::milli>(Clock::now() - w0).count();
                if (got && g_worker.hasOut) {
                    memcpy(g_answer.data(), g_worker.outBuf.data(), (size_t)g_bytesC);
                    g_worker.hasOut = false;
                    g_haveEver = true;
                    g_outstanding = false;
                    BuildEditUpload();
                }
            }
        }
    }

    // 若本帧喂过帧：把这一帧的 4K 冻结为底图（与 edit 严格同帧）
    if (g_freezeBasePending) {
        Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, g_base.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(g_base.Get(), g_out4k.Get());
        Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        g_freezeBasePending = false;
    }

    // ② 真 XeSS 先出 4K（之后我们才拿得到 pOutput 的内容）
    int rc = real ? real(ctx, cmdList, params) : -1;

    // ③ pOutput → 我的 4K 拷贝 → 下采样 1080p → 回读
    {
        Barrier(cl, outp, kOutStates[g_outStateIdx], D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(g_out4k.Get(), outp);
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_SOURCE, kOutStates[g_outStateIdx]);
        Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        SetPass(cl, g_psoDownC.Get(), 0, 1, 2, kNetW, kNetH, 1.0f, 1.0f);
        Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

        // 差分：本帧1080p − 上一帧1080p → g_delta
        Barrier(cl, g_net.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_editSrc.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        SetPass(cl, g_psoDiff.Get(), 1, 13, 11, kNetW, kNetH, 1.0f, 1.0f);
        Barrier(cl, g_editSrc.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(cl, g_net.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);


        D3D12_TEXTURE_COPY_LOCATION rd{}; rd.pResource = g_rbC.Get();
        rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; rd.PlacedFootprint = g_fpC;
        D3D12_TEXTURE_COPY_LOCATION rs{}; rs.pResource = g_net.Get();
        rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rs.SubresourceIndex = 0;
        cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
        Barrier(cl, g_net.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_pendC = true;

        // MV：PIXEL_SHADER_RESOURCE → NON_PIXEL 读 → 下采样 → 回读 → 复原
        if (mvRes && g_feedMv) {
            EnsureMvSrv(g_dev.Get(), mvRes);
            Barrier(cl, mvRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            SetPass(cl, g_psoDownMv.Get(), 8, 1, 7, kNetW, kNetH, g_mvScaleX, g_mvScaleY);
            Barrier(cl, mvRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            Barrier(cl, g_mv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION md{}; md.pResource = g_rbMv.Get();
            md.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; md.PlacedFootprint = g_fpMv;
            D3D12_TEXTURE_COPY_LOCATION ms{}; ms.pResource = g_mv.Get();
            ms.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; ms.SubresourceIndex = 0;
            cl->CopyTextureRegion(&md, 0, 0, 0, &ms, nullptr);
            Barrier(cl, g_mv.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    // ④ 叠加（只有 apply 才做）
    if (g_passthrough) {
        // 纯往返：当前帧 pOutput → g_out4k → 原样拷回，不叠任何修正
        Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, outp, kOutStates[g_outStateIdx], D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(outp, g_out4k.Get());
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_DEST, kOutStates[g_outStateIdx]);
        Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    } else if (g_apply && g_haveEver && g_nrOn) {
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (SUCCEEDED(g_upEdit->Map(0, &nr, &m)) && m) {
            UINT pitch = g_fpEdit.Footprint.RowPitch;
            unsigned char* dst = (unsigned char*)m;
            for (size_t i = 0, n = (size_t)kNetW * kNetH * 4; i < n; ++i) {
                int dv = (int)g_answer[i] - (int)g_fedColor[i];
                int e = dv + 128;
                if (e < 0) e = 0; if (e > 255) e = 255;
                size_t p = i / 4;
                dst[(p / kNetW) * pitch + (p % kNetW) * 4 + (i & 3)] = (unsigned char)e;
            }
            D3D12_RANGE w2{ 0, (SIZE_T)g_bytesEdit }; g_upEdit->Unmap(0, &w2);

            D3D12_TEXTURE_COPY_LOCATION dd{}; dd.pResource = g_edit.Get();
            dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ss{}; ss.pResource = g_upEdit.Get();
            ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; ss.PlacedFootprint = g_fpEdit;
            cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
            Barrier(cl, g_edit.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            // 变化门限：raw edit × 1/(1+k·Δ) → editSmooth
            Barrier(cl, g_delta.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            SetPass(cl, g_psoEma.Get(), 5, 12, 10, kNetW, kNetH, 1.0f, 1.0f);
            Barrier(cl, g_delta.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, g_editSmooth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            // B 模式：底图已在 harvest 时冻结成「与 edit 同帧」的那一帧
            Barrier(cl, g_base.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            SetPass(cl, g_psoComp.Get(), 4, 9, 6, g_outW, g_outH, 1.0f, 1.0f);   // 用平滑后的 edit
            Barrier(cl, g_big.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, outp, kOutStates[g_outStateIdx], D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION od{}; od.pResource = outp;
            od.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; od.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION os{}; os.pResource = g_big.Get();
            os.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; os.SubresourceIndex = 0;
            cl->CopyTextureRegion(&od, 0, 0, 0, &os, nullptr);
            Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_DEST, kOutStates[g_outStateIdx]);
            Barrier(cl, g_big.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, g_out4k.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(cl, g_base.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(cl, g_edit.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(cl, g_editSmooth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_applied++;
        }
    }

    g_frames++;
    double msTotal = std::chrono::duration<double, std::milli>(Clock::now() - tA).count();
    double ms = msTotal - (double)g_waitMs;                 // 等待时间不算我们的开销
    if (ms < 0) ms = 0;
    if (ms > 14.0) { if (++g_slow >= 120) { g_disabled = true; Log("!! 保险：连续 120 帧真实提交 >14ms，永久关闭"); } }
    else g_slow = 0;
    if (g_frames % 120 == 0) {
        std::lock_guard<std::mutex> lk(g_worker.m);
        Log("stats: frames=%d 叠加=%d 提交=%.2fms(净) 等待=%.2fms worker=%.2fms #%d edit=%.1f/255 峰值=%d",
            g_frames, g_applied, ms, (double)g_waitMs, g_worker.lastMs, g_worker.processed,
            g_worker.lastEditMean * 255.0, g_worker.lastEditMax);
    }
    return rc;
}

static DWORD WINAPI HotkeyThread(LPVOID) {
    bool p10 = false, p7 = false, p8 = false;
    while (true) {
        bool d10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        bool d7  = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        bool d8  = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (d10 && !p10) { g_nrOn = !g_nrOn; Log("*** F10: NR %s ***", g_nrOn ? "开" : "关"); }
        if (d7 && !p7) { g_strength = (g_strength > 0.25f) ? g_strength - 0.25f : 0.0f; Log("*** F7: 强度 %.2f ***", g_strength); }
        if (d8 && !p8) { g_strength = (g_strength < 4.0f) ? g_strength + 0.25f : 4.0f; Log("*** F8: 强度 %.2f ***", g_strength); }
        static bool p9 = false;
        bool d9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (d9 && !p9) {
            static int mode = 0;
            mode = (mode + 1) % 4;
            if (mode == 0)      { g_emaStatic = 1.0f;  g_modeIdx = 0; Log("*** F9: EMA 关（左上无条）***"); }
            else if (mode == 1) { g_emaStatic = 0.15f; g_modeIdx = 1; Log("*** F9: EMA 默认 alpha0.15（左上 1 绿）***"); }
            else if (mode == 2) { g_emaStatic = 0.05f; g_modeIdx = 2; Log("*** F9: EMA 强 alpha0.05（左上 2 黄）***"); }
            else                { g_emaStatic = 0.02f; g_modeIdx = 3; Log("*** F9: EMA 极强 alpha0.02（左上 3 红）***"); }
        }
        p9 = d9;
        static bool p2 = false;
        bool d2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        if (d2 && !p2) {
            g_outStateIdx = (g_outStateIdx + 1) % 5;
            Log("*** F2: pOutput 假定状态 = %s ***", kOutStateNames[g_outStateIdx]);
        }
        p2 = d2;
        static bool p3 = false;
        bool d3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (d3 && !p3) {
            g_passthrough = !g_passthrough;
            Log("*** F3: %s ***", g_passthrough ? "纯往返测试（当前帧拷出再拷回，无修正）" : "正常");
        }
        p3 = d3;
        static bool p4 = false;
        bool d4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (d4 && !p4) {
            g_answerOnly = !g_answerOnly;
            Log("*** F4: %s ***", g_answerOnly ? "只呈现网络答案（会变软，但不滞后）" : "正常：底图 + 修正量");
        }
        p4 = d4;
        static bool p5 = false;
        bool d5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (d5 && !p5) { g_feedMv = !g_feedMv; Log("*** F5: 运动矢量 %s ***", g_feedMv ? "开（慢/质量高）" : "关（快）"); }
        p5 = d5;
        p10 = d10; p7 = d7; p8 = d8;
        Sleep(40);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        OpenLog();
        Log("libxess proxy v8（输入=pOutput4K+MV）attached");
        {
            std::string off = ModuleDir() + "xess_proxy_off.txt";
            if (GetFileAttributesA(off.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_disabled = true;
                Log("检测到 xess_proxy_off.txt：完全不接管");
            }
            std::string nomv = ModuleDir() + "xess_proxy_nomv.txt";
            if (GetFileAttributesA(nomv.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_feedMv = false;
                Log("检测到 xess_proxy_nomv.txt：启动时运动矢量关闭");
            }
            std::string sy = ModuleDir() + "xess_proxy_sync.txt";
            if (GetFileAttributesA(sy.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_syncPresent = true;
                Log("检测到 xess_proxy_sync.txt：present 阻塞等待 NR");
            } else {
                Log("非阻塞模式（B 模式底图与 edit 同帧，答案晚到也不残影）");
            }
            std::string ap = ModuleDir() + "xess_proxy_apply.txt";
            if (GetFileAttributesA(ap.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_apply = true;
                Log("检测到 xess_proxy_apply.txt：允许把 edit 叠回画面");
            } else {
                Log("没有 xess_proxy_apply.txt：只测量，不改画面");
            }
        }
        InitForwarders();
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
        Log("转发就绪");
    }
    return TRUE;
}
