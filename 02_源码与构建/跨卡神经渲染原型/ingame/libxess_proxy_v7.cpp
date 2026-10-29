// libxess_proxy v7 —— 跨卡神经渲染（修正「NR 对画面没有改动」）
//
// 与 v6 的关键差异（全部由离线探针 nr_probe_output / nr_probe_abi 实测决定）：
//  1) 网络结果只存在于 job.private_output，fi.color 逐字节不变 —— v3~v6 回读的是原图，
//     所以 edit ≡ 0，画面当然不动。v7 改成回读 private_output。
//  2) 网络输入改成「XeSS 4K 输出 → 1080p 盒式下采样」（探针证明 >1920x1080 的颜色会被
//     codec 拒绝；pColor 是上采样前的糊图，喂它等于让网络修一张已经糊掉的图）。
//  3) 叠加方式改成 UAV 原地写 outp：不需要猜 outp 当前是什么 state，也不再有 4K 中间纹理。
//  4) 传输的是「本帧输出 − 本帧输入」的编码差分（±0.5 量程 / 1/255 步长），
//     彻底消掉 v6 里「N-2 帧结果减 N-1 帧输入」的时间错位。
//  5) 接真 motion（实测叫 passes=1 → 3，edit 幅度 0.0265 → 0.0312、峰值 0.17 → 0.40）；
//     depth 也接通但默认关（无 history 时实测惰性）。
//  6) 启动时做一次 GPU 自检：验证 R10G10B10A2 的 UAV 读改写真的生效，结果写进日志。
//
// 结构：pd-upscaler → 真 XeSS 出 4K → ① 下采样 1080p → 回读 → 工作线程（9070 XT）
//       ② 工作线程跑 lmxxf（输入 1080p + motion）→ 回读 private_output → 算差分
//       ③ 下一帧：上传差分 → UAV 原地叠到 outp
//
// 热键：F10 NR 开关 / F7 F8 强度 / F6 增益 ×8（可见性自证） / F5 关掉 motion /
//       F4 打开 depth / F3 绿色标记（通路自证）/ F9 Look

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <share.h>
#include <emmintrin.h>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

// ---------------- 转发 ----------------
extern "C" void* g_xessTab[72] = {};
static const char* kNames[72] = {
#include "xess_fwd_names.inc"
};

static HMODULE g_self = nullptr;
static HMODULE g_real = nullptr;
static FILE* g_log = nullptr;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static std::string ModuleDir() {
    char path[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    if (!n) return std::string();
    std::string s(path);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? std::string() : s.substr(0, p + 1);
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
    if (!g_real) Log("!! 加载 %s 失败 (err=%lu)", s.c_str(), GetLastError());
    return g_real;
}

static void InitForwarders() {
    HMODULE m = RealXess();
    for (int i = 0; i < 72; ++i) {
        g_xessTab[i] = m ? (void*)GetProcAddress(m, kNames[i]) : nullptr;
        if (!g_xessTab[i]) Log("!! 缺少导出: %s", kNames[i]);
    }
}

// ---------------- lmxxf 接口 ----------------
struct ApiTable {
    uint32_t struct_size;
    uint32_t abi_version;
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
    void* slot18;
    void* slot19;
};

// 160 字节版：base(104) + depth + velocity + 5 个控制量 + 历史掩码（我们暂不用掩码）
struct FrameInfo160 {
    uint32_t struct_size;
    uint32_t pad0;
    uint64_t session_id;
    uint64_t frame_id;
    uint64_t list_generation;
    void* command_list;
    uint32_t color_width;
    uint32_t color_height;
    void* color;
    uint32_t color_state;
    uint32_t flags;
    float transfer_strength;
    float color_strength;
    uint32_t debug_view;
    float model_scale;
    void* exposure;
    uint32_t exposure_state;
    float pre_exposure;
    float exposure_scale;
    void* depth;              // +104
    void* velocity;           // +112
    float ctlTone;            // +120
    float ctlStructure;       // +124
    float ctlSkin;            // +128
    float ctlOther;           // +132
    float ctlStyle;           // +136
    uint32_t ctlPad;          // +140
    void* historyMask;        // +144
    void* reserved152;        // +152
};
static_assert(sizeof(FrameInfo160) == 160, "FrameInfo must be 160 bytes (controls + history mask)");

struct Job { uint32_t struct_size; void* handle; void* private_output; };
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; void* reserved0; };

// ---------------- 常量 ----------------
static UINT kNetW = 1920;
static UINT kNetH = 1080;
static UINT g_outW = 0, g_outH = 0;

// ---------------- shader ----------------
static const char* kHLSL = R"(
cbuffer Params : register(b0) { float gStrength; float gSat; float gGamma; float gHiCut; float4 gPad; };
Texture2D<float4>      gSrc  : register(t0);   // 通用源（pColor / motion / depth）
Texture2D<float4>      gEdit : register(t1);   // 编码后的差分
SamplerState           gSmp  : register(s0);
RWTexture2D<float4>    gDst  : register(u0);   // 1080p 目标
RWTexture2D<float4>    gOut  : register(u1);   // outp（最终画面，UAV 读改写）

// 通用双线性重采样（motion / depth 降采样用）
[numthreads(8, 8, 1)]
void ResampleCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    gDst[id.xy] = gSrc.SampleLevel(gSmp, uv, 0);
}

// outp(4K) → gDst(1080p)：2x2 盒式下采样（UAV 读，避开 outp 的状态问题）
[numthreads(8, 8, 1)]
void CaptureCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint ow, oh; gOut.GetDimensions(ow, oh);
    uint2 p = id.xy * 2u;
    float4 c = gOut[p];
    if (p.x + 1u < ow)              c += gOut[p + uint2(1, 0)];
    if (p.y + 1u < oh)              c += gOut[p + uint2(0, 1)];
    if (p.x + 1u < ow && p.y + 1u < oh) c += gOut[p + uint2(1, 1)];
    gDst[id.xy] = c * 0.25;
}

// 把差分原地叠到 outp 上（UAV 读改写；不需要任何资源状态转换）
[numthreads(8, 8, 1)]
void EditCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float4 base = gOut[id.xy];
    float3 e = (gEdit.SampleLevel(gSmp, uv, 0).rgb - 0.5) * gStrength;
    float3 c = saturate(base.rgb + e);
    // Look 层（对应 AMDNR 的 [AmdLook]）
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(lum, lum, lum), c, gSat);
    c = pow(saturate(c), float3(gGamma, gGamma, gGamma));
    c = c / (1.0 + max(0.0, c - 0.75) * gHiCut);
    gOut[id.xy] = float4(c, base.a);
}

// 绿色标记：验证「我们的 dispatch 真能改到屏幕上的 outp」
[numthreads(8, 8, 1)]
void MarkCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 240u || id.y >= 240u) return;
    gOut[id.xy] = float4(0.0, 1.0, 0.0, 1.0);
}
)";

// ---------------- 工作线程（9070 XT） ----------------
struct Worker {
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> inBuf;          // 1080p RGBA8（来自 XeSS 输出下采样，紧凑行）
    std::vector<uint8_t> inMv;           // 1080p SNORM RGBA16（可空）
    std::vector<uint8_t> inDep;          // 1080p R32F（可空）
    std::vector<uint8_t> outEdit;        // 1080p RGBA8 编码差分
    bool hasIn = false, hasOut = false, quit = false;
    bool useMv = true, useDep = false;
    int processed = 0;
    double lastMs = 0, lastEditMean = 0, lastEditMax = 0;
    int poW = 0, poH = 0;

    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fe;
    UINT64 fv = 0;
    ComPtr<ID3D12Resource> color, up, rbPo;
    ComPtr<ID3D12Resource> mv, mvUp, dep, depUp;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpColor{}, fpMv{}, fpDep{}, fpPo{};
    UINT64 bytesColor = 0, bytesMv = 0, bytesDep = 0, bytesPo = 0;
    UINT rowColor = 0, rowMv = 0, rowDep = 0, rowPo = 0;
    HMODULE mod = nullptr;
    ApiTable api{};
    void* ctx = nullptr;
    bool ok = false;
};

