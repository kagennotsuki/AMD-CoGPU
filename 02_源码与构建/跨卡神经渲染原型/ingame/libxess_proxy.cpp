// libxess.dll 代理（v1：只记录，不改行为）
//
// 目的：pd-upscaler（REFramework 的 TemporalUpscaler）会调用 libxess 的
//       xessD3D12Execute，并把 color / depth / velocity / output / jitter
//       全部递进来。我们要在这里把它们拦下来（后续交给 9070 XT 上的 lmxxf）。
//
// 本版本只做两件事：
//   1) 所有 72 个导出原样转发到 libxess_orig.dll（XeSS 行为完全不变）
//   2) 拦 xessD3D12Execute，把参数结构体的前 160 字节按 qword 打出来，
//      并对其中像资源指针的字段做 SEH 保护的 GetDesc 探测（坏指针直接跳过）
//
// 跑一次就能知道结构体布局，之后再按实测布局写死。

#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <share.h>

// ---------- 转发目标表 ----------
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

static void OpenLog() {
    if (g_log) return;
    char path[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    if (!n) return;
    std::string s(path);
    size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) s = s.substr(0, p + 1);
    s += "xess_proxy.log";
    g_log = _fsopen(s.c_str(), "w", _SH_DENYNO);
}

static HMODULE RealXess() {
    if (g_real) return g_real;
    char path[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    if (!n) return nullptr;
    std::string s(path);
    size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) s = s.substr(0, p + 1);
    s += "libxess_orig.dll";
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

// ---------- 回读游戏 color（验证我们能拿到真实内容）----------
static ID3D12Resource* g_rb = nullptr;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp{};
static UINT64 g_rbBytes = 0;
static UINT g_rbW = 0, g_rbH = 0;
static bool g_pending = false;
static int g_reads = 0;

static bool EnsureReadback(ID3D12Device* dev, ID3D12Resource* color) {
    D3D12_RESOURCE_DESC d = color->GetDesc();
    if (g_rb && g_rbW == d.Width && g_rbH == d.Height) return true;
    if (g_rb) { g_rb->Release(); g_rb = nullptr; g_pending = false; }
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &g_fp, &rows, &rowSize, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_rb)))) {
        Log("!! 回读缓冲创建失败");
        return false;
    }
    g_rbBytes = total; g_rbW = (UINT)d.Width; g_rbH = (UINT)d.Height;
    Log("回读缓冲就绪 %ux%u bytes=%llu", g_rbW, g_rbH, (unsigned long long)g_rbBytes);
    return true;
}

// 上一帧的拷贝应已完成，读出来看内容
static void DrainReadback() {
    if (!g_pending || !g_rb) return;
    g_pending = false;
    D3D12_RANGE rr{ 0, (SIZE_T)g_rbBytes };
    void* m = nullptr;
    if (FAILED(g_rb->Map(0, &rr, &m)) || !m) { Log("!! Map 回读失败"); return; }
    const unsigned char* p = (const unsigned char*)m;
    // 采样求平均亮度 + 看是否全零
    unsigned long long sum = 0; int cnt = 0; int nonzero = 0;
    for (UINT64 i = 0; i + 4 < g_rbBytes; i += 4099) {
        unsigned b = p[i], g2 = p[i + 1], r = p[i + 2];
        sum += (b + g2 + r) / 3;
        if (p[i] || p[i + 1] || p[i + 2]) nonzero++;
        cnt++;
    }
    Log("color 回读 #%d: 均值 %.1f, 非零采样 %d/%d, 首像素 B=%u G=%u R=%u",
        g_reads++, cnt ? (double)sum / cnt : 0.0, nonzero, cnt, p[0], p[1], p[2]);
    D3D12_RANGE nw{ 0, 0 };
    g_rb->Unmap(0, &nw);
}
// ---------- 测试用：往 pOutput 左上角写纯绿方块 ----------
// 目的：验证"我们能不能改游戏最终画面"（pOutput 是游戏设备上的普通纹理）
static ID3D12Resource* g_patch = nullptr;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_patchFp{};
static const UINT kPatchW = 480, kPatchH = 480;

