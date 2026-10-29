// libxess.dll 代理 v5 —— pre 布局（借 AMDNR 的 AmdPlacement=pre）
//
// AMDNR 的配置（OptiScaler.ini [DlssNr]）：
//   AmdPlacement = pre          NR 作用在上采样"之前"的低分辨率帧
//   LmxxfFullNetwork = true     AmdRefineMode = full
//   AmdNeuralLightingStrength = 1.15   AmdTemporalStability = 0.75
//   LocalStructure = 0.9  SkinStructure = 1.35
// 所以：编辑同一个低分辨率帧 → 交给真实 XeSS 上采样 → 不变软、不错位。
//
// 我们这条跨卡链：
//   pd-upscaler → xessD3D12Execute(color 2227x1253, MV, depth, jitter, output 3840x2160)
//     ① pColor 降到 1920x1080 → 回读 8.3MB
//     ② 跨卡 → 9070 XT → lmxxf（struct_size=144，带 look 控制）→ 结果 1080p
//     ③ 下一帧：结果放大回 pColor 的几何 → **写进 pColor**
//     ④ 真实 XeSS 用这个已编辑的帧去上采样 → 4K
//
// 另：每 200 帧打印运行时状态，观察 hip / 档位何时就绪。

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

static HMODULE g_self = nullptr;
static HMODULE g_real = nullptr;
static FILE* g_log = nullptr;
static volatile bool g_nrOn = true;
static float g_strength = 1.0f;
static int g_lookTone = 1, g_lookStruct = 1, g_lookSkin = 3, g_lookOther = 2, g_lookStyle = 0;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static std::string ModuleDir() {
    char p[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(g_self, p, MAX_PATH);
    if (!n) return std::string();
    std::string s(p);
    size_t q = s.find_last_of("\\/");
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
    if (!g_real) Log("!! 加载失败 %s (err=%lu)", s.c_str(), GetLastError());
    return g_real;
}

static void InitForwarders() {
    HMODULE m = RealXess();
    for (int i = 0; i < 72; ++i) {
        g_xessTab[i] = m ? (void*)GetProcAddress(m, kNames[i]) : nullptr;
        if (!g_xessTab[i]) Log("!! 缺少导出: %s", kNames[i]);
    }
}

// ---------------- lmxxf ----------------
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
    void* extra0; void* extra1;
};

struct FrameInfo {                 // 144 字节：带 look 控制
    uint32_t struct_size;
    uint64_t session_id; uint64_t frame_id; uint64_t list_generation;
    void* command_list;
    uint32_t color_width, color_height;
    void* color;
    uint32_t color_state, flags;
    float transfer_strength, color_strength;
    uint32_t debug_view;
    float model_scale;
    void* exposure;
    uint32_t exposure_state;
    float pre_exposure, exposure_scale;
    void* depth;
    void* velocity;
    float ctlTone, ctlStructure, ctlSkin, ctlOther, ctlStyle;
    uint32_t ctlPad;
};

struct Job { uint32_t struct_size; void* handle; void* private_output; };
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; void* reserved0; };

static const UINT kNetW = 1920, kNetH = 1080;    // 唯一被接受的网络几何
static UINT g_cw = 0, g_ch = 0;                  // pColor 的几何（pre 布局写回目标）

static const char* kHLSL = R"(
Texture2D<float4>   gSrc : register(t0);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);

[numthreads(8, 8, 1)]
void ResampleCS(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    gDst[id.xy] = gSrc.SampleLevel(gSmp, uv, 0);
}
)";

// ---------------- worker ----------------
struct Worker {
    std::thread th; std::mutex m; std::condition_variable cv;
    std::vector<uint8_t> inBuf, outBuf;
    bool hasIn = false, hasOut = false, quit = false;
    int processed = 0; double lastMs = 0;
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al; ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fe; UINT64 fv = 0;
    ComPtr<ID3D12Resource> color, up, rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes = 0;
    HMODULE mod = nullptr; ApiTable api{}; void* ctx = nullptr; bool ok = false;
};
static Worker g_worker;

static void WaitW() {
    Worker& W = g_worker;
    W.q->Signal(W.fe.Get(), ++W.fv);
    while (W.fe->GetCompletedValue() < W.fv) Sleep(0);
}