static Worker g_worker;

static void WaitWorker() {
    Worker& W = g_worker;
    W.q->Signal(W.fe.Get(), ++W.fv);
    while (W.fe->GetCompletedValue() < W.fv) Sleep(0);
}

static bool WorkerInit() {
    Worker& W = g_worker;
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), (void**)fac.GetAddressOf()))) return false;
    ComPtr<IDXGIAdapter1> ad;
    if (fac->EnumAdapters1(1, &ad) != S_OK) { Log("worker: 适配器1缺失"); return false; }
    DXGI_ADAPTER_DESC d{};
    ad->GetDesc(&d);
    Log("worker: 适配器 = %ls", d.Description);
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)W.dev.GetAddressOf()))) { Log("worker: 设备失败"); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    W.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)W.q.GetAddressOf());
    W.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)W.al.GetAddressOf());
    W.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, W.al.Get(), nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)W.cl.GetAddressOf());
    W.cl->Close();
    W.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)W.fe.GetAddressOf());

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    auto mkTex = [&](UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES st,
                     D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT64& bytes, UINT& rowBytes,
                     ComPtr<ID3D12Resource>& tex, ComPtr<ID3D12Resource>& upRes) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = fmt; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(W.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, st, nullptr,
            __uuidof(ID3D12Resource), (void**)tex.GetAddressOf()))) return false;
        D3D12_RESOURCE_DESC plain = td;
        plain.Flags = D3D12_RESOURCE_FLAG_NONE;
        UINT rows = 0; UINT64 rowSize = 0, total = 0;
        W.dev->GetCopyableFootprints(&plain, 0, 1, 0, &fp, &rows, &rowSize, &total);
        bytes = total;
        rowBytes = (UINT)((size_t)w * (size_t)(fmt == DXGI_FORMAT_R16G16B16A16_SNORM ? 8 : 4));
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)upRes.GetAddressOf()));
    };

    if (!mkTex(kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON,
               W.fpColor, W.bytesColor, W.rowColor, W.color, W.up)) { Log("worker: color 失败"); return false; }
    if (!mkTex(kNetW, kNetH, DXGI_FORMAT_R16G16B16A16_SNORM, D3D12_RESOURCE_STATE_COMMON,
               W.fpMv, W.bytesMv, W.rowMv, W.mv, W.mvUp)) { Log("worker: mv 失败"); return false; }
    if (!mkTex(kNetW, kNetH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COMMON,
               W.fpDep, W.bytesDep, W.rowDep, W.dep, W.depUp)) { Log("worker: depth 失败"); return false; }
    W.inBuf.assign((size_t)kNetW * kNetH * 4, 0);
    W.outEdit.assign((size_t)kNetW * kNetH * 4, 0);
    W.inMv.assign((size_t)kNetW * kNetH * 8, 0);
    W.inDep.assign((size_t)kNetW * kNetH * 4, 0);
    Log("worker: 缓冲 color=%llu mv=%llu dep=%llu", (unsigned long long)W.bytesColor,
        (unsigned long long)W.bytesMv, (unsigned long long)W.bytesDep);

    std::string dir = ModuleDir();
    std::wstring pak(dir.begin(), dir.end());
    pak += L"LmxxfNrRuntime.pak";
    std::wstring dllPath = pak;
    size_t slash = dllPath.find_last_of(L'\\');
    if (slash != std::wstring::npos) dllPath = dllPath.substr(0, slash + 1);
    dllPath += L"LmxxfNrRuntime.dll";

    W.mod = LoadLibraryW(dllPath.c_str());
    if (!W.mod) { Log("worker: lmxxf DLL 加载失败"); return false; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(W.mod, "LmxxfNrGetApi");
    W.api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!getApi || getApi(1, &W.api) != 0) { Log("worker: GetApi 失败"); return false; }

    CreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = W.dev.Get();
    ci.queue = W.q.Get();
    ci.assets_directory = pak.c_str();
    if (W.api.Create(&ci, &W.ctx) != 0) { Log("worker: Create 失败"); return false; }
    if (W.api.PrepareSession(W.ctx) != 0) { Log("worker: PrepareSession 失败"); return false; }
    Log("worker: lmxxf 会话就绪 (%ux%u)", kNetW, kNetH);
    W.ok = true;
    return true;
}

// 上传紧凑行数据到纹理（行距不一致时逐行搬）
static void UploadTight(ID3D12Resource* up, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                        UINT rowBytes, UINT h, const std::vector<uint8_t>& src) {
    D3D12_RANGE nr{ 0, 0 };
    void* m = nullptr;
    if (FAILED(up->Map(0, &nr, &m)) || !m) return;
    UINT pitch = fp.Footprint.RowPitch;
    if (pitch == rowBytes) {
        memcpy(m, src.data(), (size_t)rowBytes * h);
    } else {
        for (UINT y = 0; y < h; ++y)
            memcpy((uint8_t*)m + (size_t)y * pitch, src.data() + (size_t)y * rowBytes, rowBytes);
    }
    D3D12_RANGE wr{ 0, (SIZE_T)pitch * h };
    up->Unmap(0, &wr);
}

// 编码差分：enc = clamp(out - in + 128)；用 SSE2 精确处理（16 位展开，避免越界回绕）
static void EncodeDiff(const uint8_t* in, const uint8_t* out, uint8_t* enc, size_t n) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i half = _mm_set1_epi16(128);
    size_t i = 0;
    size_t n16 = n & ~(size_t)15;
    for (; i < n16; i += 16) {
        __m128i a = _mm_loadu_si128((const __m128i*)(in + i));
        __m128i b = _mm_loadu_si128((const __m128i*)(out + i));
        __m128i aLo = _mm_unpacklo_epi8(a, zero), aHi = _mm_unpackhi_epi8(a, zero);
        __m128i bLo = _mm_unpacklo_epi8(b, zero), bHi = _mm_unpackhi_epi8(b, zero);
        __m128i dLo = _mm_add_epi16(_mm_sub_epi16(bLo, aLo), half);
        __m128i dHi = _mm_add_epi16(_mm_sub_epi16(bHi, aHi), half);
        dLo = _mm_max_epi16(_mm_min_epi16(dLo, _mm_set1_epi16(255)), zero);
        dHi = _mm_max_epi16(_mm_min_epi16(dHi, _mm_set1_epi16(255)), zero);
        __m128i packed = _mm_packus_epi16(dLo, dHi);
        _mm_storeu_si128((__m128i*)(enc + i), packed);
    }
    for (; i < n; ++i) {
        int d = (int)out[i] - (int)in[i] + 128;
        if (d < 0) d = 0; if (d > 255) d = 255;
        enc[i] = (uint8_t)d;
    }
}

static bool InitGameSide();   // 定义在下面（工作线程里调用，避免在渲染线程阻塞）

