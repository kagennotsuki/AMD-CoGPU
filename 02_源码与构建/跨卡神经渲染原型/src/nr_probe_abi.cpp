// nr_probe_abi.cpp —— lmxxf 运行时 ABI 扫描探针 v2（离线，不碰游戏）
//
// v1 的教训：4K 颜色会让会话 fatal（"codec unverified input format/geometry"）并进入
// "session is poisoned"，后面所有实验全废。所以 v2 改成：**每个配置独立 Create/PrepareSession**，
// 出错就销毁重建，保证互不污染。
//
// 要回答：
//   A. struct_size 只有哪些合法（104/120/144/152/160/168）
//   B. 历史掩码放哪个偏移（+144 候选），单独加掩码够不够
//   C. 加 motion / 加 depth 之后 GetStatus 的 hist= 会不会变 on，输出会不会变
//   D. 4K 颜色是否确定不可用（caps: max 1920x1080，像素预算 ~2.07M）
//   E. 颜色像素预算边界（1920x1080 vs 2227x1253）
//   F. flags 合法位
//   G. 表里 slot18/slot19 两个入口是什么、能不能安全调用

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "LmxxfNrApi.h"

using Microsoft::WRL::ComPtr;

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
    void* slot18;
    void* slot19;
};

struct FrameInfoMax {
    LmxxfNrFrameInfo base;   // 0..103
    uint64_t depth;          // 104
    uint64_t velocity;       // 112
    float ctlTone;           // 120
    float ctlStructure;      // 124
    float ctlSkin;           // 128
    float ctlOther;          // 132
    float ctlStyle;          // 136
    uint32_t ctlPad;         // 140
    uint64_t extra144;       // 144（历史掩码候选）
    uint64_t extra152;       // 152
    uint64_t extra160;       // 160
};
static_assert(sizeof(LmxxfNrFrameInfo) == 104, "base must be 104");
static_assert(sizeof(FrameInfoMax) == 168, "max must be 168");

static const wchar_t* kGameDir = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2";
static const wchar_t* kAssets = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";

static const UINT kNetW = 1920, kNetH = 1080;
static const UINT kUpsW = 2227, kUpsH = 1253;
static const UINT kFrameW = 3840, kFrameH = 2160;

static ApiTable g_api{};
static ComPtr<ID3D12Device> g_dev;
static ComPtr<ID3D12CommandQueue> g_q;
static ComPtr<ID3D12CommandAllocator> g_al;
static ComPtr<ID3D12GraphicsCommandList> g_cl;
static ComPtr<ID3D12Fence> g_fe;
static UINT64 g_fv = 0;
static D3D12_HEAP_PROPERTIES g_dh{}, g_uh{}, g_rh{};

static const char* St(int32_t s) {
    switch (s) {
        case 0: return "OK";
        case 1: return "UNSUPPORTED_ABI";
        case 2: return "INVALID_ARGUMENT";
        case 3: return "NOT_IMPLEMENTED";
        case 4: return "UNAVAILABLE";
        case 5: return "FAILED";
        default: return "?";
    }
}

static void SubmitWait() {
    g_cl->Close();
    ID3D12CommandList* l[] = { g_cl.Get() };
    g_q->ExecuteCommandLists(1, l);
    g_q->Signal(g_fe.Get(), ++g_fv);
    while (g_fe->GetCompletedValue() < g_fv) Sleep(0);
}
static void ResetList() { g_al->Reset(); g_cl->Reset(g_al.Get(), nullptr); }

static ComPtr<ID3D12Resource> MakeTex(UINT w, UINT h, DXGI_FORMAT fmt,
                                      D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = flags;
    ComPtr<ID3D12Resource> r;
    HRESULT hr = g_dev->CreateCommittedResource(&g_dh, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) printf("    !! 建纹理失败 %ux%u fmt=%d hr=0x%08X\n", w, h, (int)fmt, (unsigned)hr);
    return r;
}

