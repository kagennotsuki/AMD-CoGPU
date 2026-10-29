// nr_from_capture.cpp —— 用游戏里抓到的真实一帧数据，离线跑 lmxxf，出「NR 前 / NR 后」对比图
//
// 输入（游戏目录，由 xess_proxy 抓帧模式产生）：nr_cap_out.bin / nr_cap_color.bin /
// nr_cap_depth.bin / nr_cap_mv.bin / nr_cap_params.txt
//
// 流程（与代理里的 v7 链路一致）：
//   ① pOutput 4K → 2x2 盒式下采样到 1080p → 作为网络输入
//   ② 运动矢量 2227x1253 SNORM → 1080p；深度 2227x1253 R32 → 1080p
//   ③ 三种喂法各跑一帧：只有颜色 / 颜色+MV / 颜色+MV+深度
//   ④ 读 private_output，算 edit = 网络答案 − 喂进去的输入
//   ⑤ 把 edit 双线性抬回 4K，叠到「原始 4K 画面」上 → 输出对比图
//
// 输出（E:\AI\跨卡神经渲染原型\capture\）：nr_before.bmp / nr_after*.bmp / nr_diff*.bmp

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

struct FrameInfo160 {
    uint32_t struct_size; uint32_t pad0;
    uint64_t session_id; uint64_t frame_id; uint64_t list_generation;
    void* command_list;
    uint32_t color_width; uint32_t color_height;
    void* color;
    uint32_t color_state; uint32_t flags;
    float transfer_strength; float color_strength;
    uint32_t debug_view; float model_scale;
    void* exposure; uint32_t exposure_state;
    float pre_exposure; float exposure_scale;
    void* depth;
    void* velocity;
    float ctlTone, ctlStructure, ctlSkin, ctlOther, ctlStyle;
    uint32_t ctlPad;
    void* historyMask;
    void* reserved152;
};
static_assert(sizeof(FrameInfo160) == 160, "");

struct Job { uint32_t struct_size; void* handle; void* private_output; };
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; void* reserved0; };

static const wchar_t* kGameDir = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2";
static const wchar_t* kAssets = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";
static const char* kOutDir = "E:\\AI\\tmp\\nr_capture\\";

// 抓帧几何（与 nr_cap_params.txt 一致）
static const UINT kCapW = 2227, kCapH = 1253;      // color/depth/mv
static const UINT kOutW = 3840, kOutH = 2160;      // pOutput
static const UINT kNetW = 1920, kNetH = 1080;      // 网络输入

struct Img8 { UINT w = 0, h = 0; std::vector<unsigned char> px; };

static bool LoadFile(const std::string& path, std::vector<unsigned char>& out, size_t expect) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) { printf("打不开 %s\n", path.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)sz);
    size_t rd = fread(out.data(), 1, out.size(), f);
    fclose(f);
    printf("  读入 %s : %ld 字节（期望 %zu）%s\n", path.c_str(), sz, expect, (size_t)sz == expect ? "" : "  <-- 大小不符");
    return rd == out.size();
}

static bool SaveBmp(const char* path, const unsigned char* rgba, UINT w, UINT h) {
    DWORD rowBytes = w * 4;
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + rowBytes * h;
    ih.biSize = sizeof(ih); ih.biWidth = (LONG)w; ih.biHeight = -(LONG)h;
    ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB; ih.biSizeImage = rowBytes * h;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
    std::vector<unsigned char> row(rowBytes);
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
    printf("  写出 %s\n", path);
    return true;
}

// R10G10B10A2 → RGBA8
static void UnpackR10G10B10A2(const std::vector<unsigned char>& src, Img8& dst) {
    dst.w = kOutW; dst.h = kOutH;
    dst.px.resize((size_t)kOutW * kOutH * 4);
    const unsigned int* s = (const unsigned int*)src.data();
    for (size_t i = 0, n = (size_t)kOutW * kOutH; i < n; ++i) {
        unsigned int v = s[i];
        dst.px[i * 4 + 0] = (unsigned char)((((v) & 0x3FFu) * 255u + 511u) / 1023u);
        dst.px[i * 4 + 1] = (unsigned char)((((v >> 10) & 0x3FFu) * 255u + 511u) / 1023u);
        dst.px[i * 4 + 2] = (unsigned char)((((v >> 20) & 0x3FFu) * 255u + 511u) / 1023u);
        dst.px[i * 4 + 3] = 255;
    }
}

