// nr_probe_output.cpp —— 「NR 到底有没有改画面」的决定性离线探针
//
// 要回答三个问题（不再靠猜）：
//   1) 网络的修改结果到底写进 fi.color（我们喂进去的那张），还是写进 job.private_output？
//      —— 如果 color 回读与输入逐字节相同，说明我们一直在搬运原图（这就是"画面没有改动"）。
//   2) 网络自身产生的 edit 有多大（按 0..1 单位统计 mean / max / 变化像素占比）。
//      —— 这决定"只把修正量叠到 4K 底图"这条路线能有多少可见幅度。
//   3) 把 depth / velocity 传成全 0，会不会让网络退化成恒等（v6 就是这么传的）。
//
// 用法：nr_probe_output.exe [W] [H] [frames]
// 输出：控制台统计 + BMP（input / color 回读 / private_output 回读）

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "LmxxfNrApi.h"

using Microsoft::WRL::ComPtr;

// 运行时实际是 160 字节的表（比上游头文件多两个入口）
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

// 144 字节版（v6 代理用的布局）：base(104) + depth + velocity + 5 个控制量
struct FrameInfo144 {
    LmxxfNrFrameInfo base;
    uint64_t depth;
    uint64_t velocity;
    float ctlTone;
    float ctlStructure;
    float ctlSkin;
    float ctlOther;
    float ctlStyle;
    uint32_t ctlPad;
};
static_assert(sizeof(LmxxfNrFrameInfo) == 104, "base frame info must be 104 bytes");
static_assert(sizeof(FrameInfo144) == 144, "extended frame info must be 144 bytes");

static const wchar_t* kGameDir = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2";
static const wchar_t* kAssets = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";
static const char* kOutDir = "E:\\AI\\tmp\\";   // 必须纯 ASCII：fopen 走 ANSI，中文路径会失败

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

// ---------------- 图像工具 ----------------
struct Img {
    UINT w = 0, h = 0;
    std::vector<unsigned char> px;   // 紧凑 RGBA8，行距 = w*4
};

// 造一张有结构、有细节、有噪声的测试图
static void MakeTestImage(Img& im, UINT w, UINT h, int frameNo) {
    im.w = w; im.h = h;
    im.px.assign((size_t)w * h * 4, 0);
    unsigned int seed = 12345u;
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            float fx = (float)x / (float)w, fy = (float)y / (float)h;
            float r = 0.18f + 0.55f * fx;
            float g = 0.22f + 0.45f * fy;
            float b = 0.30f + 0.35f * (1.0f - fx) * fy;
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
            unsigned char* p = &im.px[((size_t)y * w + x) * 4];
            p[0] = (unsigned char)(r * 255.0f + 0.5f);
            p[1] = (unsigned char)(g * 255.0f + 0.5f);
            p[2] = (unsigned char)(b * 255.0f + 0.5f);
            p[3] = 255;
        }
    }
}

static bool SaveBmp(const char* path, const unsigned char* rgba, UINT w, UINT h) {
    DWORD rowBytes = w * 4;
    DWORD dataBytes = rowBytes * h;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + dataBytes;
    ih.biSize = sizeof(ih);
    ih.biWidth = (LONG)w;
    ih.biHeight = -(LONG)h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = dataBytes;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<unsigned char> row((size_t)rowBytes);
    for (UINT y = 0; y < h; ++y) {
        const unsigned char* s = rgba + (size_t)y * rowBytes;
        for (UINT x = 0; x < w; ++x) {
            row[x * 4 + 0] = s[x * 4 + 2];
            row[x * 4 + 1] = s[x * 4 + 1];
            row[x * 4 + 2] = s[x * 4 + 0];
            row[x * 4 + 3] = 255;
        }
        fwrite(row.data(), rowBytes, 1, f);
    }
    fclose(f);
    return true;
}