struct Rb {
    ComPtr<ID3D12Resource> res;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    UINT rowPitch = 0;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

static Rb MakeRb(UINT w, UINT h, DXGI_FORMAT fmt) {
    Rb rb; rb.w = w; rb.h = h; rb.fmt = fmt;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    UINT rows = 0; UINT64 rowSize = 0;
    g_dev->GetCopyableFootprints(&td, 0, 1, 0, &rb.fp, &rows, &rowSize, &rb.bytes);
    rb.rowPitch = rb.fp.Footprint.RowPitch;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = rb.bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_dev->CreateCommittedResource(&g_rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                   nullptr, IID_PPV_ARGS(&rb.res));
    return rb;
}

static void CopyToRb(ID3D12Resource* src, Rb& rb, D3D12_RESOURCE_STATES from) {
    ResetList();
    if (from != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_cl->ResourceBarrier(1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = rb.res.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = rb.fp;
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s.SubresourceIndex = 0;
    g_cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    if (from != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = src;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = from;
        g_cl->ResourceBarrier(1, &b);
    }
    SubmitWait();
}

static void FillTexRGBA8(ID3D12Resource* dst, UINT w, UINT h, int frameNo) {
    D3D12_RESOURCE_DESC td = dst->GetDesc();
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    g_dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &total);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> up;
    g_dev->CreateCommittedResource(&g_uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                   nullptr, IID_PPV_ARGS(&up));
    D3D12_RANGE nr{ 0, 0 };
    void* m = nullptr;
    up->Map(0, &nr, &m);
    unsigned int seed = 12345u + (unsigned)frameNo * 7919u;
    for (UINT y = 0; y < h; ++y) {
        unsigned char* row = (unsigned char*)m + (size_t)y * fp.Footprint.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            float fx = (float)x / (float)w, fy = (float)y / (float)h;
            float r = 0.18f + 0.55f * fx, g = 0.22f + 0.45f * fy, b = 0.30f + 0.35f * (1.0f - fx) * fy;
            float dx = fx - 0.34f, dy = fy - 0.46f;
            if (dx * dx + dy * dy < 0.030f) { r = 0.85f; g = 0.30f; b = 0.22f; }
            if (fx > 0.62f && fx < 0.88f && fy > 0.18f && fy < 0.44f) { r = 0.20f; g = 0.78f; b = 0.42f; }
            float chk = ((((x / 24) + (y / 24)) & 1) ? 0.045f : -0.045f);
            seed = seed * 1664525u + 1013904223u;
            float n = ((float)((seed >> 16) & 0x3FF) / 1023.0f - 0.5f) * 0.14f;
            float t = (frameNo & 1) ? 0.008f : -0.008f;
            r += chk + n + t; g += chk * 0.8f + n * 0.9f - t; b += chk * 0.6f + n * 1.1f;
            if (r < 0) r = 0; if (r > 1) r = 1;
            if (g < 0) g = 0; if (g > 1) g = 1;
            if (b < 0) b = 0; if (b > 1) b = 1;
            row[x * 4 + 0] = (unsigned char)(r * 255.0f + 0.5f);
            row[x * 4 + 1] = (unsigned char)(g * 255.0f + 0.5f);
            row[x * 4 + 2] = (unsigned char)(b * 255.0f + 0.5f);
            row[x * 4 + 3] = 255;
        }
    }
    D3D12_RANGE wr{ 0, (SIZE_T)total };
    up->Unmap(0, &wr);
    ResetList();
    D3D12_TEXTURE_COPY_LOCATION d{};
    d.pResource = dst;
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION s{};
    s.pResource = up.Get();
    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s.PlacedFootprint = fp;
    g_cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    SubmitWait();
}

// 灌 depth（R32_FLOAT）与 motion（SNORM，每像素 (dx,dy)，单位=放大后像素）
static void FillDepthMV(ID3D12Resource* depth, ID3D12Resource* mv, UINT w, UINT h, float mvPix) {
    auto fill = [&](ID3D12Resource* dst, std::vector<unsigned char>& bytes, int bpp) {
        D3D12_RESOURCE_DESC td = dst->GetDesc();
        UINT rows = 0; UINT64 rowSize = 0, total = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        g_dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &total);
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> up;
        g_dev->CreateCommittedResource(&g_uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                       nullptr, IID_PPV_ARGS(&up));
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        up->Map(0, &nr, &m);
        for (UINT y = 0; y < h; ++y) {
            unsigned char* rowp = (unsigned char*)m + (size_t)y * fp.Footprint.RowPitch;
            memset(rowp, 0, (size_t)w * bpp);
            for (UINT x = 0; x < w; ++x) {
                if (bpp == 4) {
                    float d = 50.0f + 200.0f * (float)y / (float)h;    // 反向 Z 风格：数值大=近
                    if (x < w / 3) d = 10.0f;                          // 左侧一块"近景"
                    memcpy(rowp + (size_t)x * 4, &d, 4);
                } else {
                    short vx = (short)(mvPix / (float)w * 32767.0f);
                    short vy = (short)(mvPix / (float)h * 32767.0f);
                    short v[4] = { vx, vy, 0, 0 };
                    memcpy(rowp + (size_t)x * 8, v, 8);
                }
            }
        }
        D3D12_RANGE wr{ 0, (SIZE_T)total };
        up->Unmap(0, &wr);
        ResetList();
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = dst;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = up.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint = fp;
        g_cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        SubmitWait();
    };
    std::vector<unsigned char> dummy;
    if (depth) fill(depth, dummy, 4);
    if (mv) fill(mv, dummy, 8);
}