// 2x2 盒式下采样（4K → 1080p）
static void Downsample2x(const Img8& src, Img8& dst) {
    dst.w = src.w / 2; dst.h = src.h / 2;
    dst.px.assign((size_t)dst.w * dst.h * 4, 0);
    for (UINT y = 0; y < dst.h; ++y)
        for (UINT x = 0; x < dst.w; ++x)
            for (int c = 0; c < 4; ++c) {
                unsigned s = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        s += src.px[((size_t)(y * 2 + dy) * src.w + (x * 2 + dx)) * 4 + c];
                dst.px[((size_t)y * dst.w + x) * 4 + c] = (unsigned char)((s + 2) / 4);
            }
}

// 任意尺寸双线性下/上采样（RGBA8）
static void ResampleBilinear(const Img8& src, Img8& dst, UINT w, UINT h) {
    dst.w = w; dst.h = h;
    dst.px.assign((size_t)w * h * 4, 0);
    for (UINT y = 0; y < h; ++y) {
        float fy = ((float)y + 0.5f) * (float)src.h / (float)h - 0.5f;
        int y0 = (int)floorf(fy); float ty = fy - (float)y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0; if (y1 >= (int)src.h) y1 = src.h - 1;
        if (y1 < 0) y1 = 0;
        for (UINT x = 0; x < w; ++x) {
            float fx = ((float)x + 0.5f) * (float)src.w / (float)w - 0.5f;
            int x0 = (int)floorf(fx); float tx = fx - (float)x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0; if (x1 >= (int)src.w) x1 = src.w - 1;
            if (x1 < 0) x1 = 0;
            for (int c = 0; c < 4; ++c) {
                float a = src.px[((size_t)y0 * src.w + x0) * 4 + c];
                float b = src.px[((size_t)y0 * src.w + x1) * 4 + c];
                float cc = src.px[((size_t)y1 * src.w + x0) * 4 + c];
                float d = src.px[((size_t)y1 * src.w + x1) * 4 + c];
                float top = a + (b - a) * tx, bot = cc + (d - cc) * tx;
                dst.px[((size_t)y * w + x) * 4 + c] = (unsigned char)(top + (bot - top) * ty + 0.5f);
            }
        }
    }
}

// 浮点通道用双线性（深度）
static void ResampleFloat1(const std::vector<float>& src, UINT sw, UINT sh, UINT w, UINT h, std::vector<float>& dst) {
    dst.assign((size_t)w * h, 0.0f);
    for (UINT y = 0; y < h; ++y) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)h - 0.5f;
        int y0 = (int)floorf(fy); float ty = fy - (float)y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0; if (y1 >= (int)sh) y1 = sh - 1; if (y1 < 0) y1 = 0;
        for (UINT x = 0; x < w; ++x) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)w - 0.5f;
            int x0 = (int)floorf(fx); float tx = fx - (float)x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0; if (x1 >= (int)sw) x1 = sw - 1; if (x1 < 0) x1 = 0;
            float a = src[(size_t)y0 * sw + x0], b = src[(size_t)y0 * sw + x1];
            float c = src[(size_t)y1 * sw + x0], d = src[(size_t)y1 * sw + x1];
            float top = a + (b - a) * tx, bot = c + (d - c) * tx;
            dst[(size_t)y * w + x] = top + (bot - top) * ty;
        }
    }
}

struct Dev {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fe;
    UINT64 fv = 0;
};

static void SubmitWait(Dev& d) {
    d.cl->Close();
    ID3D12CommandList* l[] = { d.cl.Get() };
    d.q->ExecuteCommandLists(1, l);
    d.q->Signal(d.fe.Get(), ++d.fv);
    while (d.fe->GetCompletedValue() < d.fv) Sleep(0);
}

struct Tex {
    ComPtr<ID3D12Resource> res;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    UINT rowPitch = 0;
    UINT w = 0, h = 0;
};