static bool EnsureGreenPatch(ID3D12Device* dev) {
    if (g_patch) return true;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kPatchW; td.Height = kPatchH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&td, 0, 1, 0, &g_patchFp, &rows, &rowSize, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_patch)))) {
        Log("!! green patch 缓冲创建失败");
        return false;
    }
    D3D12_RANGE nr{ 0, 0 };
    void* m = nullptr;
    if (SUCCEEDED(g_patch->Map(0, &nr, &m)) && m) {
        // R10G10B10A2：A=3, B=0, G=1023, R=0 → 纯绿
        const uint32_t green = (3u << 30) | (0u << 20) | (1023u << 10) | 0u;
        for (UINT y = 0; y < kPatchH; ++y) {
            uint32_t* row = (uint32_t*)((unsigned char*)m + g_patchFp.Offset
                                        + (UINT64)y * g_patchFp.Footprint.RowPitch);
            for (UINT x = 0; x < kPatchW; ++x) row[x] = green;
        }
        D3D12_RANGE wr{ 0, (SIZE_T)total };
        g_patch->Unmap(0, &wr);
        Log("green patch 就绪 %ux%u (total=%llu)", kPatchW, kPatchH, (unsigned long long)total);
    }
    return true;
}
// ---------- 安全探测资源描述 ----------
static bool TryDesc(void* p, D3D12_RESOURCE_DESC* out) {
    if (!p) return false;
    __try {
        ID3D12Resource* r = (ID3D12Resource*)p;
        *out = r->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------- 被拦截的导出 ----------
// xess_result_t xessD3D12Execute(xess_context_handle_t, ID3D12GraphicsCommandList*,
//                                const xess_d3d12_execute_params_t*)
extern "C" int ProxyExecute(void* ctx, void* cmdList, const void* params) {
    static int calls = 0;
    ++calls;
    DrainReadback();      // 上一帧的拷贝此时应已完成
    const bool dump = (calls <= 4) || (calls % 300 == 0);

    if (dump && g_log) {
        Log("================================================================");
        Log("xessD3D12Execute #%d  ctx=%p cmdList=%p params=%p", calls, ctx, cmdList, params);
        if (params) {
            const unsigned __int64* q = (const unsigned __int64*)params;
            for (int i = 0; i < 20; ++i) {
                Log("  params[%2d] (+%3d) = 0x%016llX", i, i * 8, (unsigned long long)q[i]);
            }
            // 对前 14 个字段试当资源指针
            for (int i = 0; i < 14; ++i) {
                D3D12_RESOURCE_DESC d{};
                if (TryDesc((void*)q[i], &d)) {
                    Log("  -> [%2d] 资源 %llux%llu fmt=%d dim=%d flags=0x%llX",
                        i, (unsigned long long)d.Width, (unsigned long long)d.Height,
                        (int)d.Format, (int)d.Dimension, (unsigned long long)d.Flags);
                }
            }
            // 前 8 个 float（jitter / exposure 之类常在前部或尾部）
            const float* f = (const float*)params;
            Log("  floats: %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f",
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        }
    }

    using Fn = int(*)(void*, void*, const void*);
    Fn real = (Fn)g_xessTab[30];   // 序号 31 = xessD3D12Execute（0 基 30）
    if (!real) { Log("!! 真实 xessD3D12Execute 未解析"); return -1; }
    int rc = real(ctx, cmdList, params);
    if (dump) Log("  rc=%d", rc);

    // 测试：往 pOutput 左上角写纯绿（验证能否改最终画面）
    {
        const unsigned __int64* q = (const unsigned __int64*)params;
        ID3D12Resource* outp = (ID3D12Resource*)q[5];
        ID3D12GraphicsCommandList* cl2 = (ID3D12GraphicsCommandList*)cmdList;
        static int patchWrites = 0;
        if (outp && cl2 && patchWrites < 100000) {
            ID3D12Device* dev2 = nullptr;
            if (SUCCEEDED(outp->GetDevice(IID_PPV_ARGS(&dev2))) && dev2) {
                if (EnsureGreenPatch(dev2)) {
                    D3D12_RESOURCE_BARRIER b2{};
                    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b2.Transition.pResource = outp;
                    b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    cl2->ResourceBarrier(1, &b2);
                    D3D12_TEXTURE_COPY_LOCATION dd{};
                    dd.pResource = outp;
                    dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dd.SubresourceIndex = 0;
                    D3D12_TEXTURE_COPY_LOCATION ss{};
                    ss.pResource = g_patch;
                    ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    ss.PlacedFootprint = g_patchFp;
                    D3D12_BOX box{ 0, 0, 0, kPatchW, kPatchH, 1 };
                    cl2->CopyTextureRegion(&dd, 0, 0, 0, &ss, &box);
                    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                    cl2->ResourceBarrier(1, &b2);
                    if (patchWrites < 3) Log("已记录写 pOutput（第 %d 次）", patchWrites + 1);
                    patchWrites++;
                }
                dev2->Release();
            }
        }
    }

    // 记录 color → 回读缓冲（下一帧读；证明我们能拿到游戏真实画面内容）
    if (params && cmdList) {
        const unsigned __int64* q = (const unsigned __int64*)params;
        ID3D12Resource* color = (ID3D12Resource*)q[0];
        ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmdList;
        if (color && cl && !g_pending) {
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(color->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
                if (EnsureReadback(dev, color)) {
                    D3D12_RESOURCE_BARRIER bb{};
                    bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    bb.Transition.pResource = color;
                    bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    bb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    cl->ResourceBarrier(1, &bb);
                    D3D12_TEXTURE_COPY_LOCATION dd{};
                    dd.pResource = g_rb;
                    dd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dd.PlacedFootprint = g_fp;
                    D3D12_TEXTURE_COPY_LOCATION ss{};
                    ss.pResource = color;
                    ss.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    ss.SubresourceIndex = 0;
                    cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
                    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    bb.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    cl->ResourceBarrier(1, &bb);
                    g_pending = true;
                }
                dev->Release();
            }
        }
    }
    return rc;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        OpenLog();
        Log("libxess proxy attached");
        InitForwarders();
        Log("forwarders ready (%s)", g_real ? "libxess_orig.dll 已加载" : "真实 DLL 加载失败");
    }
    return TRUE;
}