struct DiffStat {
    double mean = 0;      // 0..1 单位
    double maxd = 0;
    double pctFar = 0;    // |diff| >= 2/255 的像素占比
    double meanIn = 0, meanOut = 0;
    size_t changedBytes = 0;
};

static DiffStat Compare(const Img& a, const Img& b) {
    DiffStat s{};
    if (a.px.size() != b.px.size() || a.px.empty()) return s;
    double sum = 0, sumA = 0, sumB = 0;
    size_t nFar = 0, px = 0;   // 注意：far 是 windef.h 的宏，不能当标识符
    for (size_t i = 0; i + 3 < a.px.size(); i += 4) {
        double chMax = 0;
        for (int c = 0; c < 3; ++c) {
            int d = (int)a.px[i + c] - (int)b.px[i + c];
            if (d < 0) d = -d;
            sum += (double)d / 255.0;
            if ((double)d / 255.0 > s.maxd) s.maxd = (double)d / 255.0;
            if (d > chMax) chMax = (double)d;
            if (b.px[i + c] != a.px[i + c]) s.changedBytes++;
            sumA += a.px[i + c];
            sumB += b.px[i + c];
        }
        if (chMax >= 2.0) nFar++;
        px++;
    }
    s.mean = sum / (double)(px * 3);
    s.pctFar = 100.0 * (double)nFar / (double)px;
    s.meanIn = sumA / (double)(px * 3);
    s.meanOut = sumB / (double)(px * 3);
    return s;
}

// ---------------- D3D12 侧 ----------------
struct Readback {
    ComPtr<ID3D12Resource> res;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    UINT rowPitch = 0;
    UINT rows = 0;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

static bool MakeReadback(ID3D12Device* dev, const D3D12_RESOURCE_DESC& td, Readback& rb) {
    D3D12_HEAP_PROPERTIES rh{};
    rh.Type = D3D12_HEAP_TYPE_READBACK;
    rh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    rh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    rh.CreationNodeMask = 1;
    rh.VisibleNodeMask = 1;
    UINT64 rowSize64 = 0;
    UINT64 total = 0;
    dev->GetCopyableFootprints(&td, 0, 1, 0, &rb.fp, &rb.rows, &rowSize64, &total);
    rb.bytes = total;
    rb.rowPitch = rb.fp.Footprint.RowPitch;
    rb.w = (UINT)td.Width;
    rb.h = td.Height;
    rb.fmt = td.Format;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb.res)));
}

// 把回读缓冲整理成紧凑 RGBA8
static bool FetchRgba8(Readback& rb, Img& out) {
    out.w = rb.w; out.h = rb.h;
    out.px.assign((size_t)rb.w * rb.h * 4, 0);
    if (!rb.res) return false;
    D3D12_RANGE rr{ 0, (SIZE_T)rb.bytes };
    void* m = nullptr;
    if (FAILED(rb.res->Map(0, &rr, &m)) || !m) return false;
    bool bgra = (rb.fmt == DXGI_FORMAT_B8G8R8A8_UNORM || rb.fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    for (UINT y = 0; y < rb.h; ++y) {
        const unsigned char* s = (const unsigned char*)m + (size_t)y * rb.rowPitch;
        unsigned char* d = &out.px[(size_t)y * rb.w * 4];
        if (bgra) {
            for (UINT x = 0; x < rb.w; ++x) {
                d[x * 4 + 0] = s[x * 4 + 2];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 0];
                d[x * 4 + 3] = s[x * 4 + 3];
            }
        } else {
            memcpy(d, s, (size_t)rb.w * 4);
        }
    }
    D3D12_RANGE nw{ 0, 0 };
    rb.res->Unmap(0, &nw);
    return true;
}

struct Ctx {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fv = 0;
};

