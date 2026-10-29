// libxess.dll 代理 v4 —— 按 AMDNR 的实测配方驱动 LmxxfNrRuntime
//
// 配方来自运行时自己的日志：
//   lmxxf inputs: colour fmt 24 state 8 | motion fmt 13 | depth fmt 19 inverted
//   rebuild=tier frame 3840x2160 (colour allocation 3840x2160), network fed 1920x1080
//   控制量需要 struct_size 144
//
// 也就是：
//   color = 3840x2160, R10G10B10A2_UNORM, 状态 = UNORDERED_ACCESS(8)
//   运行时自己降到 1080p 跑网络，再把 edit 抬回 4K 原地写回
//
// 链路：
//   pd-upscaler → xessD3D12Execute(color 2227x1253, MV, depth, jitter, output 3840x2160)
//     ① pColor 放大成 4K / R10G10B10A2 → 回读 33MB
//     ② 跨卡 → 9070 XT → lmxxf（4K 进出，原地写回）→ 回读 33MB
//     ③ 下一帧：上传 → 直接拷进 pOutput（格式相同）
//     ④ 始终照常调用真实 XeSS 兜底

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
static volatile bool g_nrOn = true;     // F10

static float g_strength = 1.0f;         // F7/F8（靠 transfer_strength 传给运行时）

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

struct FrameInfo {                 // 必须 144 字节才有 look 控制
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
    void* depth;
    void* velocity;
    float ctlTone;
    float ctlStructure;
    float ctlSkin;
    float ctlOther;
    float ctlStyle;
    uint32_t ctlPad;
};

struct Job { uint32_t struct_size; void* handle; void* private_output; };

struct CreateInfo {                // 40 字节
    uint32_t struct_size;
    uint32_t pad0;
    void* device;
    void* queue;
    const wchar_t* assets_directory;
    void* reserved0;
};

// ---------------- 尺寸/格式 ----------------
static UINT g_w = 0, g_h = 0;      // 与输出一致（4K）
static const DXGI_FORMAT kFmt = DXGI_FORMAT_R10G10B10A2_UNORM;

// ---------------- 上采样 shader（pColor → 4K 10bit）----------------
static const char* kHLSL = R"(
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