static void WorkerMain() {
    Worker& W = g_worker;
    std::vector<uint8_t> localIn;
    bool fbLogged = false;

    // ---- 初始化全部在工作线程完成（不在渲染线程的 xessD3D12Execute 里阻塞）----
    if (!InitGameSide()) return;

    while (true) {
        bool useMv = true, useDep = false;
        {
            std::unique_lock<std::mutex> lk(W.m);
            W.cv.wait(lk, [&] { return W.quit || W.hasIn; });
            if (W.quit) break;
            localIn = W.inBuf;
            useMv = W.useMv;
            useDep = W.useDep;
            W.hasIn = false;
        }
        auto t0 = Clock::now();

        UploadTight(W.up.Get(), W.fpColor, W.rowColor, kNetH, localIn);
        if (useMv) { std::lock_guard<std::mutex> lk(W.m); UploadTight(W.mvUp.Get(), W.fpMv, W.rowMv, kNetH, W.inMv); }
        if (useDep) { std::lock_guard<std::mutex> lk(W.m); UploadTight(W.depUp.Get(), W.fpDep, W.rowDep, kNetH, W.inDep); }

        // 上传到 color / mv / depth 纹理
        W.al->Reset();
        W.cl->Reset(W.al.Get(), nullptr);
        {
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = W.color.Get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = W.up.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint = W.fpColor;
            W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            if (useMv) {
                d.pResource = W.mv.Get();
                s.pResource = W.mvUp.Get();
                s.PlacedFootprint = W.fpMv;
                W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            }
            if (useDep) {
                d.pResource = W.dep.Get();
                s.pResource = W.depUp.Get();
                s.PlacedFootprint = W.fpDep;
                W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            }
        }
        W.cl->Close();
        { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
        WaitWorker();

        Job job{};
        job.struct_size = (uint32_t)sizeof(Job);
        FrameInfo160 fi{};
        fi.struct_size = (uint32_t)sizeof(FrameInfo160);
        fi.session_id = 1;
        fi.frame_id = (uint64_t)W.processed + 1;
        fi.list_generation = 1;
        fi.command_list = W.cl.Get();
        fi.color_width = kNetW;
        fi.color_height = kNetH;
        fi.color = W.color.Get();
        fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        fi.transfer_strength = 1.0f;
        fi.color_strength = 1.0f;
        fi.model_scale = 1.0f;
        fi.pre_exposure = 1.0f;
        fi.exposure_scale = 1.0f;
        fi.depth = useDep ? W.dep.Get() : nullptr;
        fi.velocity = useMv ? W.mv.Get() : nullptr;
        fi.ctlTone = 1.0f;        // 与 AMDNR 日志实测一致：ctl=1,1,3,2,0
        fi.ctlStructure = 1.0f;
        fi.ctlSkin = 3.0f;
        fi.ctlOther = 2.0f;
        fi.ctlStyle = 0.0f;
        fi.historyMask = nullptr;

        int32_t rc = W.api.PrepareFrame(W.ctx, &fi, &job);
        if (rc != 0) {
            static int logged = 0;
            if (logged < 5) {
                char eb[256] = { 0 };
                if (W.api.GetLastError) W.api.GetLastError(eb, 256);
                Log("worker: PrepareFrame 失败 rc=%d err=%s", (int)rc, eb);
                logged++;
            }
            std::lock_guard<std::mutex> lk(W.m);
            W.processed++;
            continue;
        }

        // private_output 几何
        ID3D12Resource* po = (ID3D12Resource*)job.private_output;
        if (po) {
            D3D12_RESOURCE_DESC pd = po->GetDesc();
            if ((int)pd.Width != W.poW || (int)pd.Height != W.poH) {
                W.poW = (int)pd.Width; W.poH = (int)pd.Height;
                Log("worker: private_output = %dx%d fmt=%d flags=0x%llx",
                    W.poW, W.poH, (int)pd.Format, (unsigned long long)pd.Flags);
            }
            if (!W.rbPo) {
                D3D12_RESOURCE_DESC plain = pd;
                plain.Flags = D3D12_RESOURCE_FLAG_NONE;
                UINT rows = 0; UINT64 rowSize = 0, total = 0;
                W.dev->GetCopyableFootprints(&plain, 0, 1, 0, &W.fpPo, &rows, &rowSize, &total);
                W.bytesPo = total;
                W.rowPo = (UINT)((size_t)pd.Width * 4);
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
                bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                D3D12_HEAP_PROPERTIES rh{};
                rh.Type = D3D12_HEAP_TYPE_READBACK;
                rh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                rh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                rh.CreationNodeMask = 1; rh.VisibleNodeMask = 1;
                W.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)W.rbPo.GetAddressOf());
            }
        }

        W.al->Reset();
        W.cl->Reset(W.al.Get(), nullptr);
        bool ran = false;
        if (W.api.RecordInputs(W.ctx, job.handle, W.cl.Get()) == 0) {
            W.cl->Close();
            { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
            WaitWorker();
            if (W.api.ExecuteAfterProducer(W.ctx, job.handle, W.q.Get()) == 0) {
                W.al->Reset();
                W.cl->Reset(W.al.Get(), nullptr);
                if (W.api.RecordOutputs(W.ctx, job.handle, W.cl.Get()) == 0) {
                    // 关键：结果在 private_output，读它（要在 Retire 之前）
                    if (po && W.rbPo) {
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = po;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        W.cl->ResourceBarrier(1, &b);
                        D3D12_TEXTURE_COPY_LOCATION d{};
                        d.pResource = W.rbPo.Get();
                        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        d.PlacedFootprint = W.fpPo;
                        D3D12_TEXTURE_COPY_LOCATION s{};
                        s.pResource = po;
                        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        s.SubresourceIndex = 0;
                        W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                        W.cl->ResourceBarrier(1, &b);
                    }
                    W.cl->Close();
                    { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                    WaitWorker();
                    W.api.Retire(W.ctx, job.handle);
                    ran = true;
                }
            }
        }

        std::vector<uint8_t> edit((size_t)kNetW * kNetH * 4, 128);
        double meanEdit = 0, maxEdit = 0;
        if (ran && po && W.rbPo && W.poW == (int)kNetW && W.poH == (int)kNetH) {
            D3D12_RANGE rr{ 0, (SIZE_T)W.bytesPo };
            void* m = nullptr;
            if (SUCCEEDED(W.rbPo->Map(0, &rr, &m)) && m) {
                std::vector<uint8_t> out((size_t)kNetW * kNetH * 4);
                UINT pitch = W.fpPo.Footprint.RowPitch;
                for (UINT y = 0; y < kNetH; ++y)
                    memcpy(&out[(size_t)y * W.rowPo], (uint8_t*)m + (size_t)y * pitch, W.rowPo);
                D3D12_RANGE nw{ 0, 0 };
                W.rbPo->Unmap(0, &nw);
                EncodeDiff(localIn.data(), out.data(), edit.data(), edit.size());
                // 统计真实 edit 幅度（0..1 单位），作为「网络在干活」的自证
                double sum = 0; size_t cnt = 0;
                for (size_t i = 0; i < out.size(); ++i) {
                    if ((i & 3) == 3) continue;
                    int d = (int)out[i] - (int)localIn[i];
                    if (d < 0) d = -d;
                    sum += (double)d / 255.0;
                    if ((double)d / 255.0 > maxEdit) maxEdit = (double)d / 255.0;
                    cnt++;
                }
                meanEdit = cnt ? sum / (double)cnt : 0.0;
            }
        } else if (!fbLogged) {
            fbLogged = true;
            Log("worker: private_output 不可用或几何不符（po=%p %dx%d），差分保持中性", (void*)po, W.poW, W.poH);
        }

        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(W.m);
            W.outEdit = edit;
            W.hasOut = true;
            W.processed++;
            W.lastMs = ms;
            W.lastEditMean = meanEdit;
            W.lastEditMax = maxEdit;
            if (W.processed <= 3 || W.processed % 120 == 0)
                Log("worker: frame %d 完成 %.2f ms | 网络 edit 平均 %.4f 最大 %.4f | mv=%d dep=%d",
                    W.processed, ms, meanEdit, maxEdit, (int)useMv, (int)useDep);
        }
    }
}

// ---------------- 游戏侧（7900 XT） ----------------
static ComPtr<ID3D12Device> g_dev;
static ComPtr<ID3D12RootSignature> g_rs;
static ComPtr<ID3D12PipelineState> g_psoResample, g_psoCapture, g_psoEdit, g_psoMark;
static ComPtr<ID3D12DescriptorHeap> g_heap;
static UINT g_inc = 0;

