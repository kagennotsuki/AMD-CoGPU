// libxess.dll 代理 v3 —— 跨卡神经渲染（正确集成点）
//
// 链路（全部录在游戏自己的命令列表上）：
//   pd-upscaler → xessD3D12Execute(color 2227x1253, depth, MV, jitter, output 3840x2160)
//     ① pColor 重采样到 1920x1080 RGBA → 拷进回读缓冲
//     ② 回读 8.3MB → CPU → 工作线程（9070 XT 上跑 lmxxf）→ 结果回 CPU
//     ③ 下一帧：结果上传 → 重采样到 3840x2160 → 写进 pOutput
//     ④ 每次都照常调用真实 XeSS（兜底：链路没就绪时画面依然正确）
//
// 已验证：拦截 Execute ✓ / 读取真实 color ✓ / 写入 pOutput 并显示在屏幕 ✓

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
    void* extra0;
    void* extra1;
};

struct FrameInfo {              // 实测需要 144 字节才能带 look 控制
    uint32_t struct_size;
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
};

struct Job {
    uint32_t struct_size;
    void* handle;
    void* private_output;
};

struct CreateInfo {          // 实测 40 字节：struct_size + device + queue + assets + reserved
    uint32_t struct_size;
    uint32_t pad0;
    void* device;
    void* queue;
    const wchar_t* assets_directory;
    void* reserved0;
};

// ---------------- 常量 ----------------
static UINT kNetW = 1920;    // 运行时初始化时改为 pColor 的真实尺寸
static UINT kNetH = 1080;
static UINT g_outW = 0, g_outH = 0;

// ---------------- 重采样 shader ----------------
static const char* kHLSL = R"(
cbuffer Params : register(b0) { float gStrength; float gSat; float gGamma; float gHiCut; float4 gPad; };
Texture2D<float4>   gSrc    : register(t0);
Texture2D<float4>   gNrOut  : register(t1);
Texture2D<float4>   gNrIn   : register(t2);
Texture2D<float4>   gMv     : register(t3);
SamplerState        gSmp    : register(s0);
RWTexture2D<float4> gDst    : register(u0);

[numthreads(8, 8, 1)]
void ResampleCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    gDst[id.xy] = gSrc.SampleLevel(gSmp, uv, 0);
}

// 只把网络对画面的"修改量"叠加到 4K 底图上：保住原生细节
[numthreads(8, 8, 1)]
void EditCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float4 base = gSrc.Load(int3(id.xy, 0));
    float3 edit = (gNrOut.SampleLevel(gSmp, uv, 0).rgb - gNrIn.SampleLevel(gSmp, uv, 0).rgb) * gStrength;
    float2 mv = gMv.SampleLevel(gSmp, uv, 0).xy;
    float motion = length(mv);                       // 像素单位的运动量
    float mask = 1.0;   // 暂时关闭掩码（MV 是像素单位，此前的 1-|mv|/2 会把 edit 全抹掉）
    float3 c = saturate(base.rgb + edit * mask);
    // Look 层：饱和度 / gamma(对比) / 高光压缩（对应 AMDNR 的 [AmdLook]）
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(lum, lum, lum), c, gSat);
    c = pow(saturate(c), float3(gGamma, gGamma, gGamma));
    c = c / (1.0 + max(0.0, c - 0.75) * gHiCut);
    gDst[id.xy] = float4(c, base.a);
}
)";

// ---------------- 工作线程（9070 XT） ----------------
struct Worker {
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> inBuf, outBuf;
    std::vector<uint8_t> inDepth, inVel;
    bool hasIn = false, hasOut = false, quit = false;
    int processed = 0;
    double lastMs = 0;

    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fe;
    UINT64 fv = 0;
    ComPtr<ID3D12Resource> color, up, rb;
    ComPtr<ID3D12Resource> depth, vel, upD, upV;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpD{}, fpV{};
    UINT64 bytesD = 0, bytesV = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
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

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kNetW; td.Height = kNetH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(W.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)W.color.GetAddressOf()))) { Log("worker: color 失败"); return false; }

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
    W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)W.up.GetAddressOf());
    W.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)W.rb.GetAddressOf());

    W.inBuf.assign((size_t)total, 0);
    W.outBuf.assign((size_t)total, 0);

    // depth / MV 纹理（同几何）
    {
        D3D12_RESOURCE_DESC dd = td;
        dd.Format = DXGI_FORMAT_R32_FLOAT;
        W.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)W.depth.GetAddressOf());
        D3D12_RESOURCE_DESC dv = td;
        dv.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
        W.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &dv, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), (void**)W.vel.GetAddressOf());
        UINT rr2 = 0; UINT64 rs2 = 0, t2 = 0;
        W.dev->GetCopyableFootprints(&dd, 0, 1, 0, &W.fpD, &rr2, &rs2, &t2);
        W.bytesD = t2;
        W.dev->GetCopyableFootprints(&dv, 0, 1, 0, &W.fpV, &rr2, &rs2, &t2);
        W.bytesV = t2;
        D3D12_RESOURCE_DESC bdD = bd; bdD.Width = W.bytesD;
        D3D12_RESOURCE_DESC bdV = bd; bdV.Width = W.bytesV;
        W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bdD, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)W.upD.GetAddressOf());
        W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bdV, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)W.upV.GetAddressOf());
        W.inDepth.assign((size_t)W.bytesD, 0);
        W.inVel.assign((size_t)W.bytesV, 0);
        Log("worker: depth %llu, vel %llu 字节", (unsigned long long)W.bytesD, (unsigned long long)W.bytesV);
    }

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