static bool WorkerInit() {
    Worker& W = g_worker;
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) return false;
    ComPtr<IDXGIAdapter1> ad;
    if (fac->EnumAdapters1(1, ad.GetAddressOf()) != S_OK) { Log("worker: 适配器1缺失"); return false; }
    DXGI_ADAPTER_DESC d{}; ad->GetDesc(&d);
    Log("worker: %ls", d.Description);
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(W.dev.GetAddressOf())))) return false;
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

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kNetW; td.Height = kNetH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(W.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(W.color.GetAddressOf())))) return false;
    D3D12_RESOURCE_DESC sd = td; sd.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    W.dev->GetCopyableFootprints(&sd, 0, 1, 0, &W.fp, &rows, &rowSize, &total);
    W.bytes = total;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(W.up.GetAddressOf()));
    W.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(W.rb.GetAddressOf()));
    W.inBuf.assign((size_t)total, 0);
    W.outBuf.assign((size_t)total, 0);

    std::string dir = ModuleDir();
    std::wstring pak(dir.begin(), dir.end());
    pak += L"LmxxfNrRuntime.pak";
    std::wstring dllp = pak;
    size_t sl = dllp.find_last_of(L'\\');
    if (sl != std::wstring::npos) dllp = dllp.substr(0, sl + 1);
    dllp += L"LmxxfNrRuntime.dll";
    W.mod = LoadLibraryW(dllp.c_str());
    if (!W.mod) { Log("worker: lmxxf 加载失败"); return false; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(W.mod, "LmxxfNrGetApi");
    W.api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!getApi || getApi(1, &W.api) != 0) return false;

    CreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = W.dev.Get(); ci.queue = W.q.Get(); ci.assets_directory = pak.c_str();
    if (W.api.Create(&ci, &W.ctx) != 0) { Log("worker: Create 失败"); return false; }
    if (W.api.PrepareSession(W.ctx) != 0) { Log("worker: PrepareSession 失败"); return false; }
    Log("worker: 会话就绪 (%ux%u RGBA8 COMMON)", kNetW, kNetH);
    W.ok = true;
    return true;
}

static void WorkerMain() {
    Worker& W = g_worker;
    std::vector<uint8_t> local;
    int frameNo = 0;
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
            WaitW();

            if (W.api.PrepareFrame) {
                Job job{};
                job.struct_size = (uint32_t)sizeof(Job);
                FrameInfo fi{};
                fi.struct_size = 144;
                fi.session_id = 1;
                fi.frame_id = (uint64_t)W.processed + 1;
                fi.list_generation = 1;
                fi.command_list = W.cl.Get();
                fi.color_width = kNetW;
                fi.color_height = kNetH;
                fi.color = W.color.Get();
                fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
                fi.transfer_strength = g_strength;
                fi.color_strength = 1.0f;
                fi.model_scale = 1.0f;
                fi.pre_exposure = 1.0f;
                fi.exposure_scale = 1.0f;
                fi.ctlTone = (float)g_lookTone;
                fi.ctlStructure = (float)g_lookStruct;
                fi.ctlSkin = (float)g_lookSkin;
                fi.ctlOther = (float)g_lookOther;
                fi.ctlStyle = (float)g_lookStyle;

                int32_t rc = W.api.PrepareFrame(W.ctx, &fi, &job);
                if (rc != 0) {
                    if (W.processed < 3) {
                        char eb[256] = { 0 };
                        if (W.api.GetLastError) W.api.GetLastError(eb, 256);
                        Log("worker: PrepareFrame 失败 rc=%d err=%s", (int)rc, eb);
                    }
                } else {
                    W.al->Reset();
                    W.cl->Reset(W.al.Get(), nullptr);
                    if (W.api.RecordInputs(W.ctx, job.handle, W.cl.Get()) == 0) {
                        W.cl->Close();
                        { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                        WaitW();
                        if (W.api.ExecuteAfterProducer(W.ctx, job.handle, W.q.Get()) == 0) {
                            W.al->Reset();
                            W.cl->Reset(W.al.Get(), nullptr);
                            if (W.api.RecordOutputs(W.ctx, job.handle, W.cl.Get()) == 0) {
                                W.cl->Close();
                                { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                                WaitW();
                                W.api.Retire(W.ctx, job.handle);
                            }
                        }
                    }
                }
                // 每 200 帧看运行时状态（关注 hip 何时变 1、档位是否升）
                ++frameNo;
                if (frameNo % 200 == 0 && W.api.GetStatus) {
                    char st[1024] = { 0 };
                    W.api.GetStatus(W.ctx, st, 1024);
                    Log("运行时状态 @%d: %s", frameNo, st);
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
            WaitW();

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
            if (W.processed <= 3 || W.processed % 200 == 0)
                Log("worker: frame %d 完成 %.2f ms", W.processed, ms);
        }
    }
}

// ---------------- 游戏侧 ----------------
static ComPtr<ID3D12Device> g_dev;
static ComPtr<ID3D12RootSignature> g_rs;
static ComPtr<ID3D12PipelineState> g_pso;
static ComPtr<ID3D12DescriptorHeap> g_heap;
static UINT g_inc = 0;
static ComPtr<ID3D12Resource> g_small;     // 下采样目标 1920x1080 RGBA8 (UAV)
static ComPtr<ID3D12Resource> g_nrIn;      // 结果上传目标 1920x1080 RGBA8 (COPY_DEST→SRV)
static ComPtr<ID3D12Resource> g_pre;       // pColor 几何的编辑结果 (UAV)
static ComPtr<ID3D12Resource> g_rbBuf, g_upBuf;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fpNet{}, g_fpPre{};
static UINT64 g_bytesNet = 0, g_bytesPre = 0;
static std::vector<uint8_t> g_result;
static bool g_haveEver = false, g_disabled = false;
static int g_frames = 0, g_written = 0, g_slow = 0;

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

static bool BuildResources(ID3D12Device* dev, ID3D12Resource* color, ID3D12Resource* outp) {
    D3D12_RESOURCE_DESC cd = color->GetDesc();
    g_cw = (UINT)cd.Width; g_ch = cd.Height;
    DXGI_FORMAT cf = cd.Format;
    Log("pColor: %ux%u fmt=%d", g_cw, g_ch, (int)cf);

    g_small = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g_nrIn = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, false);
    g_pre = MakeTex(dev, g_cw, g_ch, cf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    if (!g_small || !g_nrIn || !g_pre) { Log("纹理创建失败"); return false; }

    // 缓冲
    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC nd{};
    nd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    nd.Width = kNetW; nd.Height = kNetH; nd.DepthOrArraySize = 1; nd.MipLevels = 1;
    nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nd.SampleDesc.Count = 1;
    nd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    UINT rows = 0; UINT64 rowSize = 0;
    dev->GetCopyableFootprints(&nd, 0, 1, 0, &g_fpNet, &rows, &rowSize, &g_bytesNet);

    D3D12_RESOURCE_DESC pd = nd;
    pd.Width = g_cw; pd.Height = g_ch; pd.Format = cf;
    dev->GetCopyableFootprints(&pd, 0, 1, 0, &g_fpPre, &rows, &rowSize, &g_bytesPre);

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = g_bytesNet; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;
    dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(g_upBuf.GetAddressOf()));
    dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(g_rbBuf.GetAddressOf()));
    g_result.assign((size_t)g_bytesNet, 0);

    // 根签名 + PSO
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
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(g_rs.GetAddressOf())))) return false;
    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "x.hlsl", nullptr, nullptr, "ResampleCS", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) { Log("shader 失败"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
    cp.pRootSignature = g_rs.Get();
    cp.CS.pShaderBytecode = cs->GetBufferPointer();
    cp.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(g_pso.GetAddressOf())))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
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
    mkSrv(color, DXGI_FORMAT_B8G8R8A8_UNORM, 0);              // pColor
    mkUav(g_small.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);      // 下采样目标
    mkSrv(g_nrIn.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 2);       // NR 结果
    mkUav(g_pre.Get(), cf, 3);                                // 写回 pColor 前的目标
    Log("资源就绪: net %ux%u, pre %ux%u fmt=%d", kNetW, kNetH, g_cw, g_ch, (int)cf);
    return true;
}