static ComPtr<ID3D12Resource> g_net;        // 1080p 抓帧目标（UAV）+ 回读源
static ComPtr<ID3D12Resource> g_netEdit;    // 上传的编码差分（SRV）
static ComPtr<ID3D12Resource> g_mv1080, g_dep1080;
static ComPtr<ID3D12Resource> g_upNet, g_rbNet[2];
static ComPtr<ID3D12Resource> g_upMv, g_rbMv, g_upDep, g_rbDep;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fpNet{}, g_fpMv{}, g_fpDep{};
static UINT64 g_bytesNet = 0, g_bytesMv = 0, g_bytesDep = 0;
static int g_rbIdx = 0;
static bool g_pending[2] = { false, false };
static bool g_pendMv = false, g_pendDep = false;

static std::vector<uint8_t> g_edit;         // 当前要叠加的编码差分
static std::vector<uint8_t> g_mvHost, g_depHost;
static bool g_haveEdit = false;

static ComPtr<ID3D12Resource> g_outpUav;    // outp（UAV）
static int g_outpSlot = 7;
static bool g_outpUavOk = false;
static ComPtr<ID3D12Resource> g_color0, g_outp0;   // 首帧拿到的 pColor / pOutput
static volatile bool g_resReady = false;           // 工作线程初始化完成

static volatile bool g_nrOn = true;
static volatile float g_strength = 1.0f;
static volatile float g_boost = 1.0f;
static volatile float g_lookSat = 1.0f, g_lookGamma = 1.0f, g_lookHiCut = 0.0f;
static volatile bool g_feedMv = true, g_feedDep = false;
static volatile int g_markFrames = 0;
// 分级开关（xess_proxy_level.txt）：
//   0/1 = 只调真 XeSS，连初始化都不做（验证「不是我崩的」）
//   2   = 只做初始化（建资源 + worker 会话），每帧不录任何命令
//   3   = 只抓帧（UAV 读 outp → 1080p → 回读）
//   4   = 只 motion（对游戏 MV 做状态转换 → 1080p → 回读）
//   5   = 抓帧 + motion
//   6   = 再加 worker 推理
//   7   = 全开（默认）
static volatile int g_level = 7;
static bool g_disabled = false;
static int g_frames = 0, g_applied = 0;
static double g_sumSubmit = 0, g_sumWorker = 0;
static int g_nSample = 0, g_nWorker = 0;

// 回读缓冲（带行距）→ 紧凑数组
static void UnpackTight(const void* mapped, UINT pitch, UINT rowBytes, UINT h, std::vector<uint8_t>& dst) {
    dst.resize((size_t)rowBytes * h);
    if (pitch == rowBytes) {
        memcpy(dst.data(), mapped, dst.size());
    } else {
        for (UINT y = 0; y < h; ++y)
            memcpy(dst.data() + (size_t)y * rowBytes, (const uint8_t*)mapped + (size_t)y * pitch, rowBytes);
    }
}

static void SetPass(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso,
                    UINT srv0, UINT srv1, UINT uavSlot, UINT uav1Slot, float strength) {
    cl->SetPipelineState(pso);
    cl->SetComputeRootSignature(g_rs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE base = g_heap->GetGPUDescriptorHandleForHeapStart();
    auto at = [&](UINT slot) { D3D12_GPU_DESCRIPTOR_HANDLE h = base; h.ptr += (SIZE_T)slot * g_inc; return h; };
    cl->SetComputeRootDescriptorTable(0, at(srv0));
    cl->SetComputeRootDescriptorTable(1, at(srv1));
    cl->SetComputeRootDescriptorTable(2, at(uavSlot));
    cl->SetComputeRootDescriptorTable(3, at(uav1Slot));
    float c[8] = { strength, g_lookSat, g_lookGamma, g_lookHiCut, 0, 0, 0, 0 };
    cl->SetComputeRoot32BitConstants(4, 8, c, 0);
}

static void UavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}

static void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
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

static bool BuildResources(ID3D12Device* dev, ID3D12Resource* color, ID3D12Resource* outp);

static bool InitGameSide() {
    bool okRes = (g_level == 9) ? true : BuildResources(g_dev.Get(), g_color0.Get(), g_outp0.Get());
    bool okWorker = false;
    if (okRes && g_level != 8) okWorker = WorkerInit();
    bool okAll = okRes && (g_level == 8 ? true : okWorker);
    Log("工作线程初始化：资源=%d worker=%d → %s", (int)okRes, (int)okWorker,
        okAll ? "开始跨卡 NR（结果取自 private_output）" : "放弃集成");
    if (okAll) g_resReady = true;
    return okAll;
}

static void MkSrv(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * g_inc;
    dev->CreateShaderResourceView(r, &sd, h);
}

static void MkUav(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = fmt;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * g_inc;
    dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
}

// 槽位：0 pColor SRV / 1 g_netEdit SRV / 2 g_net SRV / 3 mv SRV / 4 g_net UAV
//       5 g_mv1080 UAV / 6 g_dep1080 UAV / 7 inp UAV / 8 mv1080 SRV / 9 dep1080 SRV
enum { SLOT_PCOLOR = 0, SLOT_EDIT = 1, SLOT_NET_SRV = 2, SLOT_MV = 3, SLOT_NET_UAV = 4,
       SLOT_MV1080_UAV = 5, SLOT_DEP1080_UAV = 6, SLOT_OUTP_UAV = 7,
       SLOT_MV1080_SRV = 8, SLOT_DEP1080_SRV = 9, SLOT_COUNT = 12 };