static bool MakeTex(Dev& d, const D3D12_HEAP_PROPERTIES& dh, UINT w, UINT h, DXGI_FORMAT fmt, Tex& t) {
    t.w = w; t.h = h;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(d.dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t.res)))) return false;
    D3D12_RESOURCE_DESC plain = td; plain.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rows = 0; UINT64 rowSize = 0;
    d.dev->GetCopyableFootprints(&plain, 0, 1, 0, &t.fp, &rows, &rowSize, &t.bytes);
    t.rowPitch = t.fp.Footprint.RowPitch;
    return true;
}

// 把紧凑的源数据上传进纹理
static void UploadTex(Dev& d, const D3D12_HEAP_PROPERTIES& uh, Tex& t, const unsigned char* src, UINT tightRow) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = t.bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> up;
    d.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up));
    void* m = nullptr;
    D3D12_RANGE nr{ 0, 0 };
    up->Map(0, &nr, &m);
    for (UINT y = 0; y < t.h; ++y)
        memcpy((unsigned char*)m + (size_t)y * t.rowPitch, src + (size_t)y * tightRow, tightRow);
    D3D12_RANGE wr{ 0, (SIZE_T)t.bytes };
    up->Unmap(0, &wr);
    d.al->Reset(); d.cl->Reset(d.al.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dd{};
    dd.pResource = t.res.Get(); dd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dd.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION ss{};
    ss.pResource = up.Get(); ss.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; ss.PlacedFootprint = t.fp;
    d.cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
    SubmitWait(d);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== 用游戏内真实抓帧数据离线跑 NR ===\n");
    CreateDirectoryA("E:\\AI\\tmp\\nr_capture", nullptr);

    std::string gdir = "E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\";
    std::vector<unsigned char> rawOut, rawColor, rawDepth, rawMv;
    if (!LoadFile(gdir + "nr_cap_out.bin", rawOut, (size_t)kOutW * kOutH * 4)) return 1;
    LoadFile(gdir + "nr_cap_color.bin", rawColor, (size_t)kCapW * kCapH * 4);
    LoadFile(gdir + "nr_cap_depth.bin", rawDepth, (size_t)kCapW * kCapH * 4);
    LoadFile(gdir + "nr_cap_mv.bin", rawMv, (size_t)kCapW * kCapH * 8);

    // ① 原始 4K 画面（NR 前）
    Img8 before;
    UnpackR10G10B10A2(rawOut, before);
    SaveBmp((std::string(kOutDir) + "nr_before.bmp").c_str(), before.px.data(), before.w, before.h);

    // ② 网络输入：4K → 1080p
    Img8 net1080;
    if (before.w == kNetW * 2 && before.h == kNetH * 2) Downsample2x(before, net1080);
    else ResampleBilinear(before, net1080, kNetW, kNetH);
    SaveBmp((std::string(kOutDir) + "nr_fed1080.bmp").c_str(), net1080.px.data(), net1080.w, net1080.h);

    // ③ 是否抓到 mv / depth
    bool hasMv = rawMv.size() >= (size_t)kCapW * kCapH * 8;
    bool hasDepth = rawDepth.size() >= (size_t)kCapW * kCapH * 4;
    std::vector<unsigned char> mv1080;
    std::vector<float> dep1080;
    if (hasMv) {
        // SNORM RGBA16 → 双线性降到 1080p（只处理 xy 两通道，z/w 清零）
        mv1080.assign((size_t)kNetW * kNetH * 8, 0);
        for (UINT y = 0; y < kNetH; ++y) {
            float fy = ((float)y + 0.5f) * (float)kCapH / (float)kNetH - 0.5f;
            int y0 = (int)floorf(fy); float ty = fy - (float)y0; int y1 = y0 + 1;
            if (y0 < 0) y0 = 0; if (y1 >= (int)kCapH) y1 = kCapH - 1; if (y1 < 0) y1 = 0;
            for (UINT x = 0; x < kNetW; ++x) {
                float fx = ((float)x + 0.5f) * (float)kCapW / (float)kNetW - 0.5f;
                int x0 = (int)floorf(fx); float tx = fx - (float)x0; int x1 = x0 + 1;
                if (x0 < 0) x0 = 0; if (x1 >= (int)kCapW) x1 = kCapW - 1; if (x1 < 0) x1 = 0;
                short* dst = (short*)&mv1080[((size_t)y * kNetW + x) * 8];
                for (int c = 0; c < 2; ++c) {
                    float a = ((const short*)rawMv.data())[((size_t)y0 * kCapW + x0) * 4 + c];
                    float b = ((const short*)rawMv.data())[((size_t)y0 * kCapW + x1) * 4 + c];
                    float cc = ((const short*)rawMv.data())[((size_t)y1 * kCapW + x0) * 4 + c];
                    float d = ((const short*)rawMv.data())[((size_t)y1 * kCapW + x1) * 4 + c];
                    float top = a + (b - a) * tx, bot = cc + (d - cc) * tx;
                    float v = top + (bot - top) * ty;
                    if (v > 32767.0f) v = 32767.0f;
                    if (v < -32768.0f) v = -32768.0f;
                    dst[c] = (short)lrintf(v);
                }
                dst[2] = 0; dst[3] = 0;
            }
        }
    }
    if (hasDepth) {
        std::vector<float> dep((size_t)kCapW * kCapH);
        memcpy(dep.data(), rawDepth.data(), dep.size() * 4);
        ResampleFloat1(dep, kCapW, kCapH, kNetW, kNetH, dep1080);
    }

    // ④ 运行时
    HMODULE mod = LoadLibraryW((std::wstring(kGameDir) + L"\\LmxxfNrRuntime.dll").c_str());
    if (!mod) { printf("运行时加载失败\n"); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    ApiTable api{};
    api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!getApi || getApi(1, &api) != 0) { printf("GetApi 失败\n"); return 1; }

    ComPtr<IDXGIFactory6> fac;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));
    ComPtr<IDXGIAdapter1> ad;
    if (!fac || fac->EnumAdapters1(1, &ad) != S_OK) { printf("没有适配器 1\n"); return 1; }
    DXGI_ADAPTER_DESC1 adx{};
    ad->GetDesc1(&adx);
    printf("适配器 = %ls\n", adx.Description);

    Dev d;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.dev)))) { printf("设备失败\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    d.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&d.q));
    d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d.al));
    d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.al.Get(), nullptr, IID_PPV_ARGS(&d.cl));
    d.cl->Close();
    d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fe));

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    Tex texColor, texMv, texDep;
    if (!MakeTex(d, dh, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, texColor)) { printf("color 纹理失败\n"); return 1; }
    MakeTex(d, dh, kNetW, kNetH, DXGI_FORMAT_R16G16B16A16_SNORM, texMv);
    MakeTex(d, dh, kNetW, kNetH, DXGI_FORMAT_R32_FLOAT, texDep);
    UploadTex(d, uh, texColor, net1080.px.data(), kNetW * 4);
    if (hasMv) UploadTex(d, uh, texMv, mv1080.data(), kNetW * 8);
    if (hasDepth) UploadTex(d, uh, texDep, (const unsigned char*)dep1080.data(), kNetW * 4);

    void* ctx = nullptr;
    LmxxfNrCreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(ci);
    ci.device = d.dev.Get(); ci.queue = d.q.Get(); ci.assets_directory = kAssets;
    if (api.Create(&ci, &ctx) != 0) { printf("Create 失败\n"); return 1; }
    if (api.PrepareSession(ctx) != 0) { printf("PrepareSession 失败\n"); return 1; }
    printf("会话就绪\n");

    // ⑤ 三种喂法各跑一帧
    struct Variant { const char* name; bool mv; bool dep; bool norm; };
    Variant vs[] = {
        { "只有颜色", false, false, false },
        { "颜色 + 运动矢量", true, false, false },
        { "颜色 + 运动矢量 + 深度", true, true, false },
        { "颜色(曝光归一) + 运动矢量 + 深度", true, true, true },
    };
    // 曝光归一：运行时会把输入均值拉到 0.5 中灰，我们先自己拉好，输出再除回来，
    // 这样 edit 里就不会混进整体亮度偏移。
    double fedMean = 0;
    {
        double sum = 0; size_t cnt = 0;
        for (size_t i = 0; i < net1080.px.size(); ++i) {
            if ((i & 3) == 3) continue;
            sum += net1080.px[i];
            cnt++;
        }
        fedMean = cnt ? sum / (double)cnt : 0.0;
    }
    double normGain = 1.0;
    {
        double m = fedMean / 255.0;
        if (m > 0.02) normGain = 0.5 / m;
        if (normGain < 0.5) normGain = 0.5;
        if (normGain > 6.0) normGain = 6.0;
    }
    printf("送入网络的平均亮度 = %.1f/255 (%.3f)，曝光归一增益 = %.3f\n", fedMean, fedMean / 255.0, normGain);
    Img8 fedNorm = net1080;
    for (size_t i = 0; i < fedNorm.px.size(); ++i) {
        if ((i & 3) == 3) continue;
        float v = (float)net1080.px[i] * (float)normGain;
        if (v > 255.0f) v = 255.0f;
        fedNorm.px[i] = (unsigned char)(v + 0.5f);
    }
    SaveBmp((std::string(kOutDir) + "nr_fed1080_norm.bmp").c_str(), fedNorm.px.data(), fedNorm.w, fedNorm.h);

    std::vector<unsigned char> bestEdit;
    std::string bestName;
    for (const Variant& v : vs) {
        Job job{};
        job.struct_size = (uint32_t)sizeof(Job);
        FrameInfo160 fi{};
        fi.struct_size = (uint32_t)sizeof(FrameInfo160);
        fi.session_id = 1; fi.frame_id = 1; fi.list_generation = 1;
        fi.command_list = d.cl.Get();
        fi.color_width = kNetW; fi.color_height = kNetH;
        fi.color = texColor.res.Get();
        fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        fi.transfer_strength = 1.0f; fi.color_strength = 1.0f;
        fi.model_scale = 1.0f; fi.pre_exposure = 1.0f; fi.exposure_scale = 1.0f;
        fi.depth = v.dep ? texDep.res.Get() : nullptr;
        fi.velocity = v.mv ? texMv.res.Get() : nullptr;
        fi.ctlTone = 1.0f; fi.ctlStructure = 1.0f; fi.ctlSkin = 3.0f; fi.ctlOther = 2.0f; fi.ctlStyle = 0.0f;

        // 归一化变体：上传拉亮后的图
        if (v.norm) UploadTex(d, uh, texColor, fedNorm.px.data(), kNetW * 4);

        int32_t r = api.PrepareFrame(ctx, &fi, &job);
        if (r != 0) {
            char eb[256] = { 0 }; api.GetLastError(eb, 256);
            printf("[%s] PrepareFrame -> %d err=%s\n", v.name, r, eb);
            continue;
        }
        d.al->Reset(); d.cl->Reset(d.al.Get(), nullptr);
        api.RecordInputs(ctx, job.handle, d.cl.Get());
        SubmitWait(d);
        if (api.ExecuteAfterProducer(ctx, job.handle, d.q.Get()) != 0) { printf("[%s] 推理失败\n", v.name); continue; }
        d.al->Reset(); d.cl->Reset(d.al.Get(), nullptr);
        api.RecordOutputs(ctx, job.handle, d.cl.Get());
        SubmitWait(d);

        ID3D12Resource* po = (ID3D12Resource*)job.private_output;
        std::vector<unsigned char> out((size_t)kNetW * kNetH * 4, 0);
        if (po) {
            D3D12_RESOURCE_DESC pd = po->GetDesc();
            D3D12_RESOURCE_DESC plain = pd; plain.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            UINT rows = 0; UINT64 rowSize = 0, total = 0;
            d.dev->GetCopyableFootprints(&plain, 0, 1, 0, &fp, &rows, &rowSize, &total);
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> rb;
            d.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
            d.al->Reset(); d.cl->Reset(d.al.Get(), nullptr);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = po;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            d.cl->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dd{};
            dd.pResource = rb.Get(); dd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dd.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION ss{};
            ss.pResource = po; ss.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; ss.SubresourceIndex = 0;
            d.cl->CopyTextureRegion(&dd, 0, 0, 0, &ss, nullptr);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            d.cl->ResourceBarrier(1, &b);
            SubmitWait(d);
            D3D12_RANGE rr{ 0, (SIZE_T)total };
            void* m = nullptr;
            if (SUCCEEDED(rb->Map(0, &rr, &m)) && m) {
                UINT pitch = fp.Footprint.RowPitch;
                for (UINT y = 0; y < kNetH; ++y)
                    memcpy(&out[(size_t)y * kNetW * 4], (unsigned char*)m + (size_t)y * pitch, (size_t)kNetW * 4);
                D3D12_RANGE nw{ 0, 0 };
                rb->Unmap(0, &nw);
            }
        }
        api.Retire(ctx, job.handle);
        if (v.norm) UploadTex(d, uh, texColor, net1080.px.data(), kNetW * 4);   // 复原

        double sum = 0, mx = 0; size_t cnt = 0, nFar = 0;
        std::vector<unsigned char> edit(out.size(), 128);
        for (size_t i = 0; i < out.size(); ++i) {
            // 归一化变体：答案先除回增益，再和原始输入比，去掉整体曝光偏移
            float ansF = (float)out[i];
            if (v.norm) ansF /= (float)normGain;
            int ans = (int)(ansF + 0.5f);
            if (ans < 0) ans = 0;
            if (ans > 255) ans = 255;
            int dv = ans - (int)net1080.px[i];
            int e = dv + 128;
            if (e < 0) e = 0; if (e > 255) e = 255;
            edit[i] = (unsigned char)e;
            if ((i & 3) == 3) continue;
            int ad = dv < 0 ? -dv : dv;
            double f = (double)ad / 255.0;
            sum += f;
            if (f > mx) mx = f;
            if (ad >= 2) nFar++;
            cnt++;
        }
        printf("[%s] edit 平均 %.4f 最大 %.4f | 变化(>=2/255) %.2f%%\n", v.name,
            cnt ? sum / (double)cnt : 0.0, mx, cnt ? 100.0 * (double)nFar / (double)cnt : 0.0);
        if (bestEdit.empty() || v.mv) { bestEdit = edit; bestName = v.name; }
    }
    api.Destroy(ctx);

    if (bestEdit.empty()) { printf("没有拿到网络输出\n"); return 1; }
    printf("选用喂法：%s\n", bestName.c_str());

    // ⑥ 把 edit 抬回 4K 叠上去（双线性），并出放大版差分图
    Img8 editImg; editImg.w = kNetW; editImg.h = kNetH; editImg.px = bestEdit;
    Img8 edit4k;
    ResampleBilinear(editImg, edit4k, kOutW, kOutH);

    auto applyAndSave = [&](const char* path, float strength) {
        Img8 out = before;
        for (size_t i = 0, n = (size_t)kOutW * kOutH; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                float e = ((float)edit4k.px[i * 4 + c] / 255.0f - 0.5f) * strength;
                float v = (float)before.px[i * 4 + c] + e * 255.0f;
                if (v < 0) v = 0; if (v > 255) v = 255;
                out.px[i * 4 + c] = (unsigned char)(v + 0.5f);
            }
        }
        SaveBmp(path, out.px.data(), out.w, out.h);
    };
    applyAndSave((std::string(kOutDir) + "nr_after_x1.bmp").c_str(), 1.0f);
    applyAndSave((std::string(kOutDir) + "nr_after_x4.bmp").c_str(), 4.0f);

    // 差分可视化（×8，128 为中性灰）
    {
        Img8 dv;
        dv.w = kOutW; dv.h = kOutH;
        dv.px.assign((size_t)kOutW * kOutH * 4, 255);
        for (size_t i = 0, n = (size_t)kOutW * kOutH; i < n; ++i)
            for (int c = 0; c < 3; ++c) {
                float e = ((float)edit4k.px[i * 4 + c] / 255.0f - 0.5f) * 8.0f;
                float v = 128.0f + e * 255.0f;
                if (v < 0) v = 0; if (v > 255) v = 255;
                dv.px[i * 4 + c] = (unsigned char)v;
            }
        SaveBmp((std::string(kOutDir) + "nr_diff_x8.bmp").c_str(), dv.px.data(), dv.w, dv.h);
    }

    if (api.Drain) api.Drain(ctx);
    FreeLibrary(mod);
    printf("\n完成，图片在 %s\n", kOutDir);
    return 0;
}