static void Dispatch(ID3D12GraphicsCommandList* cl, UINT srvSlot, UINT uavSlot, UINT w, UINT h) {
    cl->SetPipelineState(g_pso.Get());
    cl->SetComputeRootSignature(g_rs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE b = g_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE s = b; s.ptr += (SIZE_T)srvSlot * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE u = b; u.ptr += (SIZE_T)uavSlot * g_inc;
    cl->SetComputeRootDescriptorTable(0, s);
    cl->SetComputeRootDescriptorTable(1, u);
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

extern "C" int ProxyExecute(void* ctx, void* cmdList, const void* params) {
    using Fn = int(*)(void*, void*, const void*);
    Fn real = (Fn)g_xessTab[30];
    if (!params || !cmdList) return real ? real(ctx, cmdList, params) : -1;
    if (g_disabled) return real ? real(ctx, cmdList, params) : -1;

    const unsigned __int64* q = (const unsigned __int64*)params;
    ID3D12Resource* color = (ID3D12Resource*)q[0];
    ID3D12Resource* outp = (ID3D12Resource*)q[5];
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmdList;

    static bool inited = false, gaveUp = false, pend = false;
    if (gaveUp) return real ? real(ctx, cmdList, params) : -1;
    if (!inited) {
        if (!color || !outp) return real ? real(ctx, cmdList, params) : -1;
        if (FAILED(color->GetDevice(__uuidof(ID3D12Device), (void**)g_dev.GetAddressOf())) || !g_dev) { gaveUp = true; return real ? real(ctx, cmdList, params) : -1; }
        if (!BuildResources(g_dev.Get(), color, outp) || !WorkerInit()) {
            gaveUp = true; Log("!! 初始化失败，永久放弃"); return real ? real(ctx, cmdList, params) : -1;
        }
        g_worker.th = std::thread(WorkerMain);
        inited = true;
        Log("初始化完成（pre 布局）");
    }

    auto tA = Clock::now();

    // ① 收上一帧回读 → worker；取结果
    if (pend) {
        pend = false;
        D3D12_RANGE rr{ 0, (SIZE_T)g_bytesNet };
        void* m = nullptr;
        if (SUCCEEDED(g_rbBuf->Map(0, &rr, &m)) && m) {
            std::lock_guard<std::mutex> lk(g_worker.m);
            if (g_worker.hasOut) {
                memcpy(g_result.data(), g_worker.outBuf.data(), (size_t)g_bytesNet);
                g_worker.hasOut = false;
                g_haveEver = true;
            }
            if (!g_worker.hasIn && g_worker.ok && g_worker.inBuf.size() == (size_t)g_bytesNet) {
                memcpy(g_worker.inBuf.data(), m, (size_t)g_bytesNet);
                g_worker.hasIn = true;
                g_worker.cv.notify_one();
            }
            D3D12_RANGE nw{ 0, 0 };
            g_rbBuf->Unmap(0, &nw);
        }
    }

    // ② pre 布局：把上一帧的 NR 结果放大回 pColor 几何，写进 pColor（XeSS 之后会用它上采样）
    if (g_haveEver && g_nrOn) {
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (SUCCEEDED(g_upBuf->Map(0, &nr, &m)) && m) {
            memcpy(m, g_result.data(), g_result.size());
            D3D12_RANGE wr{ 0, (SIZE_T)g_bytesNet };
            g_upBuf->Unmap(0, &wr);

            D3D12_TEXTURE_COPY_LOCATION dd{};
            dd.pResource = g_nrIn.Get();
            dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ss{};
            ss.pResource = g_upBuf.Get();
            ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            ss.PlacedFootprint = g_fpNet;
            cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);

            Barrier(cl, g_nrIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ID3D12DescriptorHeap* heaps[] = { g_heap.Get() };
            cl->SetDescriptorHeaps(1, heaps);
            Dispatch(cl, 2, 3, g_cw, g_ch);      // 1080p 结果 → pColor 几何
            Barrier(cl, g_nrIn.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

            D3D12_TEXTURE_COPY_LOCATION cd{};
            cd.pResource = color;
            cd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION cs{};
            cs.pResource = g_pre.Get();
            cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cs.SubresourceIndex = 0;
            cl->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);

            Barrier(cl, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_written++;
        }
    }

    // ③ 真实 XeSS（用我们编辑过的 pColor 上采样）
    int rc = real ? real(ctx, cmdList, params) : -1;

    // ④ 抓帧：pColor → 1080p → 回读
    {
        ID3D12DescriptorHeap* heaps[] = { g_heap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        Dispatch(cl, 0, 1, kNetW, kNetH);
        Barrier(cl, g_small.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rd{};
        rd.pResource = g_rbBuf.Get();
        rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rd.PlacedFootprint = g_fpNet;
        D3D12_TEXTURE_COPY_LOCATION rs{};
        rs.pResource = g_small.Get();
        rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rs.SubresourceIndex = 0;
        cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
        Barrier(cl, g_small.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pend = true;
    }

    g_frames++;
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - tA).count();
    if (ms > 12.0) { if (++g_slow >= 60) { g_disabled = true; Log("!! 保险：连续 60 帧提交 >12ms，永久关闭"); } }
    else g_slow = 0;
    if (g_frames % 200 == 0) {
        std::lock_guard<std::mutex> lk(g_worker.m);
        Log("stats: frames=%d 写回=%d 提交=%.3fms worker=%.2fms processed=%d",
            g_frames, g_written, ms, g_worker.lastMs, g_worker.processed);
    }
    return rc;
}

static DWORD WINAPI HotkeyThread(LPVOID) {
    bool p10 = false, p7 = false, p8 = false;
    while (true) {
        bool d10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        bool d7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        bool d8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (d10 && !p10) { g_nrOn = !g_nrOn; Log("*** F10: NR %s ***", g_nrOn ? "开（pre 编辑）" : "关（原样）"); }
        if (d7 && !p7) { g_strength = (g_strength > 0.5f) ? g_strength - 0.5f : 0.0f; Log("*** F7: 强度 %.1f ***", g_strength); }
        if (d8 && !p8) { g_strength = (g_strength < 4.0f) ? g_strength + 0.5f : 4.0f; Log("*** F8: 强度 %.1f ***", g_strength); }
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
        Log("libxess proxy v5 (pre 布局) attached");
        {
            std::string off = ModuleDir() + "xess_proxy_off.txt";
            if (GetFileAttributesA(off.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_disabled = true;
                Log("检测到 xess_proxy_off.txt：完全不接管");
            }
        }
        InitForwarders();
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
        Log("转发就绪");
    }
    return TRUE;
}