static bool BuildResources(ID3D12Device* dev, ID3D12Resource* color, ID3D12Resource* outp) {
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    kNetW = 1920; kNetH = 1080;    // 探针实测：codec 的像素预算上限就是 1080p 这一档
    g_outW = (UINT)od.Width;
    g_outH = od.Height;

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kNetW; td.Height = kNetH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    {
        D3D12_RESOURCE_DESC d = td;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)g_net.GetAddressOf()))) { Log("g_net 失败"); return false; }
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, __uuidof(ID3D12Resource), (void**)g_netEdit.GetAddressOf()))) { Log("g_netEdit 失败"); return false; }
        D3D12_RESOURCE_DESC dmv = td;
        dmv.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
        dmv.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &dmv,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)g_mv1080.GetAddressOf()))) { Log("g_mv1080 失败"); return false; }
        D3D12_RESOURCE_DESC ddep = td;
        ddep.Format = DXGI_FORMAT_R32_FLOAT;
        ddep.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &ddep,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)g_dep1080.GetAddressOf()))) { Log("g_dep1080 失败"); return false; }
    }
    // 缓冲
    {
        D3D12_RESOURCE_DESC d = td;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        UINT rows = 0; UINT64 rowSize = 0, total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &g_fpNet, &rows, &rowSize, &total);
        g_bytesNet = total;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)g_upNet.GetAddressOf());
        for (int i = 0; i < 2; ++i)
            dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)g_rbNet[i].GetAddressOf());
        // MV 1080p（SNORM，8 字节/像素）
        D3D12_RESOURCE_DESC dmv = td;
        dmv.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
        UINT r2 = 0; UINT64 rs2 = 0, t2 = 0;
        dev->GetCopyableFootprints(&dmv, 0, 1, 0, &g_fpMv, &r2, &rs2, &t2);
        g_bytesMv = t2;
        D3D12_RESOURCE_DESC bmv = bd; bmv.Width = t2;
        dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bmv, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)g_upMv.GetAddressOf());
        dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bmv, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)g_rbMv.GetAddressOf());
        D3D12_RESOURCE_DESC ddep = td;
        ddep.Format = DXGI_FORMAT_R32_FLOAT;
        dev->GetCopyableFootprints(&ddep, 0, 1, 0, &g_fpDep, &r2, &rs2, &t2);
        g_bytesDep = t2;
        D3D12_RESOURCE_DESC bdep = bd; bdep.Width = t2;
        dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bdep, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)g_upDep.GetAddressOf());
        dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bdep, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)g_rbDep.GetAddressOf());
        g_edit.assign((size_t)kNetW * kNetH * 4, 128);
        g_mvHost.assign((size_t)kNetW * kNetH * 8, 0);
        g_depHost.assign((size_t)kNetW * kNetH * 4, 0);
        Log("缓冲: net=%llu mv=%llu dep=%llu 字节", (unsigned long long)g_bytesNet,
            (unsigned long long)g_bytesMv, (unsigned long long)g_bytesDep);
    }

    // 根签名：t0 / t1 / u0 / u1 各一张表 + 32 位常量
    D3D12_DESCRIPTOR_RANGE rT0{}, rT1{}, rU0{}, rU1{};
    rT0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rT0.NumDescriptors = 1; rT0.BaseShaderRegister = 0;
    rT1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rT1.NumDescriptors = 1; rT1.BaseShaderRegister = 1;
    rU0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rU0.NumDescriptors = 1; rU0.BaseShaderRegister = 0;
    rU1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rU1.NumDescriptors = 1; rU1.BaseShaderRegister = 1;
    D3D12_ROOT_PARAMETER rp[5] = {};
    for (int i = 0; i < 4; ++i) {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i].DescriptorTable.NumDescriptorRanges = 1;
        rp[i].DescriptorTable.pDescriptorRanges = i == 0 ? &rT0 : (i == 1 ? &rT1 : (i == 2 ? &rU0 : &rU1));
        rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[4].Constants.ShaderRegister = 0;
    rp[4].Constants.Num32BitValues = 8;
    rp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sm{};
    sm.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sm.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sm.MaxLOD = D3D12_FLOAT32_MAX;
    sm.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 5; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &sm;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { Log("根签名序列化失败"); return false; }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)g_rs.GetAddressOf()))) { Log("根签名失败"); return false; }

    struct { const char* entry; ComPtr<ID3D12PipelineState>* pso; } passes[] = {
        { "ResampleCS",   &g_psoResample },
        { "CaptureCS",    &g_psoCapture },
        { "EditCS",       &g_psoEdit },
        { "MarkCS",       &g_psoMark },
    };
    for (auto& p : passes) {
        ComPtr<ID3DBlob> cs, e2;
        if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "x.hlsl", nullptr, nullptr, p.entry, "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
            Log("shader %s 编译失败: %s", p.entry, e2 ? (const char*)e2->GetBufferPointer() : "?");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs.Get();
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)p.pso->GetAddressOf()))) {
            Log("PSO %s 失败", p.entry);
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = SLOT_COUNT;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)g_heap.GetAddressOf()))) { Log("描述符堆失败"); return false; }
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    MkSrv(dev, color, color->GetDesc().Format, SLOT_PCOLOR);
    MkSrv(dev, g_netEdit.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, SLOT_EDIT);
    MkSrv(dev, g_net.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, SLOT_NET_SRV);
    MkUav(dev, g_net.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, SLOT_NET_UAV);
    MkUav(dev, g_mv1080.Get(), DXGI_FORMAT_R16G16B16A16_SNORM, SLOT_MV1080_UAV);
    MkUav(dev, g_dep1080.Get(), DXGI_FORMAT_R32_FLOAT, SLOT_DEP1080_UAV);
    MkSrv(dev, g_mv1080.Get(), DXGI_FORMAT_R16G16B16A16_SNORM, SLOT_MV1080_SRV);
    MkSrv(dev, g_dep1080.Get(), DXGI_FORMAT_R32_FLOAT, SLOT_DEP1080_SRV);

    // outp 的 UAV（这是「原地改画面」的唯一依赖）
    if (g_level == 10) {
        Log("等级10：跳过 outp UAV 创建（验证是不是它导致的崩溃）");
    } else {
        D3D12_RESOURCE_DESC d = od;
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = od.Format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)SLOT_OUTP_UAV * g_inc;
        dev->CreateUnorderedAccessView(outp, nullptr, &ud, h);
        g_outpUav = outp;
        g_outpUavOk = ((od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
        Log("outp %ux%u fmt=%d flags=0x%llx 可作 UAV=%d",
            (UINT)od.Width, od.Height, (int)od.Format, (unsigned long long)od.Flags, (int)g_outpUavOk);
    }
    Log("游戏侧资源就绪（网络输入 %ux%u，输出 %ux%u）", kNetW, kNetH, g_outW, g_outH);
    return true;
}

// ================= 单帧抓帧模式 =================
// 启用条件：游戏目录存在 xess_proxy_capture.txt
// 触发：游戏内按 F2，或让游戏目录出现 xess_proxy_capnow.txt（外部脚本触发）
// 产物（都在游戏目录）：
//   nr_cap_color.bin   pColor   原始像素（行距已压平）
//   nr_cap_depth.bin   pDepth   原始像素
//   nr_cap_mv.bin      pVelocity 原始像素
//   nr_cap_out.bin     pOutput  原始像素（就是「这一帧不经过 NR 的最终画面」）
//   nr_cap_color.bmp   上面 color 的可视化
//   nr_cap_out.bmp     上面 output 的可视化（对比用）
//   nr_cap_params.txt  几何/格式/全部 qword 参数/jitter/exposure
//
// 这条路径刻意不建 PSO / 描述符堆 / 纹理 / 不加载 lmxxf，只建 4 个回读缓冲 + 4 次拷贝。
static bool g_capMode = false;
static volatile bool g_capWant = false;
static int g_capCountdown = -1;
static int g_capFrames = 0;
static bool g_capWritten = false;
static unsigned long long g_capQ[40] = { 0 };

struct CapBuf {
    ComPtr<ID3D12Resource> rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    UINT rowPitch = 0;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT bpp = 4;
    const char* name = "";
    bool ok = false;
};
static CapBuf g_cap[4];
static bool g_capReady = false;

static bool CapInitBuf(CapBuf& b, ID3D12Resource* r, const char* name, UINT bpp) {
    if (!r) { Log("抓帧: %s 资源为空", name); return false; }
    D3D12_RESOURCE_DESC d = r->GetDesc();
    b.w = (UINT)d.Width; b.h = d.Height; b.fmt = d.Format; b.bpp = bpp; b.name = name;
    D3D12_RESOURCE_DESC plain = d;
    plain.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    g_dev->GetCopyableFootprints(&plain, 0, 1, 0, &b.fp, &rows, &rowSize, &total);
    b.bytes = total;
    b.rowPitch = b.fp.Footprint.RowPitch;
    D3D12_HEAP_PROPERTIES rh{};
    rh.Type = D3D12_HEAP_TYPE_READBACK;
    rh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    rh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    rh.CreationNodeMask = 1; rh.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = g_dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)b.rb.GetAddressOf());
    b.ok = SUCCEEDED(hr);
    Log("抓帧缓冲 %s: %ux%u fmt=%d rowPitch=%u total=%llu %s", name, b.w, b.h, (int)b.fmt,
        b.rowPitch, (unsigned long long)b.bytes, b.ok ? "ok" : "失败");
    return b.ok;
}

static void CapOneCopy(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, CapBuf& b,
                       D3D12_RESOURCE_STATES assumed) {
    if (!b.ok || !src) return;
    if (assumed != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = src;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = assumed;
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &t);
    }
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = b.rb.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = b.fp;
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;
    cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    if (assumed != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = src;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        t.Transition.StateAfter = assumed;
        cl->ResourceBarrier(1, &t);
    }
}