static void WorkerMain() {
    Worker& W = g_worker;
    std::vector<uint8_t> local;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(W.m);
            W.cv.wait(lk, [&] { return W.quit || W.hasIn; });
            if (W.quit) break;
            local = W.inBuf;
            W.hasIn = false;
        }
        auto t0 = Clock::now();

        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (SUCCEEDED(W.up->Map(0, &nr, &m)) && m) {
            memcpy(m, local.data(), local.size());
            D3D12_RANGE wr{ 0, (SIZE_T)local.size() };
            W.up->Unmap(0, &wr);

            W.al->Reset();
            W.cl->Reset(W.al.Get(), nullptr);
            D3D12_TEXTURE_COPY_LOCATION dd{};
            dd.pResource = W.color.Get();
            dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ss{};
            ss.pResource = W.up.Get();
            ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            ss.PlacedFootprint = W.fp;
            W.cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
            W.cl->Close();
            { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
            WaitWorker();

            // 上传 depth / MV（若代理已喂入）
            {
                void* md = nullptr; void* mv = nullptr;
                D3D12_RANGE zr{ 0, 0 };
                if (!W.inDepth.empty() && SUCCEEDED(W.upD->Map(0, &zr, &md)) && md) {
                    memcpy(md, W.inDepth.data(), W.inDepth.size());
                    D3D12_RANGE w2{ 0, (SIZE_T)W.inDepth.size() }; W.upD->Unmap(0, &w2);
                }
                if (!W.inVel.empty() && SUCCEEDED(W.upV->Map(0, &zr, &mv)) && mv) {
                    memcpy(mv, W.inVel.data(), W.inVel.size());
                    D3D12_RANGE w2{ 0, (SIZE_T)W.inVel.size() }; W.upV->Unmap(0, &w2);
                }
                if (md || mv) {
                    W.al->Reset(); W.cl->Reset(W.al.Get(), nullptr);
                    if (md) {
                        D3D12_TEXTURE_COPY_LOCATION a{}; a.pResource = W.depth.Get();
                        a.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; a.SubresourceIndex = 0;
                        D3D12_TEXTURE_COPY_LOCATION b{}; b.pResource = W.upD.Get();
                        b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; b.PlacedFootprint = W.fpD;
                        W.cl->CopyTextureRegion(&a, 0, 0, 0, &b, nullptr);
                    }
                    if (mv) {
                        D3D12_TEXTURE_COPY_LOCATION a{}; a.pResource = W.vel.Get();
                        a.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; a.SubresourceIndex = 0;
                        D3D12_TEXTURE_COPY_LOCATION b{}; b.pResource = W.upV.Get();
                        b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; b.PlacedFootprint = W.fpV;
                        W.cl->CopyTextureRegion(&a, 0, 0, 0, &b, nullptr);
                    }
                    W.cl->Close();
                    { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                    WaitWorker();
                }
            }

            // 测试 A：每帧清历史（去掉时序记忆 → 应该消除抖动/重影）
            if (W.api.ResetHistory) W.api.ResetHistory(W.ctx);

            if (W.api.PrepareFrame) {
                Job job{};
                job.struct_size = (uint32_t)sizeof(Job);
                FrameInfo fi{};
                fi.struct_size = 144;                  // 必须 144 才能带 look 控制
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
                fi.depth = W.depth.Get();
                fi.velocity = W.vel.Get();
                fi.ctlTone = 1.0f;        // look 控制：取自运行时日志的默认组合
                fi.ctlStructure = 1.0f;
                fi.ctlSkin = 3.0f;
                fi.ctlOther = 2.0f;
                fi.ctlStyle = 0.0f;
                fi.ctlPad = 0;

                int32_t rcPF = W.api.PrepareFrame(W.ctx, &fi, &job);
                static int loggedRc = 0;
                if (rcPF != 0) {
                    if (loggedRc < 5) {
                        char eb[256] = { 0 };
                        if (W.api.GetLastError) W.api.GetLastError(eb, 256);
                        Log("worker: PrepareFrame 失败 rc=%d err=%s (%ux%u)", (int)rcPF, eb, kNetW, kNetH);
                        loggedRc++;
                    }
                }
                if (rcPF == 0) {
                    W.al->Reset();
                    W.cl->Reset(W.al.Get(), nullptr);
                    if (W.api.RecordInputs(W.ctx, job.handle, W.cl.Get()) == 0) {
                        W.cl->Close();
                        { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                        WaitWorker();
                        if (W.api.ExecuteAfterProducer(W.ctx, job.handle, W.q.Get()) == 0) {
                            W.al->Reset();
                            W.cl->Reset(W.al.Get(), nullptr);
                            if (W.api.RecordOutputs(W.ctx, job.handle, W.cl.Get()) == 0) {
                                W.cl->Close();
                                { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                                WaitWorker();
                                W.api.Retire(W.ctx, job.handle);
                            }
                        }
                    }
                }
            }

            W.al->Reset();
            W.cl->Reset(W.al.Get(), nullptr);
            D3D12_TEXTURE_COPY_LOCATION rd{};
            rd.pResource = W.rb.Get();
            rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rd.PlacedFootprint = W.fp;
            D3D12_TEXTURE_COPY_LOCATION rs{};
            rs.pResource = W.color.Get();
            rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            rs.SubresourceIndex = 0;
            W.cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
            W.cl->Close();
            { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
            WaitWorker();

            D3D12_RANGE rr{ 0, (SIZE_T)W.bytes };
            void* rm = nullptr;
            if (SUCCEEDED(W.rb->Map(0, &rr, &rm)) && rm) {
                memcpy(local.data(), rm, local.size());
                D3D12_RANGE nw{ 0, 0 };
                W.rb->Unmap(0, &nw);
            }
        }

        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(W.m);
            W.outBuf = local;
            W.hasOut = true;
            W.processed++;
            W.lastMs = ms;
            if (W.processed <= 3 || W.processed % 120 == 0)
                Log("worker: frame %d 完成 %.2f ms", W.processed, ms);
        }
    }
}

// ---------------- 游戏侧资源 ----------------
static ComPtr<ID3D12Device> g_dev;
static ComPtr<ID3D12RootSignature> g_rs;
static ComPtr<ID3D12PipelineState> g_pso;
static ComPtr<ID3D12PipelineState> g_psoEdit;
static ComPtr<ID3D12DescriptorHeap> g_heap;
static UINT g_inc = 0;
static ComPtr<ID3D12Resource> g_net;
static ComPtr<ID3D12Resource> g_netIn;
static ComPtr<ID3D12Resource> g_big;
static ComPtr<ID3D12Resource> g_dep1080, g_mv1080;   // depth/MV 下采样目标
static ComPtr<ID3D12Resource> g_rbDep, g_upDep, g_upMv;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fpDep{};
static UINT64 g_bytesDep = 0;
static bool g_pendDep = false;
static ComPtr<ID3D12Resource> g_base;      // pOutput 的拷贝（避免直接当 SRV 读）
static ComPtr<ID3D12Resource> g_upNet;
static ComPtr<ID3D12Resource> g_rbNet[2];
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fpNet{};
static UINT64 g_bytesNet = 0;
static int g_rbIdx = 0;
static bool g_pending[2] = { false, false };

// depth / MV 回读（数值校验用，先不接 ABI）
static ComPtr<ID3D12Resource> g_rbDepth, g_rbMv;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fpDepth{}, g_fpMv{};
static UINT64 g_bytesDepth = 0, g_bytesMv = 0;
static bool g_pendDepth = false, g_pendMv = false;
static const void* g_firstParams = nullptr;   // 首次 Execute 的参数（取 depth/MV 资源）
static int g_statCount = 0;
static std::vector<uint8_t> g_result;
static bool g_haveResult = false;
static bool g_haveEver = false;      // 只要拿到过一次结果，之后每帧都贴（避免 NR/原图交替闪烁）
static int g_frames = 0, g_written = 0;
static double g_sumSample = 0, g_sumWorker = 0;
static int g_nSample = 0, g_nWorker = 0;

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

static float g_editStrength = 1.0f;
static volatile float g_lookSat = 1.0f, g_lookGamma = 1.0f, g_lookHiCut = 0.0f;   // Look 层（F9 切换）     // 修正量强度（F11/F12 调整）
static void SetPass(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso,
                   UINT srv0, UINT srv1, UINT uavSlot) {
    cl->SetPipelineState(pso);
    cl->SetComputeRootSignature(g_rs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE base = g_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE a = base; a.ptr += (SIZE_T)srv0 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE b = base; b.ptr += (SIZE_T)srv1 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE u = base; u.ptr += (SIZE_T)uavSlot * g_inc;
    cl->SetComputeRootDescriptorTable(0, a);
    cl->SetComputeRootDescriptorTable(1, b);
    cl->SetComputeRootDescriptorTable(2, u);
    float stArr[8] = { g_editStrength, g_lookSat, g_lookGamma, g_lookHiCut, 0, 0, 0, 0 };
    cl->SetComputeRoot32BitConstants(3, 8, stArr, 0);
}

static void Resample(ID3D12GraphicsCommandList* cl, UINT srvSlot, UINT uavSlot, UINT w, UINT h) {
    SetPass(cl, g_pso.Get(), srvSlot, 1, uavSlot);      // t1/t2 填有效描述符即可
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

static void EditComposite(ID3D12GraphicsCommandList* cl, UINT baseSlot, UINT uavSlot, UINT w, UINT h) {
    SetPass(cl, g_psoEdit.Get(), baseSlot, 1, uavSlot);  // t1=NR输出 t2=NR输入（槽 1,2）
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}
static bool BuildResources(ID3D12Device* dev, ID3D12Resource* color, ID3D12Resource* outp) {
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    {
        D3D12_RESOURCE_DESC cd = color->GetDesc();
        kNetW = 1920;   // 已验证组合：1920x1080 + RGBA8
        kNetH = 1080;
        Log("网络输入尺寸改为跟随 pColor: %ux%u", kNetW, kNetH);
    }
    g_outW = (UINT)od.Width;
    g_outH = od.Height;

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;

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
        D3D12_RESOURCE_DESC d2 = td;
        d2.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d2,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)g_netIn.GetAddressOf()))) { Log("g_netIn 失败"); return false; }
    }
    {
        D3D12_RESOURCE_DESC d = td;
        d.Width = g_outW; d.Height = g_outH;
        d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)g_big.GetAddressOf()))) { Log("g_big 失败"); return false; }
    }
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
        D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)g_upNet.GetAddressOf()))) { Log("g_upNet 失败"); return false; }
        for (int i = 0; i < 2; ++i) {
            if (FAILED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)g_rbNet[i].GetAddressOf()))) { Log("g_rbNet 失败"); return false; }
        }
        g_result.assign((size_t)total, 0);
        Log("网络缓冲: %ux%u bytes=%llu 输出 %ux%u", kNetW, kNetH, (unsigned long long)total, g_outW, g_outH);
    }

    D3D12_DESCRIPTOR_RANGE srvA{}, srvB{}, uavR{};
    srvA.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srvA.NumDescriptors = 1; srvA.BaseShaderRegister = 0;
    srvB.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srvB.NumDescriptors = 3; srvB.BaseShaderRegister = 1;
    uavR.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavR.NumDescriptors = 1; uavR.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[4] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &srvA;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &srvB;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &uavR;
    D3D12_STATIC_SAMPLER_DESC sm{};
    sm.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sm.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sm.MaxLOD = D3D12_FLOAT32_MAX;
    sm.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[3].Constants.ShaderRegister = 0;
    rp[3].Constants.RegisterSpace = 0;
    rp[3].Constants.Num32BitValues = 8;
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rsd.NumParameters = 4; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &sm;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { Log("根签名序列化失败"); return false; }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)g_rs.GetAddressOf()))) { Log("根签名失败"); return false; }

    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "x.hlsl", nullptr, nullptr, "ResampleCS", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) { Log("shader 编译失败"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)g_pso.GetAddressOf()))) { Log("PSO 失败"); return false; }
    {
        ComPtr<ID3DBlob> cs2, e3;
        if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "x.hlsl", nullptr, nullptr, "EditCS", "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs2, &e3))) { Log("EditCS 编译失败"); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd2{};
        pd2.pRootSignature = g_rs.Get();
        pd2.CS.pShaderBytecode = cs2->GetBufferPointer();
        pd2.CS.BytecodeLength = cs2->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&pd2, __uuidof(ID3D12PipelineState), (void**)g_psoEdit.GetAddressOf()))) { Log("EditPSO 失败"); return false; }
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)g_heap.GetAddressOf()))) { Log("描述符堆失败"); return false; }
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_heap->GetCPUDescriptorHandleForHeapStart();

    auto mkSrv = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)slot * g_inc;
        dev->CreateShaderResourceView(r, &sd, h);
    };
    auto mkUav = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = fmt;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)slot * g_inc;
        dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
    };
    mkSrv(g_netIn.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);        // t1: NR 输出
    mkSrv(g_net.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 2);           // t2: NR 输入
    mkSrv(outp, DXGI_FORMAT_R10G10B10A2_UNORM, 6);               // t0: 4K 底图（真实 XeSS 输出）
    mkUav(g_net.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 4);           // u0: 下采样目标
    mkUav(g_big.Get(), DXGI_FORMAT_R10G10B10A2_UNORM, 5);        // u0: 合成目标
    mkSrv(color, DXGI_FORMAT_B8G8R8A8_UNORM, 0);                 // t0: 游戏 color

    {
        D3D12_HEAP_PROPERTIES dh2{};
        dh2.Type = D3D12_HEAP_TYPE_DEFAULT;
        dh2.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        dh2.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        dh2.CreationNodeMask = 1; dh2.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC dt{};
        dt.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dt.Width = kNetW; dt.Height = kNetH; dt.DepthOrArraySize = 1; dt.MipLevels = 1;
        dt.SampleDesc.Count = 1; dt.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dt.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        dt.Format = DXGI_FORMAT_R32_FLOAT;
        dev->CreateCommittedResource(&dh2, D3D12_HEAP_FLAG_NONE, &dt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)g_dep1080.GetAddressOf());
        dt.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
        dev->CreateCommittedResource(&dh2, D3D12_HEAP_FLAG_NONE, &dt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)g_mv1080.GetAddressOf());
        if (!g_dep1080 || !g_mv1080) { Log("depth/MV 目标创建失败"); return false; }
    }
    {
        D3D12_RESOURCE_DESC bdD{}, bdM{};
        D3D12_HEAP_PROPERTIES uh2 = dh; uh2.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_HEAP_PROPERTIES rh2 = dh; rh2.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC td2{};
        td2.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td2.Width = kNetW; td2.Height = kNetH; td2.DepthOrArraySize = 1; td2.MipLevels = 1;
        td2.SampleDesc.Count = 1; td2.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td2.Format = DXGI_FORMAT_R32_FLOAT;
        UINT r3 = 0; UINT64 rs3 = 0;
        dev->GetCopyableFootprints(&td2, 0, 1, 0, &g_fpDep, &r3, &rs3, &g_bytesDep);
        td2.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
        dev->GetCopyableFootprints(&td2, 0, 1, 0, &g_fpMv, &r3, &rs3, &g_bytesMv);
        bdD.Dimension = bdM.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bdD.Width = g_bytesDep; bdM.Width = g_bytesMv;
        bdD.Height = bdM.Height = 1; bdD.DepthOrArraySize = bdM.DepthOrArraySize = 1;
        bdD.MipLevels = bdM.MipLevels = 1;
        bdD.Format = bdM.Format = DXGI_FORMAT_UNKNOWN;
        bdD.SampleDesc.Count = bdM.SampleDesc.Count = 1;
        bdD.Layout = bdM.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        dev->CreateCommittedResource(&uh2, D3D12_HEAP_FLAG_NONE, &bdD, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)g_upDep.GetAddressOf());
        dev->CreateCommittedResource(&uh2, D3D12_HEAP_FLAG_NONE, &bdM, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)g_upMv.GetAddressOf());
        dev->CreateCommittedResource(&rh2, D3D12_HEAP_FLAG_NONE, &bdD, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)g_rbDep.GetAddressOf());
        dev->CreateCommittedResource(&rh2, D3D12_HEAP_FLAG_NONE, &bdM, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)g_rbMv.GetAddressOf());
        Log("depth 目标 %llu 字节, MV 目标 %llu 字节", (unsigned long long)g_bytesDep, (unsigned long long)g_bytesMv);
    }
    // depth / MV 回读缓冲（按各自格式的 footprint）
    auto mkRb = [&](ID3D12Resource* src, ComPtr<ID3D12Resource>& out, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT64& bytes) -> bool {
        D3D12_RESOURCE_DESC sd = src->GetDesc();
        UINT rows = 0; UINT64 rowSize = 0, total = 0;
        dev->GetCopyableFootprints(&sd, 0, 1, 0, &fp, &rows, &rowSize, &total);
        bytes = total;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rh2{};
        rh2.Type = D3D12_HEAP_TYPE_READBACK;
        rh2.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        rh2.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        rh2.CreationNodeMask = 1; rh2.VisibleNodeMask = 1;
        return SUCCEEDED(dev->CreateCommittedResource(&rh2, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)out.GetAddressOf()));
    };
    {
        ID3D12Resource* mv = (ID3D12Resource*)((const unsigned __int64*)g_firstParams)[1];
        ID3D12Resource* dp = (ID3D12Resource*)((const unsigned __int64*)g_firstParams)[2];
        if (mv) { if (!mkRb(mv, g_rbMv, g_fpMv, g_bytesMv)) Log("MV 回读缓冲失败"); else Log("MV 回读缓冲 %llu 字节", (unsigned long long)g_bytesMv); }
        if (dp) { if (!mkRb(dp, g_rbDepth, g_fpDepth, g_bytesDepth)) Log("depth 回读缓冲失败"); else Log("depth 回读缓冲 %llu 字节", (unsigned long long)g_bytesDepth); }
    }
    Log("游戏侧资源就绪");
    return true;
}