static std::string GetStatusStr(void* ctx) {
    char st[1024] = { 0 };
    if (g_api.GetStatus) g_api.GetStatus(ctx, st, 1024);
    return std::string(st);
}

static std::string StatusBits(const std::string& s) {
    if (s.empty()) return "(空)";
    std::string out;
    for (const char* key : { "hist=", "net=", "passes=", "hip=", "ctl=", "tp=" }) {
        size_t p = s.find(key);
        if (p == std::string::npos) continue;
        size_t e = s.find(' ', p);
        out += (e == std::string::npos) ? s.substr(p) : s.substr(p, e - p);
        out += " ";
    }
    size_t p = s.find("skip=");
    if (p != std::string::npos) out += s.substr(p, 20);
    return out;
}

struct Cfg {
    const char* name;
    uint32_t fiSize;
    uint32_t flags;
    int colorKind;   // 0=1920x1080 RGBA8, 1=2227x1253 RGBA8, 2=4K fmt24 UAV, 3=1600x900 RGBA8
    int depth;       // 0/1(R32_FLOAT)/2(D32S8)
    float mvPix;     // >0 = 提供 motion 且位移为该像素数；0 = 不提供
    int mask;        // 0=无, 1=R16F@4K, 2=R8@4K
    int frames;
    uint32_t debugView;
};

struct Result {
    int32_t rc = -1;
    char err[512] = { 0 };
    std::string status;
    UINT poW = 0, poH = 0;
    DXGI_FORMAT poFmt = DXGI_FORMAT_UNKNOWN;
    double poMean = 0;
    double editMean = -1, editMax = -1;
};