static void CapWriteBmp(const char* path, const unsigned char* px, UINT w, UINT h, bool rgba) {
    DWORD rowBytes = w * 4;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + rowBytes * h;
    ih.biSize = sizeof(ih);
    ih.biWidth = (LONG)w;
    ih.biHeight = -(LONG)h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = rowBytes * h;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<unsigned char> row(rowBytes);
    for (UINT y = 0; y < h; ++y) {
        const unsigned char* s = px + (size_t)y * rowBytes;
        for (UINT x = 0; x < w; ++x) {
            unsigned char r = s[x * 4 + (rgba ? 0 : 2)];
            unsigned char g = s[x * 4 + 1];
            unsigned char b = s[x * 4 + (rgba ? 2 : 0)];
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
        fwrite(row.data(), rowBytes, 1, f);
    }
    fclose(f);
}

static void CapWriteFiles() {
    std::string dir = ModuleDir();
    UINT tight[4];
    std::vector<std::vector<unsigned char>> data(4);
    for (int i = 0; i < 4; ++i) {
        if (!g_cap[i].ok) continue;
        D3D12_RANGE rr{ 0, (SIZE_T)g_cap[i].bytes };
        void* m = nullptr;
        if (FAILED(g_cap[i].rb->Map(0, &rr, &m)) || !m) { Log("抓帧: %s Map 失败", g_cap[i].name); continue; }
        tight[i] = g_cap[i].w * g_cap[i].bpp;
        data[i].resize((size_t)tight[i] * g_cap[i].h);
        for (UINT y = 0; y < g_cap[i].h; ++y)
            memcpy(&data[i][(size_t)y * tight[i]],
                   (unsigned char*)m + (size_t)y * g_cap[i].rowPitch, tight[i]);
        D3D12_RANGE nw{ 0, 0 };
        g_cap[i].rb->Unmap(0, &nw);
    }
    auto saveBin = [&](int i, const char* name) {
        if (data[i].empty()) return;
        FILE* f = nullptr;
        std::string p = dir + name;
        if (fopen_s(&f, p.c_str(), "wb") == 0 && f) {
            fwrite(data[i].data(), 1, data[i].size(), f);
            fclose(f);
            Log("抓帧写出 %s (%zu 字节)", name, data[i].size());
        }
    };
    saveBin(0, "nr_cap_color.bin");
    saveBin(1, "nr_cap_depth.bin");
    saveBin(2, "nr_cap_mv.bin");
    saveBin(3, "nr_cap_out.bin");
    if (!data[0].empty())
        CapWriteBmp((dir + "nr_cap_color.bmp").c_str(), data[0].data(), g_cap[0].w, g_cap[0].h, false);
    if (!data[3].empty()) {
        // R10G10B10A2 → RGBA8
        std::vector<unsigned char> rgba((size_t)g_cap[3].w * g_cap[3].h * 4);
        for (size_t i = 0, n = (size_t)g_cap[3].w * g_cap[3].h; i < n; ++i) {
            unsigned int v = ((const unsigned int*)data[3].data())[i];
            unsigned int r = v & 0x3FF, g = (v >> 10) & 0x3FF, b = (v >> 20) & 0x3FF;
            rgba[i * 4 + 0] = (unsigned char)((r * 255u + 511u) / 1023u);
            rgba[i * 4 + 1] = (unsigned char)((g * 255u + 511u) / 1023u);
            rgba[i * 4 + 2] = (unsigned char)((b * 255u + 511u) / 1023u);
            rgba[i * 4 + 3] = 255;
        }
        CapWriteBmp((dir + "nr_cap_out.bmp").c_str(), rgba.data(), g_cap[3].w, g_cap[3].h, true);
    }
    {
        FILE* f = nullptr;
        std::string p = dir + "nr_cap_params.txt";
        if (fopen_s(&f, p.c_str(), "wb") == 0 && f) {
            fprintf(f, "proxy=v7-capture\n");
            fprintf(f, "color %ux%u fmt=%d tightsem=%u\n", g_cap[0].w, g_cap[0].h, (int)g_cap[0].fmt, tight[0]);
            fprintf(f, "depth %ux%u fmt=%d tightsem=%u\n", g_cap[1].w, g_cap[1].h, (int)g_cap[1].fmt, tight[1]);
            fprintf(f, "mv    %ux%u fmt=%d tightsem=%u\n", g_cap[2].w, g_cap[2].h, (int)g_cap[2].fmt, tight[2]);
            fprintf(f, "out   %ux%u fmt=%d tightsem=%u\n", g_cap[3].w, g_cap[3].h, (int)g_cap[3].fmt, tight[3]);
            for (int i = 0; i < 40; ++i) fprintf(f, "q[%d]=0x%llx\n", i, g_capQ[i]);
            float jx = 0, jy = 0, ex = 0;
            memcpy(&jx, (const char*)g_capQ + 48, 4);
            memcpy(&jy, (const char*)g_capQ + 52, 4);
            memcpy(&ex, (const char*)g_capQ + 56, 4);
            fprintf(f, "jitter = %.6f %.6f\nexposureScale = %.6f\n", jx, jy, ex);
            fclose(f);
            Log("抓帧写出 nr_cap_params.txt");
        }
    }
    g_capWritten = true;
    Log("抓帧完成（数据已在游戏目录）");
}

static void CapTriggerCheck() {
    if (g_capWritten) return;
    std::string f = ModuleDir() + "xess_proxy_capnow.txt";
    if (GetFileAttributesA(f.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(f.c_str());
        g_capWant = true;
        Log("抓帧：检测到触发文件");
    }
}

static int CapExecute(void* ctx, void* cmdList, const void* params, void* realFn, void* color,
                      void* depth, void* mv, void* outp) {
    using Fn = int(*)(void*, void*, const void*);
    Fn real = (Fn)realFn;
    int rc = real ? real(ctx, cmdList, params) : -1;
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmdList;

    if (!g_capReady) {
        if (!g_dev && color) ((ID3D12Resource*)color)->GetDevice(__uuidof(ID3D12Device), (void**)g_dev.GetAddressOf());
        if (g_dev) {
            CapInitBuf(g_cap[0], (ID3D12Resource*)color, "color", 4);
            CapInitBuf(g_cap[1], (ID3D12Resource*)depth, "depth", 4);
            CapInitBuf(g_cap[2], (ID3D12Resource*)mv, "mv", 8);
            CapInitBuf(g_cap[3], (ID3D12Resource*)outp, "out", 4);
            g_capReady = true;
        }
    }
    if (++g_capFrames % 30 == 0) CapTriggerCheck();

    if (g_capWant && !g_capWritten && g_capReady) {
        g_capWant = false;
        if (params) {
            const unsigned long long* q = (const unsigned long long*)params;
            for (int i = 0; i < 40; ++i) g_capQ[i] = q[i];
        }
        CapOneCopy(cl, (ID3D12Resource*)color, g_cap[0], D3D12_RESOURCE_STATE_COMMON);
        CapOneCopy(cl, (ID3D12Resource*)depth, g_cap[1], D3D12_RESOURCE_STATE_DEPTH_READ);
        CapOneCopy(cl, (ID3D12Resource*)mv, g_cap[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        CapOneCopy(cl, (ID3D12Resource*)outp, g_cap[3], D3D12_RESOURCE_STATE_COMMON);
        g_capCountdown = 90;    // 90 帧后再读（此时 GPU 肯定跑完了，不阻塞）
        Log("抓帧：已在本帧命令列表上记录 4 次拷贝，等 %d 帧后回读", g_capCountdown);
        g_capWritten = false;
    } else if (g_capCountdown > 0) {
        if (--g_capCountdown == 0) {
            g_capWritten = true;    // 先标记，避免重复
            std::thread([&]() { CapWriteFiles(); }).detach();
        }
    }
    return rc;
}


static DWORD WINAPI HotkeyThread(LPVOID) {
    auto edge = [](int vk) { static bool prev[256] = {}; bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
                             bool e = d && !prev[vk & 0xFF]; prev[vk & 0xFF] = d; return e; };
    while (true) {
        if (edge(VK_F10)) { g_nrOn = !g_nrOn; Log("*** F10: NR %s ***", g_nrOn ? "开" : "关（游戏原生 4K）"); }
        if (edge(VK_F8)) { g_strength = (g_strength < 4.0f) ? g_strength + 0.25f : 4.0f; Log("*** F8: 强度 %.2f ***", g_strength); }
        if (edge(VK_F7)) { g_strength = (g_strength > 0.25f) ? g_strength - 0.25f : 0.0f; Log("*** F7: 强度 %.2f ***", g_strength); }
        if (edge(VK_F6)) { g_boost = (g_boost > 1.5f) ? 1.0f : 8.0f; Log("*** F6: 增益 %.0fx（可见性自证）***", g_boost); }
        if (edge(VK_F5)) { g_feedMv = !g_feedMv; Log("*** F5: motion 喂入 %s ***", g_feedMv ? "开" : "关"); }
        if (edge(VK_F4)) { g_feedDep = !g_feedDep; Log("*** F4: depth 喂入 %s ***", g_feedDep ? "开" : "关"); }
        if (edge(VK_F2)) { g_capWant = true; Log("*** F2: 抓取一帧原始数据 ***"); }
        if (edge(VK_F3)) { g_markFrames = 240; Log("*** F3: 往 outp 左上角画绿块（通路自证）***"); }
        if (edge(VK_F9)) {
            static int look = 0;
            look = (look + 1) % 3;
            if (look == 0) { g_lookSat = 1.0f; g_lookGamma = 1.0f; g_lookHiCut = 0.0f; Log("*** F9: Look 关 ***"); }
            else if (look == 1) { g_lookSat = 1.10f; g_lookGamma = 0.97f; g_lookHiCut = 0.3f; Log("*** F9: Look 轻 ***"); }
            else { g_lookSat = 1.22f; g_lookGamma = 0.93f; g_lookHiCut = 0.8f; Log("*** F9: Look 强 ***"); }
        }
        Sleep(40);
    }
    return 0;
}

extern "C" int ProxyExecute(void* ctx, void* cmdList, const void* params) {
    using Fn = int(*)(void*, void*, const void*);
    Fn real = (Fn)g_xessTab[30];
    if (!params || !cmdList) return real ? real(ctx, cmdList, params) : -1;
    if (g_disabled) return real ? real(ctx, cmdList, params) : -1;
    // 抓帧模式：完全不碰 NR 链路，只调真 XeSS + 记录 4 次拷贝
    if (g_capMode) {
        const unsigned __int64* qq = (const unsigned __int64*)params;
        return CapExecute(ctx, cmdList, params, (void*)real,
                          (void*)qq[0], (void*)qq[2], (void*)qq[1], (void*)qq[5]);
    }
    // 分级开关（xess_proxy_level.txt）：0/1=完全不录命令也不初始化（只调真 XeSS）
    if (g_level <= 1) return real ? real(ctx, cmdList, params) : -1;
    const bool wantCapture = (g_level == 3 || (g_level >= 5 && g_level <= 7));
    const bool wantMv = (g_level == 4 || (g_level >= 5 && g_level <= 7));

    const unsigned __int64* q = (const unsigned __int64*)params;
    ID3D12Resource* color = (ID3D12Resource*)q[0];
    ID3D12Resource* outp = (ID3D12Resource*)q[5];
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmdList;

    static bool inited = false;
    static bool gaveUp = false;
    if (gaveUp) return real ? real(ctx, cmdList, params) : -1;
    if (!inited) {
        if (!color || !outp) return real ? real(ctx, cmdList, params) : -1;
        if (FAILED(color->GetDevice(__uuidof(ID3D12Device), (void**)g_dev.GetAddressOf())) || !g_dev) {
            gaveUp = true; Log("!! 取设备失败，永久放弃集成");
            return real ? real(ctx, cmdList, params) : -1;
        }
        // 关键：渲染线程只取设备指针，所有重活（编译 shader、建资源、建 9070 XT 设备、
        // 加载 lmxxf、PrepareSession）都交给工作线程，绝不在 xessD3D12Execute 里阻塞。
        g_color0 = color;
        g_outp0 = outp;
        inited = true;
        g_worker.th = std::thread(WorkerMain);
        Log("初始化移交给工作线程（渲染线程不再阻塞）");
        return real ? real(ctx, cmdList, params) : -1;
    }
    if (!g_resReady) return real ? real(ctx, cmdList, params) : -1;   // 资源还没就绪 → 完全等同「只调真 XeSS」

    // outp 被重建（resize / 上采样档位变化）时重建 UAV 描述符，否则会一直写旧纹理
    if (g_heap && outp && outp != g_outpUav.Get()) {
        D3D12_RESOURCE_DESC od = outp->GetDesc();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = od.Format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)SLOT_OUTP_UAV * g_inc;
        g_dev->CreateUnorderedAccessView(outp, nullptr, &ud, h);
        g_outpUav = outp;
        g_outW = (UINT)od.Width;
        g_outH = od.Height;
        g_outpUavOk = ((od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
        Log("outp 变化: %ux%u fmt=%d flags=0x%llx 可作 UAV=%d",
            g_outW, g_outH, (int)od.Format, (unsigned long long)od.Flags, (int)g_outpUavOk);
    }

    // motion / depth 的 SRV（惰性建立，槽 3 = motion；depth 需要时另建）
    {
        static bool mvSrv = false;
        ID3D12Resource* mv = (ID3D12Resource*)q[1];
        if (!mvSrv && mv && g_heap) {
            MkSrv(g_dev.Get(), mv, mv->GetDesc().Format, SLOT_MV);
            mvSrv = true;
            Log("motion SRV 已建立 (%ux%u fmt=%d)", (UINT)mv->GetDesc().Width, mv->GetDesc().Height, (int)mv->GetDesc().Format);
        }
    }
    {
        static bool depSrv = false;
        ID3D12Resource* dp = (ID3D12Resource*)q[2];
        if (!depSrv && dp && g_heap) {
            MkSrv(g_dev.Get(), dp, DXGI_FORMAT_R32_FLOAT, 10);   // 槽 10 复用
            depSrv = true;
            Log("depth SRV 已建立 (%ux%u fmt=%d)", (UINT)dp->GetDesc().Width, dp->GetDesc().Height, (int)dp->GetDesc().Format);
        }
    }

    auto tA = Clock::now();

    // ① 收上一帧结果
    for (int i = 0; i < 2; ++i) {
        if (!g_pending[i]) continue;
        g_pending[i] = false;
        D3D12_RANGE rr{ 0, (SIZE_T)g_bytesNet };
        void* m = nullptr;
        if (SUCCEEDED(g_rbNet[i]->Map(0, &rr, &m)) && m) {
            std::lock_guard<std::mutex> lk(g_worker.m);
            if (g_worker.hasOut) {
                g_edit = g_worker.outEdit;
                g_worker.hasOut = false;
                g_haveEdit = true;
            }
            if (!g_worker.hasIn && g_worker.ok && (g_level == 6 || g_level == 7)) {
                UnpackTight(m, g_fpNet.Footprint.RowPitch, kNetW * 4, kNetH, g_worker.inBuf);
                g_worker.useMv = g_feedMv;
                g_worker.useDep = g_feedDep;
                g_worker.hasIn = true;
                g_worker.cv.notify_one();
            }
            D3D12_RANGE nw{ 0, 0 };
            g_rbNet[i]->Unmap(0, &nw);
        }
    }
    if (g_pendMv) {
        g_pendMv = false;
        D3D12_RANGE rr{ 0, (SIZE_T)g_bytesMv };
        void* m = nullptr;
        if (SUCCEEDED(g_rbMv->Map(0, &rr, &m)) && m) {
            std::lock_guard<std::mutex> lk(g_worker.m);
            UnpackTight(m, g_fpMv.Footprint.RowPitch, kNetW * 8, kNetH, g_worker.inMv);
            D3D12_RANGE nw{ 0, 0 };
            g_rbMv->Unmap(0, &nw);
        }
    }
    if (g_pendDep) {
        g_pendDep = false;
        D3D12_RANGE rr{ 0, (SIZE_T)g_bytesDep };
        void* m = nullptr;
        if (SUCCEEDED(g_rbDep->Map(0, &rr, &m)) && m) {
            std::lock_guard<std::mutex> lk(g_worker.m);
            UnpackTight(m, g_fpDep.Footprint.RowPitch, kNetW * 4, kNetH, g_worker.inDep);
            D3D12_RANGE nw{ 0, 0 };
            g_rbDep->Unmap(0, &nw);
        }
    }

    // ② 照常调用真实 XeSS（兜底）
    int rc = real ? real(ctx, cmdList, params) : -1;

    static ID3D12DescriptorHeap* heaps[1] = { nullptr };
    heaps[0] = g_heap.Get();

    // ③ 抓帧：outp(4K) → 1080p 盒式下采样（UAV 读，不需要知道 outp 状态）
    if (g_outpUavOk && wantCapture) {
        cl->SetDescriptorHeaps(1, heaps);
        float st = 1.0f;
        SetPass(cl, g_psoCapture.Get(), SLOT_PCOLOR, SLOT_EDIT, SLOT_NET_UAV, SLOT_OUTP_UAV, st);
        UavBarrier(cl, outp);
        Transition(cl, g_net.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->Dispatch((kNetW + 7) / 8, (kNetH + 7) / 8, 1);
        UavBarrier(cl, outp);
        UavBarrier(cl, g_net.Get());

        Transition(cl, g_net.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rd{};
        rd.pResource = g_rbNet[g_rbIdx].Get();
        rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rd.PlacedFootprint = g_fpNet;
        D3D12_TEXTURE_COPY_LOCATION rs{};
        rs.pResource = g_net.Get();
        rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rs.SubresourceIndex = 0;
        cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
        Transition(cl, g_net.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_pending[g_rbIdx] = true;
        g_rbIdx ^= 1;
    }

    // ④ motion / depth → 1080p（真实数据，喂给运行时的时序通路）
    {
        ID3D12Resource* mv = (ID3D12Resource*)q[1];
        ID3D12Resource* dp = (ID3D12Resource*)q[2];
        if (g_feedMv && mv && wantMv) {
            cl->SetDescriptorHeaps(1, heaps);
            SetPass(cl, g_psoResample.Get(), SLOT_MV, SLOT_EDIT, SLOT_MV1080_UAV, SLOT_OUTP_UAV, 1.0f);
            Transition(cl, mv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl->Dispatch((kNetW + 7) / 8, (kNetH + 7) / 8, 1);
            Transition(cl, mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            UavBarrier(cl, g_mv1080.Get());
            Transition(cl, g_mv1080.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION rd{};
            rd.pResource = g_rbMv.Get();
            rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rd.PlacedFootprint = g_fpMv;
            D3D12_TEXTURE_COPY_LOCATION rs{};
            rs.pResource = g_mv1080.Get();
            rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            rs.SubresourceIndex = 0;
            cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
            Transition(cl, g_mv1080.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_pendMv = true;
        }
        if (g_feedDep && dp && wantMv) {
            cl->SetDescriptorHeaps(1, heaps);
            SetPass(cl, g_psoResample.Get(), 10, SLOT_EDIT, SLOT_DEP1080_UAV, SLOT_OUTP_UAV, 1.0f);
            Transition(cl, dp, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cl->Dispatch((kNetW + 7) / 8, (kNetH + 7) / 8, 1);
            Transition(cl, dp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_READ);
            UavBarrier(cl, g_dep1080.Get());
            Transition(cl, g_dep1080.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION rd{};
            rd.pResource = g_rbDep.Get();
            rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rd.PlacedFootprint = g_fpDep;
            D3D12_TEXTURE_COPY_LOCATION rs{};
            rs.pResource = g_dep1080.Get();
            rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            rs.SubresourceIndex = 0;
            cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
            Transition(cl, g_dep1080.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_pendDep = true;
        }
    }

    // ⑤ 有结果就把差分原地叠到 outp（UAV 读改写，零状态假设）
    if (g_haveEdit && g_nrOn && g_outpUavOk && g_level == 7) {
        {
            D3D12_RANGE nr{ 0, 0 };
            void* m = nullptr;
            if (SUCCEEDED(g_upNet->Map(0, &nr, &m)) && m) {
                UINT pitch = g_fpNet.Footprint.RowPitch;
                size_t rowBytes = (size_t)kNetW * 4;
                if (pitch == rowBytes) memcpy(m, g_edit.data(), (size_t)rowBytes * kNetH);
                else for (UINT y = 0; y < kNetH; ++y) memcpy((uint8_t*)m + (size_t)y * pitch, g_edit.data() + (size_t)y * rowBytes, rowBytes);
                D3D12_RANGE wr{ 0, (SIZE_T)pitch * kNetH };
                g_upNet->Unmap(0, &wr);
            }
        }
        Transition(cl, g_netEdit.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dd{};
        dd.pResource = g_netEdit.Get();
        dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dd.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION ss{};
        ss.pResource = g_upNet.Get();
        ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        ss.PlacedFootprint = g_fpNet;
        cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
        Transition(cl, g_netEdit.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        cl->SetDescriptorHeaps(1, heaps);
        float st = g_strength * g_boost;
        SetPass(cl, g_psoEdit.Get(), SLOT_PCOLOR, SLOT_EDIT, SLOT_NET_UAV, SLOT_OUTP_UAV, st);
        UavBarrier(cl, outp);
        cl->Dispatch((g_outW + 7) / 8, (g_outH + 7) / 8, 1);
        UavBarrier(cl, outp);
        g_applied++;
    }

    // ⑥ F3 通路自证：绿块
    if (g_markFrames > 0) {
        g_markFrames--;
        cl->SetDescriptorHeaps(1, heaps);
        SetPass(cl, g_psoMark.Get(), SLOT_PCOLOR, SLOT_EDIT, SLOT_NET_UAV, SLOT_OUTP_UAV, 1.0f);
        UavBarrier(cl, outp);
        cl->Dispatch(32, 32, 1);
        UavBarrier(cl, outp);
    }

    g_frames++;
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - tA).count();
    g_sumSubmit += ms; g_nSample++;
    {
        std::lock_guard<std::mutex> lk(g_worker.m);
        g_sumWorker += g_worker.lastMs; g_nWorker++;
    }
    if (g_frames % 120 == 0) {
        std::lock_guard<std::mutex> lk(g_worker.m);
        Log("stats: frames=%d 叠加=%d 提交=%.2fms worker=%.2fms processed=%d | 网络edit 平均%.4f 最大%.4f | 增益%.0fx mv=%d dep=%d",
            g_frames, g_applied, g_sumSubmit / g_nSample,
            g_nWorker ? g_sumWorker / g_nWorker : 0.0, g_worker.processed,
            g_worker.lastEditMean, g_worker.lastEditMax,
            g_boost, (int)g_feedMv, (int)g_feedDep);
    }
    // 保险：跑够 300 帧后若平均提交开销过高，永久关闭（游戏回到原生 4K，不会卡死）
    if (!g_disabled && g_frames > 300 && (g_sumSubmit / g_nSample) > 8.0) {
        g_disabled = true;
        Log("!! 保险：平均提交 %.2fms 过高，永久关闭集成（画面回到游戏原生）", g_sumSubmit / g_nSample);
    }
    return rc;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        OpenLog();
        Log("libxess proxy v7 (跨卡 NR, private_output) attached");
        {
            std::string off = ModuleDir() + "xess_proxy_off.txt";
            if (GetFileAttributesA(off.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_disabled = true;
                Log("检测到 xess_proxy_off.txt：完全不接管（保险生效）");
            }
        }
        {
            std::string cap = ModuleDir() + "xess_proxy_capture.txt";
            if (GetFileAttributesA(cap.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_capMode = true;
                Log("抓帧模式开启（xess_proxy_capture.txt）：只抓帧，不跑 NR");
            }
        }
        {
            std::string lv = ModuleDir() + "xess_proxy_level.txt";
            FILE* f = nullptr;
            if (fopen_s(&f, lv.c_str(), "r") == 0 && f) {
                int v = 4;
                if (fscanf_s(f, "%d", &v) == 1 && v >= 0 && v <= 9) g_level = v;
                fclose(f);
                Log("分级开关 xess_proxy_level.txt = %d", (int)g_level);
            }
        }
        InitForwarders();
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
        Log("转发就绪 (%s)", g_real ? "libxess_orig.dll 已加载" : "真实 DLL 加载失败");
    }
    return TRUE;
}