// ---------------- 拦截 ----------------
// ---------------- 三道保险 ----------------
// 1) 总开关文件：游戏目录放 xess_proxy_off.txt 即完全不接管（救命用）
// 2) 每帧开销看门狗：连续 30 帧提交超过 3ms → 永久关闭
// 3) 显存看门狗：用量超过预算 92% → 永久关闭
static bool g_disabled = false;
static volatile bool g_nrOn = true;
static DWORD WINAPI HotkeyThread(LPVOID) {
    bool p10 = false, p7 = false, p8 = false;
    while (true) {
        bool d10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        bool d7  = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        bool d8  = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (d10 && !p10) {
            g_nrOn = !g_nrOn;
            Log("*** F10: NR %s ***", g_nrOn ? "开（叠加网络修正量）" : "关（游戏原生 4K）");
        }
        if (d7 && !p7) {
            g_editStrength = (g_editStrength > 0.25f) ? g_editStrength - 0.25f : 0.0f;
            Log("*** F7: 强度 = %.1f ***", g_editStrength);
        }
        if (d8 && !p8) {
            g_editStrength = (g_editStrength < 2.0f) ? g_editStrength + 0.25f : 2.0f;
            Log("*** F8: 强度 = %.1f ***", g_editStrength);
        }
        p10 = d10; p7 = d7; p8 = d8;
        static bool prevF9 = false;
        bool d9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (d9 && !prevF9) {
            static int look = 0;
            look = (look + 1) % 3;
            if (look == 0) { g_lookSat = 1.0f; g_lookGamma = 1.0f; g_lookHiCut = 0.0f; Log("*** F9: Look 关 ***"); }
            else if (look == 1) { g_lookSat = 1.12f; g_lookGamma = 0.95f; g_lookHiCut = 0.5f; Log("*** F9: Look 轻 ***"); }
            else { g_lookSat = 1.25f; g_lookGamma = 0.90f; g_lookHiCut = 1.0f; Log("*** F9: Look 强 ***"); }
        }
        prevF9 = d9;
        Sleep(40);
    }
    return 0;
}