static void SubmitWait(Ctx& c) {
    c.list->Close();
    ID3D12CommandList* l[] = { c.list.Get() };
    c.queue->ExecuteCommandLists(1, l);
    c.queue->Signal(c.fence.Get(), ++c.fv);
    while (c.fence->GetCompletedValue() < c.fv) Sleep(0);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // 崩溃时也要看到已经打印的进度
    printf("[step] main 进入\n");

    UINT W = 1920, H = 1080;
    int frames = 12;
    if (argc > 1) W = (UINT)atoi(argv[1]);
    if (argc > 2) H = (UINT)atoi(argv[2]);
    if (argc > 3) frames = atoi(argv[3]);
    printf("=== NR 输出落点探针 %ux%u, frames=%d ===\n", W, H, frames);
    bool skipRt = (argc > 4 && strcmp(argv[4], "nort") == 0);   // 对照实验：不加载运行时只建工厂

    printf("[step] 载入运行时...\n");
    HMODULE mod = skipRt ? nullptr : LoadLibraryW((std::wstring(kGameDir) + L"\\LmxxfNrRuntime.dll").c_str());
    if (!skipRt && !mod) { printf("runtime load failed %lu\n", GetLastError()); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)(mod ? GetProcAddress(mod, "LmxxfNrGetApi") : nullptr);
    ApiTable api{};
    api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!skipRt && (!getApi || getApi(1, &api) != 0)) { printf("GetApi failed\n"); return 1; }

    printf("[step] 设备创建...\n");
    ComPtr<IDXGIFactory6> fac;
    HRESULT hrF = CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), (void**)fac.GetAddressOf());
    printf("[step] factory hr=0x%08X fac=%p\n", (unsigned)hrF, (void*)fac.Get());
    if (FAILED(hrF) || !fac) { printf("no dxgi factory\n"); return 1; }
    ComPtr<IDXGIAdapter1> ad;
    HRESULT hrA = fac->EnumAdapters1(1, &ad);
    printf("[step] EnumAdapters1(1) hr=0x%08X ad=%p\n", (unsigned)hrA, (void*)ad.Get());
    if (hrA != S_OK || !ad) { printf("no adapter 1\n"); return 1; }
    DXGI_ADAPTER_DESC1 adx{};
    ad->GetDesc1(&adx);
    if (skipRt) { printf("[对照] 工厂+适配器都正常，跳过运行时加载后退出\n"); return 0; }
    printf("adapter 1 = %ls\n", adx.Description);

    Ctx c;
    HRESULT hrD = D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)c.dev.GetAddressOf());
    printf("[step] D3D12CreateDevice hr=0x%08X dev=%p\n", (unsigned)hrD, (void*)c.dev.Get());
    if (FAILED(hrD) || !c.dev) { printf("device failed\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    c.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.queue));
    c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc));
    c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc.Get(), nullptr, IID_PPV_ARGS(&c.list));
    c.list->Close();
    c.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence));

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1;
    dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh;
    uh.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    printf("[step] color / 缓冲...\n");
    ComPtr<ID3D12Resource> color;
    if (FAILED(c.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color)))) { printf("color failed\n"); return 1; }
    D3D12_RESOURCE_DESC tdPlain = td;
    tdPlain.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rows = 0; UINT64 rowSize = 0; UINT64 texBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    c.dev->GetCopyableFootprints(&tdPlain, 0, 1, 0, &fp, &rows, &rowSize, &texBytes);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = texBytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> up;
    c.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up));

    // depth / velocity（测"传 0 会不会变恒等"用）
    D3D12_RESOURCE_DESC dd = tdPlain;
    dd.Format = DXGI_FORMAT_R32_FLOAT;
    ComPtr<ID3D12Resource> depthZ, velZ, depthV, velV;
    c.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&depthZ));
    D3D12_RESOURCE_DESC vd = tdPlain;
    vd.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
    c.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &vd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&velZ));
    c.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&depthV));
    c.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &vd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&velV));

    Readback rbColor;
    Readback rbPo;
    MakeReadback(c.dev.Get(), tdPlain, rbColor);

    Img base;
    struct Fixture {
        const char* name;
        uint32_t fiSize;
        int dvdMode;   // 0=不传, 1=全 0, 2=合理值
        uint32_t debugView;
    };
    Fixture fx[] = {
        { "fi=104 无 depth/MV",              104, 0, 0 },
        { "fi=120 空槽（v5 用的）",          120, 0, 0 },
        { "fi=144 depth/MV 全 0（v6 用的）", 144, 1, 0 },
        { "fi=144 depth/MV 合理值",          144, 2, 0 },
        { "fi=104 debug_view=3 (diff20x)",   104, 0, 3 },
    };

    for (const Fixture& f : fx) {
        printf("\n---------------- %s ----------------\n", f.name);

        void* ctx = nullptr;
        LmxxfNrCreateInfo ci{};
        ci.struct_size = (uint32_t)sizeof(ci);
        ci.device = c.dev.Get();
        ci.queue = c.queue.Get();
        ci.assets_directory = kAssets;
        int32_t r = api.Create(&ci, &ctx);
        if (r != 0) { printf("  Create -> %s\n", St(r)); continue; }
        r = api.PrepareSession(ctx);
        if (r != 0) {
            char eb[512] = { 0 };
            if (api.GetLastError) api.GetLastError(eb, 512);
            printf("  PrepareSession -> %s err=%s\n", St(r), eb);
            api.Destroy(ctx);
            continue;
        }

        if (f.dvdMode == 2) {
            std::vector<float> dz((size_t)W * H);
            for (UINT y = 0; y < H; ++y)
                for (UINT x = 0; x < W; ++x)
                    dz[(size_t)y * W + x] = 0.1f + 0.8f * (float)y / (float)H;
            D3D12_RESOURCE_DESC dp = dd;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpd{};
            UINT rr2 = 0; UINT64 rs2 = 0, t2 = 0;
            c.dev->GetCopyableFootprints(&dp, 0, 1, 0, &fpd, &rr2, &rs2, &t2);
            D3D12_RESOURCE_DESC b2 = bd; b2.Width = t2;
            ComPtr<ID3D12Resource> u2;
            c.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &b2, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&u2));
            void* m = nullptr;
            D3D12_RANGE nr{ 0, 0 };
            u2->Map(0, &nr, &m);
            for (UINT y = 0; y < H; ++y)
                memcpy((unsigned char*)m + (size_t)y * fpd.Footprint.RowPitch, &dz[(size_t)y * W], (size_t)W * 4);
            D3D12_RANGE wr{ 0, (SIZE_T)t2 };
            u2->Unmap(0, &wr);
            ID3D12Resource* dsts[2] = { depthV.Get(), depthZ.Get() };
            for (ID3D12Resource* dst : dsts) {
                c.alloc->Reset(); c.list->Reset(c.alloc.Get(), nullptr);
                D3D12_TEXTURE_COPY_LOCATION ddst{};
                ddst.pResource = dst; ddst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; ddst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION ssrc{};
                ssrc.pResource = u2.Get(); ssrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; ssrc.PlacedFootprint = fpd;
                c.list->CopyTextureRegion(&ddst, 0, 0, 0, &ssrc, nullptr);
                SubmitWait(c);
            }
        }

        Img lastColor, lastPrivate;
        bool gotPrivate = false;
        for (int fr = 0; fr < frames; ++fr) {
            MakeTestImage(base, W, H, fr);

            {
                D3D12_RANGE nr{ 0, 0 };
                void* m = nullptr;
                up->Map(0, &nr, &m);
                for (UINT y = 0; y < H; ++y)
                    memcpy((unsigned char*)m + (size_t)y * fp.Footprint.RowPitch, &base.px[(size_t)y * W * 4], (size_t)W * 4);
                D3D12_RANGE wr{ 0, (SIZE_T)texBytes };
                up->Unmap(0, &wr);
                c.alloc->Reset(); c.list->Reset(c.alloc.Get(), nullptr);
                D3D12_TEXTURE_COPY_LOCATION ddst{};
                ddst.pResource = color.Get(); ddst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; ddst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION ssrc{};
                ssrc.pResource = up.Get(); ssrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; ssrc.PlacedFootprint = fp;
                c.list->CopyTextureRegion(&ddst, 0, 0, 0, &ssrc, nullptr);
                SubmitWait(c);
            }

            LmxxfNrJob job{};
            job.struct_size = (uint32_t)sizeof(job);
            FrameInfo144 f144{};
            LmxxfNrFrameInfo& fi = f144.base;
            fi.struct_size = f.fiSize;
            fi.session_id = 1;
            fi.frame_id = (uint64_t)fr + 1;
            fi.list_generation = 1;
            fi.command_list = c.list.Get();
            fi.color_width = W;
            fi.color_height = H;
            fi.color = color.Get();
            fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
            fi.flags = 0;
            fi.transfer_strength = 1.0f;
            fi.color_strength = 1.0f;
            fi.debug_view = f.debugView;
            fi.model_scale = 1.0f;
            fi.exposure = nullptr;
            fi.exposure_state = 0;
            fi.pre_exposure = 1.0f;
            fi.exposure_scale = 1.0f;
            if (f.fiSize == 144) {
                f144.depth = (uint64_t)(f.dvdMode == 2 ? depthV.Get() : depthZ.Get());
                f144.velocity = (uint64_t)(f.dvdMode == 2 ? velV.Get() : velZ.Get());
                f144.ctlTone = 1.0f;
                f144.ctlStructure = 1.0f;
                f144.ctlSkin = 1.0f;
                f144.ctlOther = 1.0f;
                f144.ctlStyle = 0.0f;
            }

            r = api.PrepareFrame(ctx, &fi, &job);
            if (r != 0) {
                char eb[512] = { 0 };
                if (api.GetLastError) api.GetLastError(eb, 512);
                printf("  f%d PrepareFrame -> %s err=%s\n", fr, St(r), eb);
                break;
            }
            if (fr == 0 && job.private_output) {
                ID3D12Resource* po = (ID3D12Resource*)job.private_output;
                D3D12_RESOURCE_DESC pd2 = po->GetDesc();
                printf("  private_output: %llux%u fmt=%d flags=0x%llx\n",
                       (unsigned long long)pd2.Width, pd2.Height, (int)pd2.Format,
                       (unsigned long long)pd2.Flags);
            }

            c.alloc->Reset(); c.list->Reset(c.alloc.Get(), nullptr);
            r = api.RecordInputs(ctx, job.handle, c.list.Get());
            if (r != 0) { printf("  f%d RecordInputs -> %s\n", fr, St(r)); break; }
            SubmitWait(c);
            r = api.ExecuteAfterProducer(ctx, job.handle, c.queue.Get());
            if (r != 0) { printf("  f%d ExecuteAfterProducer -> %s\n", fr, St(r)); break; }
            c.alloc->Reset(); c.list->Reset(c.alloc.Get(), nullptr);
            r = api.RecordOutputs(ctx, job.handle, c.list.Get());
            if (r != 0) { printf("  f%d RecordOutputs -> %s\n", fr, St(r)); break; }
            SubmitWait(c);
            api.Retire(ctx, job.handle);

            // 1) 回读 color
            {
                c.alloc->Reset(); c.list->Reset(c.alloc.Get(), nullptr);
                D3D12_TEXTURE_COPY_LOCATION rd{};
                rd.pResource = rbColor.res.Get(); rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; rd.PlacedFootprint = rbColor.fp;
                D3D12_TEXTURE_COPY_LOCATION rs{};
                rs.pResource = color.Get(); rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rs.SubresourceIndex = 0;
                c.list->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
                SubmitWait(c);
                FetchRgba8(rbColor, lastColor);
            }

            // 2) 回读 private_output
            if (job.private_output) {
                ID3D12Resource* po = (ID3D12Resource*)job.private_output;
                D3D12_RESOURCE_DESC pd2 = po->GetDesc();
                bool usable = ((UINT)pd2.Width == W && pd2.Height == H &&
                               (pd2.Format == DXGI_FORMAT_R8G8B8A8_UNORM || pd2.Format == DXGI_FORMAT_B8G8R8A8_UNORM));
                if (usable) {
                    if (!rbPo.res) {
                        D3D12_RESOURCE_DESC pt = tdPlain;
                        pt.Format = pd2.Format;
                        MakeReadback(c.dev.Get(), pt, rbPo);
                    }
                    c.alloc->Reset(); c.list->Reset(c.alloc.Get(), nullptr);
                    D3D12_RESOURCE_BARRIER b1{};
                    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b1.Transition.pResource = po;
                    b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    c.list->ResourceBarrier(1, &b1);
                    D3D12_TEXTURE_COPY_LOCATION rd{};
                    rd.pResource = rbPo.res.Get(); rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; rd.PlacedFootprint = rbPo.fp;
                    D3D12_TEXTURE_COPY_LOCATION rs{};
                    rs.pResource = po; rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rs.SubresourceIndex = 0;
                    c.list->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
                    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                    c.list->ResourceBarrier(1, &b1);
                    SubmitWait(c);
                    gotPrivate = FetchRgba8(rbPo, lastPrivate);
                } else if (fr == 0) {
                    printf("  private_output 几何/格式不匹配（%llux%u fmt=%d），跳过回读\n",
                           (unsigned long long)pd2.Width, pd2.Height, (int)pd2.Format);
                }
            }

            if (fr == frames - 1) {
                DiffStat sc = Compare(base, lastColor);
                printf("  [color 回读] 均值 入%.1f 出%.1f | edit 平均 %.4f 最大 %.4f | 变化像素 %.2f%% | 不同字节 %zu\n",
                       sc.meanIn, sc.meanOut, sc.mean, sc.maxd, sc.pctFar, sc.changedBytes);
                if (gotPrivate) {
                    DiffStat sp = Compare(base, lastPrivate);
                    printf("  [priv 回读 ] 均值 入%.1f 出%.1f | edit 平均 %.4f 最大 %.4f | 变化像素 %.2f%% | 不同字节 %zu\n",
                           sp.meanIn, sp.meanOut, sp.mean, sp.maxd, sp.pctFar, sp.changedBytes);
                    DiffStat scp = Compare(lastColor, lastPrivate);
                    printf("  [color vs priv] edit 平均 %.4f 最大 %.4f | 变化像素 %.2f%%\n",
                           scp.mean, scp.maxd, scp.pctFar);
                    char nm[256];
                    sprintf_s(nm, "%sprobe_priv_fi%u_dvd%d_dv%d.bmp", kOutDir, f.fiSize, f.dvdMode, f.debugView ? 1 : 0);
                    SaveBmp(nm, lastPrivate.px.data(), W, H);
                }
            }
        }

        if (api.GetStatus) {
            char st[1024] = { 0 };
            api.GetStatus(ctx, st, 1024);
            printf("  status: %s\n", st);
        }
        if (api.Drain) api.Drain(ctx);
        api.Destroy(ctx);
        if (!lastColor.px.empty()) {
            char nm[256];
            sprintf_s(nm, "%sprobe_color_fi%u_dvd%d_dv%d.bmp", kOutDir, f.fiSize, f.dvdMode, f.debugView ? 1 : 0);
            SaveBmp(nm, lastColor.px.data(), W, H);
        }
    }

    if (!base.px.empty())
        SaveBmp((std::string(kOutDir) + "probe_input.bmp").c_str(), base.px.data(), W, H);

    FreeLibrary(mod);
    printf("\n完成。BMP 在 build\\ 下（probe_input / probe_color_* / probe_priv_*）\n");
    return 0;
}