// ---------------- worker（9070 XT）----------------
struct Worker {
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> inBuf, outBuf;
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
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    HMODULE mod = nullptr;
    ApiTable api{};
    void* ctx = nullptr;
    bool ok = false;
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
    DXGI_ADAPTER_DESC d{};
    ad->GetDesc(&d);
    Log("worker: 适配器 = %ls", d.Description);
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(W.dev.GetAddressOf())))) { Log("worker: 设备失败"); return false; }
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
    td.Width = g_w; td.Height = g_h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kFmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(W.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(W.color.GetAddressOf())))) {
        Log("worker: color 失败"); return false;
    }
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
    if (!getApi || getApi(1, &W.api) != 0) { Log("worker: GetApi 失败"); return false; }

    CreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = W.dev.Get();
    ci.queue = W.q.Get();
    ci.assets_directory = pak.c_str();
    if (W.api.Create(&ci, &W.ctx) != 0) { Log("worker: Create 失败"); return false; }
    if (W.api.PrepareSession(W.ctx) != 0) { Log("worker: PrepareSession 失败"); return false; }
    {
        char st[1024] = { 0 };
        if (W.api.GetStatus) W.api.GetStatus(W.ctx, st, 1024);
        Log("worker: 会话就绪 (%ux%u, fm24, UAV) 状态: %s", g_w, g_h, st);
    }
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

            // 上传 → color（先进 COPY_DEST，再转 UAV）
            W.al->Reset();
            W.cl->Reset(W.al.Get(), nullptr);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = W.color.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            W.cl->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dd{};
            dd.pResource = W.color.Get();
            dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ss{};
            ss.pResource = W.up.Get();
            ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            ss.PlacedFootprint = W.fp;
            W.cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            W.cl->ResourceBarrier(1, &b);
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
                fi.color_width = g_w;
                fi.color_height = g_h;
                fi.color = W.color.Get();
                fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS;   // 8，实测配方
                fi.transfer_strength = g_strength;
                fi.color_strength = 1.0f;
                fi.model_scale = 1.0f;
                fi.pre_exposure = 1.0f;
                fi.exposure_scale = 1.0f;
                fi.depth = nullptr;
                fi.velocity = nullptr;
                fi.ctlTone = 1.0f;
                fi.ctlStructure = 1.0f;
                fi.ctlSkin = 3.0f;
                fi.ctlOther = 2.0f;
                fi.ctlStyle = 0.0f;

                int32_t rcPF = W.api.PrepareFrame(W.ctx, &fi, &job);
                if (rcPF != 0) {
                    if (W.processed < 3) {
                        char eb[256] = { 0 };
                        if (W.api.GetLastError) W.api.GetLastError(eb, 256);
                        Log("worker: PrepareFrame 失败 rc=%d err=%s (%ux%u fmt24 UAV)", (int)rcPF, eb, g_w, g_h);
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
            }

            // 结果回读（运行时已原地写回 color）
            W.al->Reset();
            W.cl->Reset(W.al.Get(), nullptr);
            D3D12_RESOURCE_BARRIER b2{};
            b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b2.Transition.pResource = W.color.Get();
            b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b2.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            W.cl->ResourceBarrier(1, &b2);
            D3D12_TEXTURE_COPY_LOCATION rd{};
            rd.pResource = W.rb.Get();
            rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rd.PlacedFootprint = W.fp;
            D3D12_TEXTURE_COPY_LOCATION rs{};
            rs.pResource = W.color.Get();
            rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            rs.SubresourceIndex = 0;
            W.cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
            b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b2.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            W.cl->ResourceBarrier(1, &b2);
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
static ComPtr<ID3D12Resource> g_up4k;      // pColor → 4K 10bit（UAV）
static ComPtr<ID3D12Resource> g_in4k;      // 结果上传目标（COPY_DEST）
static ComPtr<ID3D12Resource> g_rbBuf;     // 4K 回读
static ComPtr<ID3D12Resource> g_upBuf;     // 4K 上传
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp4k{};
static UINT64 g_bytes4k = 0;
static std::vector<uint8_t> g_result;
static bool g_haveEver = false, g_haveNew = false;
static int g_frames = 0, g_written = 0;
static bool g_disabled = false;
static int g_slow = 0;

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

static bool BuildResources(ID3D12Device* dev, ID3D12Resource* color, ID3D12Resource* outp) {
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    g_w = (UINT)od.Width;
    g_h = od.Height;
    Log("目标: %ux%u fmt=%d", g_w, g_h, (int)od.Format);

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = g_w; td.Height = g_h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kFmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(g_up4k.GetAddressOf())))) { Log("g_up4k 失败"); return false; }
    {
        D3D12_RESOURCE_DESC d2 = td;
        d2.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &d2,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(g_in4k.GetAddressOf())))) { Log("g_in4k 失败"); return false; }
        UINT rows = 0; UINT64 rowSize = 0, total = 0;
        dev->GetCopyableFootprints(&d2, 0, 1, 0, &g_fp4k, &rows, &rowSize, &total);
        g_bytes4k = total;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(g_upBuf.GetAddressOf())))) { Log("g_upBuf 失败"); return false; }
        if (FAILED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(g_rbBuf.GetAddressOf())))) { Log("g_rbBuf 失败"); return false; }
        g_result.assign((size_t)total, 0);
        Log("4K 缓冲: bytes=%llu", (unsigned long long)total);
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
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { Log("根签名失败"); return false; }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(g_rs.GetAddressOf())))) { Log("RS 失败"); return false; }

    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "x.hlsl", nullptr, nullptr, "UpCS", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) { Log("shader 失败"); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(g_pso.GetAddressOf())))) { Log("PSO 失败"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(g_heap.GetAddressOf())))) { Log("堆失败"); return false; }
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(color, &sd, c);

    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = kFmt;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE c1 = c; c1.ptr += g_inc;
    dev->CreateUnorderedAccessView(g_up4k.Get(), nullptr, &ud, c1);
    Log("游戏侧资源就绪");
    return true;
}