static int g_slowStreak = 0;

static void QueryVram(ID3D12Device* dev, double* usedMB, double* budgetMB) {
    *usedMB = 0; *budgetMB = 0;
    if (!dev) return;
    LUID luid = dev->GetAdapterLuid();
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) return;
    for (UINT a = 0; a < 8; ++a) {
        ComPtr<IDXGIAdapter1> ad;
        if (fac->EnumAdapters1(a, &ad) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        if (d.AdapterLuid.HighPart != luid.HighPart || d.AdapterLuid.LowPart != luid.LowPart) continue;
        ComPtr<IDXGIAdapter3> a3;
        if (SUCCEEDED(ad.As(&a3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                *usedMB = (double)info.CurrentUsage / (1024.0 * 1024.0);
                *budgetMB = (double)info.Budget / (1024.0 * 1024.0);
            }
        }
        break;
    }
}

static void GuardCheck(double frameMs, ID3D12Device* dev, int frameNo) {
    if (g_disabled) return;
    // 每帧开销
    if (frameMs > 6.0) {
        if (++g_slowStreak >= 60) {
            g_disabled = true;
            Log("!! 保险：连续 60 帧提交超过 6ms（本次 %.2fms），永久关闭集成", frameMs);
            return;
        }
    } else {
        g_slowStreak = 0;
    }
    // 显存（每 300 帧查一次）
    if (frameNo > 0 && frameNo % 300 == 0) {
        double used = 0, budget = 0;
        QueryVram(dev, &used, &budget);
        Log("VRAM: 已用 %.0f MB / 预算 %.0f MB (%.1f%%)", used, budget, budget > 0 ? used * 100.0 / budget : 0.0);
        if (budget > 0 && used > budget * 0.92) {
            g_disabled = true;
            Log("!! 保险：显存用量超过预算 92%%，永久关闭集成");
        }
    }
}


extern "C" int ProxyExecute(void* ctx, void* cmdList, const void* params) {
    using Fn = int(*)(void*, void*, const void*);
    Fn real = (Fn)g_xessTab[30];

    if (!params || !cmdList) return real ? real(ctx, cmdList, params) : -1;
    if (g_disabled) return real ? real(ctx, cmdList, params) : -1;     // 保险：总开关

    const unsigned __int64* q = (const unsigned __int64*)params;
    if (!g_firstParams) g_firstParams = params;
    ID3D12Resource* color = (ID3D12Resource*)q[0];
    ID3D12Resource* outp = (ID3D12Resource*)q[5];
    // 记录 pColor / pOutput 尺寸（XeSS 档位一变，这里就会变）
    {
        static UINT lastCW = 0, lastCH = 0, lastOW = 0, lastOH = 0;
        if (color) {
            D3D12_RESOURCE_DESC cd = color->GetDesc();
            if ((UINT)cd.Width != lastCW || cd.Height != lastCH) {
                lastCW = (UINT)cd.Width; lastCH = cd.Height;
                Log("*** color 尺寸变化: %ux%u (fmt=%d) ***", lastCW, lastCH, (int)cd.Format);
            }
        }
        if (outp) {
            D3D12_RESOURCE_DESC od = outp->GetDesc();
            if ((UINT)od.Width != lastOW || od.Height != lastOH) {
                lastOW = (UINT)od.Width; lastOH = od.Height;
                Log("*** 输出尺寸变化: %ux%u (fmt=%d) ***", lastOW, lastOH, (int)od.Format);
            }
        }
    }
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmdList;

    static bool inited = false;
    static bool gaveUp = false;   // 关键：失败一次就永久放弃，绝不每帧重试（否则显存爆炸、显卡被拖垮）
    if (gaveUp) return real ? real(ctx, cmdList, params) : -1;
    if (!inited) {
        if (!color || !outp) return real ? real(ctx, cmdList, params) : -1;
        if (FAILED(color->GetDevice(__uuidof(ID3D12Device), (void**)g_dev.GetAddressOf())) || !g_dev) {
            gaveUp = true; Log("!! 取设备失败，永久放弃集成");
            return real ? real(ctx, cmdList, params) : -1;
        }
        bool okRes = BuildResources(g_dev.Get(), color, outp);
        bool okWorker = okRes && WorkerInit();
        if (!okRes || !okWorker) {
            gaveUp = true;
            Log("!! 初始化失败（资源=%d worker=%d），永久放弃，始终使用真实 XeSS", (int)okRes, (int)okWorker);
            return real ? real(ctx, cmdList, params) : -1;
        }
        g_worker.th = std::thread(WorkerMain);
        inited = true;
        Log("初始化完成，开始跨卡 NR");
    }

    // 运动矢量的 SRV（惰性创建，放在描述符槽 3）
    {
        static bool mvSrvReady = false;
        ID3D12Resource* mv = (ID3D12Resource*)q[1];
        if (!mvSrvReady && mv && g_heap) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)3 * g_inc;
            g_dev->CreateShaderResourceView(mv, &sd, h);
            mvSrvReady = true;
            Log("运动矢量 SRV 已建立 (fmt=%d)", (int)mv->GetDesc().Format);
        }
    }

    // 读上一帧抓到的 depth / MV，做数值校验
    if (g_pendMv && g_rbMv) {
        g_pendMv = false;
        D3D12_RANGE rr{ 0, (SIZE_T)g_bytesMv };
        void* m = nullptr;
        if (SUCCEEDED(g_rbMv->Map(0, &rr, &m)) && m) {
            const short* s = (const short*)m;
            double sum = 0; short mx = 0; size_t cnt = 0;
            for (UINT64 off = 0; off + 8 < g_bytesMv; off += 4096) {
                const short* p2 = (const short*)((const unsigned char*)m + off);
                short a = p2[0], b = p2[1];
                sum += (double)abs(a) + abs(b);
                if (abs(a) > mx) mx = abs(a);
                if (abs(b) > mx) mx = abs(b);
                cnt += 2;
            }
            (void)s;
            if (g_statCount < 3 || g_statCount % 120 == 0)
                Log("MV 校验: 平均|mv| %.1f 最大 %d (SNORM 32767=1.0)", cnt ? sum / cnt : 0.0, (int)mx);
            D3D12_RANGE nw{ 0, 0 };
            g_rbMv->Unmap(0, &nw);
        }
    }
    if (g_pendDepth && g_rbDepth) {
        g_pendDepth = false;
        D3D12_RANGE rr{ 0, (SIZE_T)g_bytesDepth };
        void* m = nullptr;
        if (SUCCEEDED(g_rbDepth->Map(0, &rr, &m)) && m) {
            double sum = 0; size_t cnt = 0; uint32_t mn = 0xFFFFFFFF, mx = 0;
            for (UINT64 off = 0; off + 4 < g_bytesDepth; off += 4096) {
                uint32_t v = *(const uint32_t*)((const unsigned char*)m + off);
                sum += v; cnt++;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            if (g_statCount < 3 || g_statCount % 120 == 0)
                Log("Depth 校验: 均值 %.0f min=%u max=%u (D32)", cnt ? sum / cnt : 0.0, mn, mx);
            g_statCount++;
            D3D12_RANGE nw{ 0, 0 };
            g_rbDepth->Unmap(0, &nw);
        }
    }

    auto tA = Clock::now();

    // ① 收上一帧回读 → 喂工作线程；有结果就留下
    for (int i = 0; i < 2; ++i) {
        if (!g_pending[i]) continue;
        g_pending[i] = false;
        D3D12_RANGE rr{ 0, (SIZE_T)g_bytesNet };
        void* m = nullptr;
        if (SUCCEEDED(g_rbNet[i]->Map(0, &rr, &m)) && m) {
            std::lock_guard<std::mutex> lk(g_worker.m);
            if (g_worker.hasOut) {
                g_result = g_worker.outBuf;
                g_worker.hasOut = false;
                g_haveResult = true;
                g_haveEver = true;      // 一旦拿到结果，之后每帧都贴
            }
            if (!g_worker.hasIn && g_worker.ok) {
                g_worker.inBuf.assign((uint8_t*)m, (uint8_t*)m + g_bytesNet);
                g_worker.hasIn = true;
                g_worker.cv.notify_one();
            }
            D3D12_RANGE nw{ 0, 0 };
            g_rbNet[i]->Unmap(0, &nw);
        }
    }

    // ② 照常调用真实 XeSS（兜底）
    int rc = real ? real(ctx, cmdList, params) : -1;

    // ③ 有 NR 结果 → 只把修正量叠加到真实 XeSS 的 4K 底图上（F10 可关）
    if (g_haveEver && g_nrOn) {
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (SUCCEEDED(g_upNet->Map(0, &nr, &m)) && m) {
            memcpy(m, g_result.data(), g_result.size());
            D3D12_RANGE wr{ 0, (SIZE_T)g_bytesNet };
            g_upNet->Unmap(0, &wr);

            D3D12_TEXTURE_COPY_LOCATION dd{};
            dd.pResource = g_netIn.Get();
            dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ss{};
            ss.pResource = g_upNet.Get();
            ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            ss.PlacedFootprint = g_fpNet;
            cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
            Barrier(cl, g_netIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            ID3D12DescriptorHeap* heaps[] = { g_heap.Get() };
            cl->SetDescriptorHeaps(1, heaps);
            Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, g_net.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            EditComposite(cl, 6, 5, g_outW, g_outH);
            Barrier(cl, g_net.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, g_big.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, outp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

            D3D12_TEXTURE_COPY_LOCATION bd2{};
            bd2.pResource = outp;
            bd2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bd2.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION bs2{};
            bs2.pResource = g_big.Get();
            bs2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bs2.SubresourceIndex = 0;
            cl->CopyTextureRegion(&bd2, 0, 0, 0, &bs2, nullptr);

            Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, g_big.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, g_netIn.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g_written++;
        }
    }

    // ④ 抓帧：pColor → g_net(1920x1080) → 回读
    {
        ID3D12DescriptorHeap* heaps[] = { g_heap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        Resample(cl, 0, 4, kNetW, kNetH);   // pColor(槽0) → g_net UAV(槽4)
        Barrier(cl, g_net.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION rd{};
        rd.pResource = g_rbNet[g_rbIdx].Get();
        rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rd.PlacedFootprint = g_fpNet;
        D3D12_TEXTURE_COPY_LOCATION rs{};
        rs.pResource = g_net.Get();
        rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rs.SubresourceIndex = 0;
        cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
        Barrier(cl, g_net.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_pending[g_rbIdx] = true;
        g_rbIdx ^= 1;
    }

    // 记录 depth / MV 拷贝（数值校验用）
    {
        ID3D12Resource* mv = (ID3D12Resource*)q[1];
        ID3D12Resource* dp = (ID3D12Resource*)q[2];
        if (mv && g_rbMv) {
            Barrier(cl, mv, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION rd{};
            rd.pResource = g_rbMv.Get();
            rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rd.PlacedFootprint = g_fpMv;
            D3D12_TEXTURE_COPY_LOCATION rs{};
            rs.pResource = mv;
            rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            rs.SubresourceIndex = 0;
            cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
            Barrier(cl, mv, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g_pendMv = true;
        }
        if (dp && g_rbDepth) {
            Barrier(cl, dp, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION rd{};
            rd.pResource = g_rbDepth.Get();
            rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rd.PlacedFootprint = g_fpDepth;
            D3D12_TEXTURE_COPY_LOCATION rs{};
            rs.pResource = dp;
            rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            rs.SubresourceIndex = 0;
            cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
            Barrier(cl, dp, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_READ);
            g_pendDepth = true;
        }
    }

    g_frames++;
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - tA).count();
    g_sumSample += ms; g_nSample++;
    {
        std::lock_guard<std::mutex> lk(g_worker.m);
        g_sumWorker += g_worker.lastMs; g_nWorker++;
    }
    GuardCheck(ms, g_dev.Get(), g_frames);
    if (g_frames % 120 == 0)
        Log("stats: frames=%d 写回=%d 提交=%.3fms worker=%.2fms processed=%d",
            g_frames, g_written, g_sumSample / g_nSample,
            g_nWorker ? g_sumWorker / g_nWorker : 0.0, g_worker.processed);

    return rc;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        OpenLog();
        {
            // 允许/指定高分辨率档：运行时靠这些环境变量决定网络尺寸
            SetEnvironmentVariableA("AMDNR_NATIVE_PW", "2560");
            SetEnvironmentVariableA("AMDNR_NATIVE_PH", "1440");
            SetEnvironmentVariableA("AMDNR_DEV_NETWORK_HEIGHT", "1440");
            SetEnvironmentVariableA("AMDNR_ALLOW_SLOW_GPU", "1");
            Log("已设置档位环境变量: NATIVE=2560x1440 ALLOW_SLOW_GPU=1");
        }
        Log("libxess proxy v3 (跨卡 NR) attached");
        {
            std::string off = ModuleDir() + "xess_proxy_off.txt";
            if (GetFileAttributesA(off.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_disabled = true;
                Log("检测到 xess_proxy_off.txt：完全不接管（保险生效）");
            }
        }
        InitForwarders();
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);   // F10 开关
        Log("转发就绪 (%s)", g_real ? "libxess_orig.dll 已加载" : "真实 DLL 加载失败");
    }
    return TRUE;
}