static Result RunConfig(const Cfg& cfg) {
    Result R{};
    void* ctx = nullptr;
    LmxxfNrCreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(ci);
    ci.device = g_dev.Get();
    ci.queue = g_q.Get();
    ci.assets_directory = kAssets;
    if (g_api.Create(&ci, &ctx) != 0) {
        if (g_api.GetLastError) g_api.GetLastError(R.err, 512);
        R.rc = -1;
        return R;
    }
    if (g_api.PrepareSession(ctx) != 0) {
        if (g_api.GetLastError) g_api.GetLastError(R.err, 512);
        R.rc = -2;
        R.status = StatusBits(GetStatusStr(ctx));
        g_api.Destroy(ctx);
        return R;
    }

    UINT cw = kNetW, ch = kNetH;
    DXGI_FORMAT cf = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_RESOURCE_STATES cs = D3D12_RESOURCE_STATE_COMMON;
    if (cfg.colorKind == 1) { cw = kUpsW; ch = kUpsH; }
    else if (cfg.colorKind == 2) { cw = kFrameW; ch = kFrameH; cf = DXGI_FORMAT_R10G10B10A2_UNORM; cs = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; }
    else if (cfg.colorKind == 3) { cw = 1600; ch = 900; }

    ComPtr<ID3D12Resource> color = MakeTex(cw, ch, cf, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, cs);
    ComPtr<ID3D12Resource> depth, mv, mask;
    if (cfg.depth == 1) depth = MakeTex(kUpsW, kUpsH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (cfg.depth == 2) depth = MakeTex(kUpsW, kUpsH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_READ);
    if (cfg.mvPix > 0.0f) mv = MakeTex(kUpsW, kUpsH, DXGI_FORMAT_R16G16B16A16_SNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (cfg.mask == 1) mask = MakeTex(kFrameW, kFrameH, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (cfg.mask == 2) mask = MakeTex(kFrameW, kFrameH, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (!color) { g_api.Destroy(ctx); R.rc = -3; return R; }

    if (depth || mv) FillDepthMV(depth.Get(), mv.Get(), kUpsW, kUpsH, cfg.mvPix);

    Rb fedRb, poRb;
    for (int f = 0; f < cfg.frames; ++f) {
        if (cf == DXGI_FORMAT_R8G8B8A8_UNORM) FillTexRGBA8(color.Get(), cw, ch, f);

        FrameInfoMax fi{};
        fi.base.struct_size = cfg.fiSize;
        fi.base.session_id = 1;
        fi.base.frame_id = (uint64_t)f + 1;
        fi.base.list_generation = 1;
        fi.base.command_list = g_cl.Get();
        fi.base.color_width = cw;
        fi.base.color_height = ch;
        fi.base.color = color.Get();
        fi.base.color_state = (uint32_t)cs;
        fi.base.flags = cfg.flags;
        fi.base.transfer_strength = 1.0f;
        fi.base.color_strength = 1.0f;
        fi.base.debug_view = cfg.debugView;
        fi.base.model_scale = 1.0f;
        fi.base.pre_exposure = 1.0f;
        fi.base.exposure_scale = 1.0f;
        fi.ctlTone = 1.0f; fi.ctlStructure = 1.0f; fi.ctlSkin = 3.0f; fi.ctlOther = 2.0f; fi.ctlStyle = 0.0f;
        if (depth) fi.depth = (uint64_t)depth.Get();
        if (mv) fi.velocity = (uint64_t)mv.Get();
        if (mask) fi.extra144 = (uint64_t)mask.Get();

        LmxxfNrJob job{};
        job.struct_size = (uint32_t)sizeof(job);
        R.rc = g_api.PrepareFrame(ctx, &fi.base, &job);
        if (R.rc != 0) {
            if (g_api.GetLastError) g_api.GetLastError(R.err, 512);
            R.status = StatusBits(GetStatusStr(ctx));
            g_api.Destroy(ctx);
            return R;
        }
        if (job.private_output) {
            D3D12_RESOURCE_DESC pd = ((ID3D12Resource*)job.private_output)->GetDesc();
            R.poW = (UINT)pd.Width; R.poH = pd.Height; R.poFmt = pd.Format;
        }
        ResetList();
        if (g_api.RecordInputs(ctx, job.handle, g_cl.Get()) != 0) {
            if (g_api.GetLastError) g_api.GetLastError(R.err, 512);
            R.rc = -4; R.status = StatusBits(GetStatusStr(ctx));
            if (g_api.CancelUnsubmitted) g_api.CancelUnsubmitted(ctx, job.handle);
            g_api.Destroy(ctx);
            return R;
        }
        SubmitWait();
        int32_t r2 = g_api.ExecuteAfterProducer(ctx, job.handle, g_q.Get());
        if (r2 != 0) {
            if (g_api.GetLastError) g_api.GetLastError(R.err, 512);
            R.rc = r2; R.status = StatusBits(GetStatusStr(ctx));
            if (g_api.CancelUnsubmitted) g_api.CancelUnsubmitted(ctx, job.handle);
            g_api.Destroy(ctx);
            return R;
        }
        ResetList();
        if (g_api.RecordOutputs(ctx, job.handle, g_cl.Get()) != 0) {
            if (g_api.GetLastError) g_api.GetLastError(R.err, 512);
            R.rc = -5; R.status = StatusBits(GetStatusStr(ctx));
            g_api.Destroy(ctx);
            return R;
        }
        SubmitWait();

        if (job.private_output && R.poFmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
            if (!poRb.res) poRb = MakeRb(R.poW, R.poH, R.poFmt);
            CopyToRb((ID3D12Resource*)job.private_output, poRb, D3D12_RESOURCE_STATE_COMMON);
            D3D12_RANGE rr{ 0, (SIZE_T)poRb.bytes };
            void* m = nullptr;
            if (SUCCEEDED(poRb.res->Map(0, &rr, &m)) && m) {
                size_t rowBytes = (size_t)R.poW * 4;
                double sum = 0; size_t cnt = 0;
                std::vector<unsigned char> tight(rowBytes * R.poH);
                for (UINT y = 0; y < R.poH; ++y)
                    memcpy(&tight[y * rowBytes], (unsigned char*)m + (size_t)y * poRb.rowPitch, rowBytes);
                for (size_t i = 0; i < tight.size(); ++i) { sum += tight[i]; cnt++; }
                R.poMean = cnt ? sum / (double)cnt : 0.0;
                // 与输入比：仅当几何/格式一致
                if (cf == DXGI_FORMAT_R8G8B8A8_UNORM && R.poW == cw && R.poH == ch) {
                    if (!fedRb.res) fedRb = MakeRb(cw, ch, cf);
                    CopyToRb(color.Get(), fedRb, cs);
                    D3D12_RANGE r2r{ 0, (SIZE_T)fedRb.bytes };
                    void* m2 = nullptr;
                    if (SUCCEEDED(fedRb.res->Map(0, &r2r, &m2)) && m2) {
                        double s2 = 0, mx = 0; size_t n = 0;
                        for (UINT y = 0; y < ch; ++y) {
                            const unsigned char* a = (unsigned char*)m2 + (size_t)y * fedRb.rowPitch;
                            const unsigned char* b = &tight[y * rowBytes];
                            for (UINT x = 0; x < cw; ++x)
                                for (int c = 0; c < 3; ++c) {
                                    int d = (int)b[x * 4 + c] - (int)a[x * 4 + c];
                                    if (d < 0) d = -d;
                                    s2 += (double)d / 255.0;
                                    if ((double)d / 255.0 > mx) mx = (double)d / 255.0;
                                    n++;
                                }
                        }
                        R.editMean = n ? s2 / (double)n : -1;
                        R.editMax = mx;
                        D3D12_RANGE nw{ 0, 0 };
                        fedRb.res->Unmap(0, &nw);
                    }
                }
                D3D12_RANGE nw{ 0, 0 };
                poRb.res->Unmap(0, &nw);
            }
        }
        g_api.Retire(ctx, job.handle);
    }
    R.status = StatusBits(GetStatusStr(ctx));
    g_api.Destroy(ctx);
    return R;
}

static void Report(const Cfg& cfg, const Result& R) {
    (void)cfg;
    printf("  rc=%-16s", St(R.rc));
    if (R.rc != 0) {
        if (R.err[0]) printf(" err=\"%s\"", R.err);
    } else {
        printf(" po=%ux%u fmt=%d mean=%.0f", R.poW, R.poH, (int)R.poFmt, R.poMean);
        if (R.editMean >= 0) printf("  edit(mean %.4f max %.4f)", R.editMean, R.editMax);
    }
    printf("\n        %s\n", R.status.c_str());
    fflush(stdout);
}

static int32_t CallSlotGuarded(void* fn, void* a, void* b, const char** tag) {
    __try {
        return ((int32_t(__stdcall*)(void*, void*))fn)(a, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *tag = "SEH 捕获访问冲突";
        return -12345;
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== lmxxf ABI 扫描探针 v2（每配置独立会话）===\n");

    HMODULE mod = LoadLibraryW((std::wstring(kGameDir) + L"\\LmxxfNrRuntime.dll").c_str());
    if (!mod) { printf("运行时加载失败 %lu\n", GetLastError()); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    g_api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!getApi || getApi(1, &g_api) != 0) { printf("GetApi 失败\n"); return 1; }
    printf("表 %u 字节 abi=%u slot18=%p slot19=%p\n", g_api.struct_size, g_api.abi_version, g_api.slot18, g_api.slot19);

    LmxxfNrCapabilities caps{};
    for (uint32_t sz : { (uint32_t)sizeof(caps), 44u, 40u, 36u }) {
        memset(&caps, 0, sizeof(caps));
        caps.struct_size = sz;
        if (g_api.QueryCapabilities(&caps) == 0) break;
    }
    printf("caps: max %ux%u history=%u overlap=%u hip_ready=%u\n",
           caps.max_input_width, caps.max_input_height, caps.history_supported, caps.overlap_supported, caps.hip_ready);

    ComPtr<IDXGIFactory6> fac;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));
    ComPtr<IDXGIAdapter1> ad;
    if (!fac || fac->EnumAdapters1(1, &ad) != S_OK) { printf("没有适配器 1\n"); return 1; }
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)))) { printf("设备失败\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_q));
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_al));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_al.Get(), nullptr, IID_PPV_ARGS(&g_cl));
    g_cl->Close();
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fe));
    g_dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    g_dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    g_dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    g_dh.CreationNodeMask = 1; g_dh.VisibleNodeMask = 1;
    g_uh = g_dh; g_uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    g_rh = g_dh; g_rh.Type = D3D12_HEAP_TYPE_READBACK;

    Cfg cfgs[] = {
        { "A1 1080p fi=120 基线",                       120, 0, 0, 0, 0.0f, 0, 4, 0 },
        { "A2 1080p fi=144 基线",                       144, 0, 0, 0, 0.0f, 0, 4, 0 },
        { "A3 1080p fi=160 无掩码",                     160, 0, 0, 0, 0.0f, 0, 4, 0 },
        { "B1 1080p fi=160 掩码R16F@4K",                160, 0, 0, 0, 0.0f, 1, 4, 0 },
        { "B2 1080p fi=160 掩码R8@4K",                  160, 0, 0, 0, 0.0f, 2, 4, 0 },
        { "C1 1080p fi=160 motion（16px）",             160, 0, 0, 0, 16.0f, 0, 4, 0 },
        { "C2 1080p fi=160 motion+掩码",                160, 0, 0, 0, 16.0f, 1, 4, 0 },
        { "C3 1080p fi=160 motion+depthR32F+掩码",      160, 0, 0, 1, 16.0f, 1, 4, 0 },
        { "C4 1080p fi=160 motion+depthD32S8+掩码",     160, 0, 0, 2, 16.0f, 1, 4, 0 },
        { "C5 1080p fi=144 motion only",                144, 0, 0, 0, 16.0f, 0, 4, 0 },
        { "D1 1600x900 fi=144（像素预算内）",            144, 0, 3, 0, 0.0f, 0, 3, 0 },
        { "D2 2227x1253 fi=144（超预算）",               144, 0, 1, 0, 0.0f, 0, 3, 0 },
        { "D3 4K fmt24 UAV fi=144",                     144, 0, 2, 0, 0.0f, 0, 3, 0 },
    };
    for (const Cfg& c : cfgs) {
        printf("---- %s ----\n", c.name);
        Report(c, RunConfig(c));
        printf("\n");
    }

    printf("---- E flags 合法位（1080p fi=144，每项独立会话）----\n");
    for (uint32_t fl : { 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u }) {
        Cfg c{ "flag", 144, fl, 0, 0, 0.0f, 0, 1, 0 };
        Result R = RunConfig(c);
        printf("  flags=0x%03X -> %-16s %s%s\n", fl, St(R.rc), R.err[0] ? "err=" : "", R.err);
        fflush(stdout);
    }

    printf("\n---- F 表项 slot18/slot19（独立会话，SEH 保护）----\n");
    {
        void* ctx = nullptr;
        LmxxfNrCreateInfo ci{};
        ci.struct_size = (uint32_t)sizeof(ci);
        ci.device = g_dev.Get(); ci.queue = g_q.Get(); ci.assets_directory = kAssets;
        if (g_api.Create(&ci, &ctx) == 0) {
            if (g_api.PrepareSession(ctx) == 0) {
                for (int si = 0; si < 2; ++si) {
                    void* fn = si == 0 ? g_api.slot18 : g_api.slot19;
                    printf("  %s=%p\n", si == 0 ? "slot18" : "slot19", fn);
                    if (!fn) continue;
                    for (uint32_t sz : { 8u, 16u, 32u, 64u, 128u }) {
                        unsigned char buf[512] = { 0 };
                        *(uint32_t*)buf = sz;
                        const char* tag = nullptr;
                        int32_t r = CallSlotGuarded(fn, ctx, buf, &tag);
                        char e[256] = { 0 };
                        if (r != -12345 && g_api.GetLastError) g_api.GetLastError(e, 256);
                        printf("    struct_size=%-4u -> %d %s%s%s\n", sz, r, tag ? tag : St(r),
                               e[0] ? " err=" : "", e);
                        fflush(stdout);
                    }
                }
                g_api.Destroy(ctx);
            } else {
                printf("  会话未就绪，跳过\n");
                g_api.Destroy(ctx);
            }
        }
    }

    FreeLibrary(mod);
    printf("完成\n");
    return 0;
}