// ---------------- 拦截 ----------------
extern "C" int ProxyExecute(void* ctx, void* cmdList, const void* params) {
    using Fn = int(*)(void*, void*, const void*);
    Fn real = (Fn)g_xessTab[30];
    if (!params || !cmdList) return real ? real(ctx, cmdList, params) : -1;
    if (g_disabled) return real ? real(ctx, cmdList, params) : -1;

    const unsigned __int64* q = (const unsigned __int64*)params;
    ID3D12Resource* color = (ID3D12Resource*)q[0];
    ID3D12Resource* outp = (ID3D12Resource*)q[5];
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmdList;

    static bool inited = false, gaveUp = false;
    if (gaveUp) return real ? real(ctx, cmdList, params) : -1;
    if (!inited) {
        if (!color || !outp) return real ? real(ctx, cmdList, params) : -1;
        if (FAILED(color->GetDevice(__uuidof(ID3D12Device), (void**)g_dev.GetAddressOf())) || !g_dev) { gaveUp = true; return real ? real(ctx, cmdList, params) : -1; }
        bool okRes = BuildResources(g_dev.Get(), color, outp);
        bool okW = okRes && WorkerInit();
        if (!okRes || !okW) { gaveUp = true; Log("!! 初始化失败，永久放弃"); return real ? real(ctx, cmdList, params) : -1; }
        g_worker.th = std::thread(WorkerMain);
        inited = true;
        Log("初始化完成（4K/10bit/UAV 配方）");
    }

    auto tA = Clock::now();

    // 收上一帧回读 → 喂 worker；取结果
    {
        static bool pend = false;
        if (pend) {
            pend = false;
            D3D12_RANGE rr{ 0, (SIZE_T)g_bytes4k };
            void* m = nullptr;
            if (SUCCEEDED(g_rbBuf->Map(0, &rr, &m)) && m) {
                std::lock_guard<std::mutex> lk(g_worker.m);
                if (g_worker.hasOut) {
                    memcpy(g_result.data(), g_worker.outBuf.data(), (size_t)g_bytes4k);
                    g_worker.hasOut = false;
                    g_haveNew = true;
                    g_haveEver = true;
                }
                if (!g_worker.hasIn && g_worker.ok && g_worker.inBuf.size() == (size_t)g_bytes4k) {
                    memcpy(g_worker.inBuf.data(), m, (size_t)g_bytes4k);
                    g_worker.hasIn = true;
                    g_worker.cv.notify_one();
                }
                D3D12_RANGE nw{ 0, 0 };
                g_rbBuf->Unmap(0, &nw);
            }
        }
        // 抓帧：pColor → 4K 10bit → 回读
        {
            ID3D12DescriptorHeap* heaps[] = { g_heap.Get() };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetPipelineState(g_pso.Get());
            cl->SetComputeRootSignature(g_rs.Get());
            D3D12_GPU_DESCRIPTOR_HANDLE b = g_heap->GetGPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE s = b;
            D3D12_GPU_DESCRIPTOR_HANDLE u = b; u.ptr += g_inc;
            cl->SetComputeRootDescriptorTable(0, s);
            cl->SetComputeRootDescriptorTable(1, u);
            Barrier(cl, g_up4k.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl->Dispatch((g_w + 7) / 8, (g_h + 7) / 8, 1);
            Barrier(cl, g_up4k.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION rd{};
            rd.pResource = g_rbBuf.Get();
            rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            rd.PlacedFootprint = g_fp4k;
            D3D12_TEXTURE_COPY_LOCATION rs{};
            rs.pResource = g_up4k.Get();
            rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            rs.SubresourceIndex = 0;
            cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
            Barrier(cl, g_up4k.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            pend = true;
        }
    }

    int rc = real ? real(ctx, cmdList, params) : -1;

    // 写回：有结果就拷进 pOutput（同格式）
    if (g_haveEver && g_nrOn) {
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (SUCCEEDED(g_upBuf->Map(0, &nr, &m)) && m) {
            memcpy(m, g_result.data(), g_result.size());
            D3D12_RANGE wr{ 0, (SIZE_T)g_bytes4k };
            g_upBuf->Unmap(0, &wr);

            D3D12_TEXTURE_COPY_LOCATION dd{};
            dd.pResource = g_in4k.Get();
            dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dd.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ss{};
            ss.pResource = g_upBuf.Get();
            ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            ss.PlacedFootprint = g_fp4k;
            cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);

            Barrier(cl, g_in4k.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION bd2{};
            bd2.pResource = outp;
            bd2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bd2.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION bs2{};
            bs2.pResource = g_in4k.Get();
            bs2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bs2.SubresourceIndex = 0;
            cl->CopyTextureRegion(&bd2, 0, 0, 0, &bs2, nullptr);
            Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, g_in4k.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            g_written++;
        }
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
        if (d10 && !p10) { g_nrOn = !g_nrOn; Log("*** F10: NR %s ***", g_nrOn ? "开" : "关（原生 4K）"); }
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
        Log("libxess proxy v4 (4K/10bit/UAV) attached");
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
