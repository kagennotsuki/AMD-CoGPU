// 我自己的离线探针（不参考任何既有工具）
//   目的：用 zip 里的原始抓帧数据，亲自确认网络输出到底落在哪张纹理上。
//   做法：同一帧跑一次 lmxxf，分别回读 fi.color 和 job.private_output，各自与输入对比。
//   输出：BMP 图 + 数值结论。
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

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
    uint64_t session_id; uint64_t frame_id; uint64_t list_generation;
    void* command_list;
    uint32_t color_width; uint32_t color_height;
    void* color;
    uint32_t color_state; uint32_t flags;
    float transfer_strength; float color_strength;
    uint32_t debug_view; float model_scale;
    void* exposure; uint32_t exposure_state;
    float pre_exposure; float exposure_scale;
    void* depth; void* velocity;
    float ctlTone, ctlStructure, ctlSkin, ctlOther, ctlStyle;
    uint32_t ctlPad;
    void* historyMask; void* reserved152;
};

struct Job { uint32_t struct_size; void* handle; void* private_output; };
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; void* reserved0; };

static const UINT kCapW = 2227, kCapH = 1253, kCapPitchC = 8908, kCapPitchMv = 17816;
static const UINT kOutW = 3840, kOutH = 2160, kOutPitch = 15360;
static const UINT kNetW = 1920, kNetH = 1080;
static const char* kDir = "E:\\AI\\跨卡神经渲染原型\\offline\\";
static const char* kOutDir = "E:\\AI\\tmp\\mynr\\";
static const wchar_t* kPak = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";

static bool LoadFile(const std::string& p, std::vector<uint8_t>& v) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) { printf("打不开 %s\n", p.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize((size_t)n);
    fread(v.data(), 1, v.size(), f);
    fclose(f);
    printf("读入 %s : %ld 字节\n", p.c_str(), n);
    return true;
}

// BGRA8（带行距）→ 1920x1080 RGBA8（紧凑），双线性
static void DownscaleColor(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kNetW * kNetH * 4, 0);
    for (UINT y = 0; y < kNetH; ++y) {
        float fy = ((float)y + 0.5f) * (float)kCapH / (float)kNetH - 0.5f;
        int y0 = (int)floorf(fy); float wy = fy - y0;
        int y1 = std::min<int>(y0 + 1, kCapH - 1); if (y0 < 0) y0 = 0;
        for (UINT x = 0; x < kNetW; ++x) {
            float fx = ((float)x + 0.5f) * (float)kCapW / (float)kNetW - 0.5f;
            int x0 = (int)floorf(fx); float wx = fx - x0;
            int x1 = std::min<int>(x0 + 1, kCapW - 1); if (x0 < 0) x0 = 0;
            for (int c = 0; c < 4; ++c) {
                auto px = [&](int xx, int yy) {
                    return (float)src[(size_t)yy * kCapPitchC + (size_t)xx * 4 + c];
                };
                float v = px(x0, y0) * (1 - wx) * (1 - wy) + px(x1, y0) * wx * (1 - wy)
                        + px(x0, y1) * (1 - wx) * wy + px(x1, y1) * wx * wy;
                // 源是 BGRA，目标纹理按 RGBA 摆：交换 R/B
                int oc = (c == 0) ? 2 : (c == 2) ? 0 : c;
                dst[((size_t)y * kNetW + x) * 4 + oc] = (uint8_t)std::min(255.0f, std::max(0.0f, v + 0.5f));
            }
        }
    }
}

// RGBA16 SNORM（带行距）→ 1920x1080 SNORM（紧凑），并按分辨率比例缩放 xy
static void DownscaleMv(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kNetW * kNetH * 8, 0);
    const float sx = (float)kNetW / (float)kCapW;   // 像素单位向量要一起缩
    const float sy = (float)kNetH / (float)kCapH;
    const int16_t* s = (const int16_t*)src.data();
    int16_t* d = (int16_t*)dst.data();
    for (UINT y = 0; y < kNetH; ++y) {
        int sy0 = (int)(((float)y + 0.5f) * (float)kCapH / (float)kNetH);
        if (sy0 >= (int)kCapH) sy0 = kCapH - 1;
        for (UINT x = 0; x < kNetW; ++x) {
            int sx0 = (int)(((float)x + 0.5f) * (float)kCapW / (float)kNetW);
            if (sx0 >= (int)kCapW) sx0 = kCapW - 1;
            size_t si = (size_t)sy0 * (kCapPitchMv / 2) + (size_t)sx0 * 4;
            size_t di = ((size_t)y * kNetW + x) * 4;
            for (int c = 0; c < 4; ++c) {
                float v = (float)s[si + c] / 32767.0f;
                if (c == 0) v *= sx;
                if (c == 1) v *= sy;
                int iv = (int)(v * 32767.0f);
                if (iv < -32768) iv = -32768; if (iv > 32767) iv = 32767;
                d[di + c] = (int16_t)iv;
            }
        }
    }
}

static void SaveBmp(const char* path, const uint8_t* rgba, UINT w, UINT h) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;
    UINT row = ((w * 3 + 3) / 4) * 4;
    UINT dataSize = row * h;
    uint8_t hdr[54] = { 'B','M' };
    UINT fileSize = 54 + dataSize;
    memcpy(hdr + 2, &fileSize, 4);
    UINT off = 54; memcpy(hdr + 10, &off, 4);
    UINT ihs = 40; memcpy(hdr + 14, &ihs, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &dataSize, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = (int)h - 1; y >= 0; --y) {
        for (UINT x = 0; x < w; ++x) {
            const uint8_t* p = rgba + ((size_t)y * w + x) * 4;
            line[x * 3 + 0] = p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[0];
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    printf("写出 %s\n", path);
}

// 4K out.bin（R10G10B10A2 带行距）→ RGBA8 紧凑
static void UnpackOut4K(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kOutW * kOutH * 4, 0);
    for (UINT y = 0; y < kOutH; ++y) {
        const uint32_t* row = (const uint32_t*)(src.data() + (size_t)y * kOutPitch);
        for (UINT x = 0; x < kOutW; ++x) {
            uint32_t v = row[x];
            uint32_t r = (v & 0x3FF), g = ((v >> 10) & 0x3FF), b = ((v >> 20) & 0x3FF);
            uint8_t* o = &dst[((size_t)y * kOutW + x) * 4];
            o[0] = (uint8_t)(r >> 2); o[1] = (uint8_t)(g >> 2); o[2] = (uint8_t)(b >> 2); o[3] = 255;
        }
    }
}

int main() {
    SetConsoleOutputCP(65001);
    CreateDirectoryA("E:\\AI\\tmp\\mynr", nullptr);

    std::vector<uint8_t> rawColor, rawMv, rawOut;
    if (!LoadFile(std::string(kDir) + "nr_cap_color.bin", rawColor)) return 1;
    if (!LoadFile(std::string(kDir) + "nr_cap_mv.bin", rawMv)) return 1;
    if (!LoadFile(std::string(kDir) + "nr_cap_out.bin", rawOut)) return 1;

    std::vector<uint8_t> netColor, netMv;
    DownscaleColor(rawColor, netColor);
    DownscaleMv(rawMv, netMv);
    printf("下采样完成：color %ux%u，mv %ux%u\n", kNetW, kNetH, kNetW, kNetH);

    // 设备（adapter 1 = 9070 XT）
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) { printf("factory 失败\n"); return 1; }
    ComPtr<IDXGIAdapter1> ad;
    if (fac->EnumAdapters1(1, ad.GetAddressOf()) != S_OK) { printf("adapter1 缺失\n"); return 1; }
    DXGI_ADAPTER_DESC ad0{}; ad->GetDesc(&ad0);
    printf("适配器: %ls\n", ad0.Description);
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf())))) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> q;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf()));
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(al.GetAddressOf()));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf()));
    cl->Close();
    ComPtr<ID3D12Fence> fe;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fe.GetAddressOf()));
    UINT64 fv = 0;
    auto submit = [&]() {
        cl->Close();
        ID3D12CommandList* l[] = { cl.Get() };
        q->ExecuteCommandLists(1, l);
        q->Signal(fe.Get(), ++fv);
        while (fe->GetCompletedValue() < fv) Sleep(0);
    };

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    auto mkTex = [&](UINT w, UINT h, DXGI_FORMAT f) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = f; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        return r;
    };
    auto mkBuf = [&](UINT64 n, bool upload) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = n; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(upload ? &uh : &rh, D3D12_HEAP_FLAG_NONE, &bd,
            upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        return r;
    };

    auto texColor = mkTex(kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto texMv    = mkTex(kNetW, kNetH, DXGI_FORMAT_R16G16B16A16_SNORM);

    auto upload = [&](ID3D12Resource* tex, const uint8_t* data, UINT rowBytes, UINT h) {
        D3D12_RESOURCE_DESC td = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0; UINT64 rs = 0, total = 0;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rs, &total);
        auto buf = mkBuf(total, true);
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (FAILED(buf->Map(0, &nr, &m))) return;
        for (UINT y = 0; y < h; ++y)
            memcpy((uint8_t*)m + (size_t)y * fp.Footprint.RowPitch, data + (size_t)y * rowBytes, rowBytes);
        D3D12_RANGE wr{ 0, (SIZE_T)total };
        buf->Unmap(0, &wr);
        al->Reset(); cl->Reset(al.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = tex;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = buf.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = fp;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        submit();
    };
    upload(texColor.Get(), netColor.data(), kNetW * 4, kNetH);
    upload(texMv.Get(), netMv.data(), kNetW * 8, kNetH);
    printf("纹理上传完成\n");

    // lmxxf
    std::wstring dir(kPak);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    std::wstring dllp = dir + L"LmxxfNrRuntime.dll";
    HMODULE mod = LoadLibraryW(dllp.c_str());
    if (!mod) { printf("DLL 加载失败\n"); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    ApiTable api{};
    api.struct_size = (uint32_t)sizeof(ApiTable);
    if (!getApi || getApi(1, &api) != 0) { printf("GetApi 失败\n"); return 1; }
    CreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = dev.Get(); ci.queue = q.Get(); ci.assets_directory = kPak;
    void* ctx = nullptr;
    if (api.Create(&ci, &ctx) != 0) { printf("Create 失败\n"); return 1; }
    if (api.PrepareSession(ctx) != 0) { printf("PrepareSession 失败\n"); return 1; }
    printf("会话就绪\n");

    Job job{};
    job.struct_size = (uint32_t)sizeof(Job);
    FrameInfo160 fi{};
    fi.struct_size = (uint32_t)sizeof(FrameInfo160);
    fi.session_id = 1; fi.frame_id = 1; fi.list_generation = 1;
    fi.command_list = cl.Get();
    fi.color_width = kNetW; fi.color_height = kNetH;
    fi.color = texColor.Get();
    fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
    fi.transfer_strength = 1.0f; fi.color_strength = 1.0f;
    fi.model_scale = 1.0f; fi.pre_exposure = 1.0f; fi.exposure_scale = 1.0f;
    fi.depth = nullptr;
    fi.velocity = texMv.Get();
    fi.ctlTone = 1.0f; fi.ctlStructure = 1.0f; fi.ctlSkin = 3.0f;
    fi.ctlOther = 2.0f; fi.ctlStyle = 0.0f;

    int32_t r = api.PrepareFrame(ctx, &fi, &job);
    printf("PrepareFrame -> %d\n", r);
    if (r != 0) {
        char eb[256] = { 0 }; api.GetLastError(eb, 256);
        printf("  err=%s\n", eb);
        return 1;
    }
    al->Reset(); cl->Reset(al.Get(), nullptr);
    api.RecordInputs(ctx, job.handle, cl.Get());
    submit();
    if (api.ExecuteAfterProducer(ctx, job.handle, q.Get()) != 0) { printf("推理失败\n"); return 1; }
    al->Reset(); cl->Reset(al.Get(), nullptr);
    api.RecordOutputs(ctx, job.handle, cl.Get());
    submit();
    printf("推理完成，private_output=%p\n", job.private_output);

    // 回读两张纹理：fi.color 与 private_output（都在 Retire 之前）
    auto readTex = [&](ID3D12Resource* tex, std::vector<uint8_t>& out) -> bool {
        if (!tex) return false;
        D3D12_RESOURCE_DESC td = tex->GetDesc();
        D3D12_RESOURCE_DESC plain = td; plain.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0; UINT64 rs = 0, total = 0;
        dev->GetCopyableFootprints(&plain, 0, 1, 0, &fp, &rows, &rs, &total);
        auto rb = mkBuf(total, false);
        al->Reset(); cl->Reset(al.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = rb.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = tex;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cl->ResourceBarrier(1, &b);
        submit();
        D3D12_RANGE rr{ 0, (SIZE_T)total };
        void* m = nullptr;
        if (FAILED(rb->Map(0, &rr, &m)) || !m) return false;
        out.resize((size_t)kNetW * kNetH * 4);
        UINT pitch = fp.Footprint.RowPitch;
        for (UINT y = 0; y < kNetH; ++y)
            memcpy(&out[(size_t)y * kNetW * 4], (uint8_t*)m + (size_t)y * pitch, (size_t)kNetW * 4);
        D3D12_RANGE nw{ 0, 0 };
        rb->Unmap(0, &nw);
        return true;
    };

    std::vector<uint8_t> readColor, readPriv;
    bool okC = readTex(texColor.Get(), readColor);
    bool okP = readTex((ID3D12Resource*)job.private_output, readPriv);
    api.Retire(ctx, job.handle);

    // 对比
    auto stats = [&](const char* name, const std::vector<uint8_t>& got) {
        if (got.empty()) { printf("%s : 读不到\n", name); return; }
        double sum = 0; int mx = 0; size_t cnt = 0, same = 0;
        for (size_t i = 0; i < got.size(); ++i) {
            if ((i & 3) == 3) continue;
            int dv = (int)got[i] - (int)netColor[i];
            int ad = dv < 0 ? -dv : dv;
            sum += ad / 255.0; if (ad > mx) mx = ad; cnt++;
            if (ad == 0) same++;
        }
        printf("%s : 与输入平均差 %.4f（最大 %d/255），逐像素相同率 %.1f%%\n",
            name, cnt ? sum / cnt : 0.0, mx, cnt ? 100.0 * same / cnt : 0.0);
    };
    printf("\n===== 结果对比（输入是 1920x1080 的 netColor）=====\n");
    stats("fi.color        ", readColor);
    stats("private_output  ", readPriv);

    // 出图：4K 原图 + (private − 输入) 抬到 4K
    std::vector<uint8_t> before4k;
    UnpackOut4K(rawOut, before4k);
    SaveBmp(std::string(kOutDir + std::string("before4k.bmp")).c_str(), before4k.data(), kOutW, kOutH);
    SaveBmp(std::string(kOutDir + std::string("net_input.bmp")).c_str(), netColor.data(), kNetW, kNetH);

    if (okP && okC) {
        // edit（1080p）→ 双线性抬到 4K → 叠加
        std::vector<uint8_t> after = before4k;
        double sum = 0; int mx = 0; size_t cnt = 0;
        for (UINT y = 0; y < kOutH; ++y) {
            float fy = ((float)y + 0.5f) * (float)kNetH / (float)kOutH - 0.5f;
            int y0 = (int)floorf(fy); float wy = fy - y0;
            int y1 = std::min<int>(y0 + 1, kNetH - 1); if (y0 < 0) y0 = 0;
            for (UINT x = 0; x < kOutW; ++x) {
                float fx = ((float)x + 0.5f) * (float)kNetW / (float)kOutW - 0.5f;
                int x0 = (int)floorf(fx); float wx = fx - x0;
                int x1 = std::min<int>(x0 + 1, kNetW - 1); if (x0 < 0) x0 = 0;
                for (int c = 0; c < 3; ++c) {
                    auto ep = [&](int xx, int yy) {
                        size_t i = ((size_t)yy * kNetW + xx) * 4 + c;
                        return (float)((int)readPriv[i] - (int)netColor[i]) / 255.0f;
                    };
                    float e = ep(x0, y0) * (1 - wx) * (1 - wy) + ep(x1, y0) * wx * (1 - wy)
                            + ep(x0, y1) * (1 - wx) * wy + ep(x1, y1) * wx * wy;
                    int iv = (int)((float)before4k[((size_t)y * kOutW + x) * 4 + c] + e * 255.0f + 0.5f);
                    if (iv < 0) iv = 0; if (iv > 255) iv = 255;
                    after[((size_t)y * kOutW + x) * 4 + c] = (uint8_t)iv;
                    sum += std::abs(e); if ((int)std::abs(e * 255.0f) > mx) mx = (int)std::abs(e * 255.0f);
                    cnt++;
                }
            }
        }
        SaveBmp(std::string(kOutDir + std::string("after4k.bmp")).c_str(), after.data(), kOutW, kOutH);
        printf("\n叠加完成：edit 平均 %.2f/255，峰值 %d/255 → after4k.bmp\n",
            cnt ? sum / cnt : 0.0, mx);
    }

    api.Destroy(ctx);
    printf("\n完成，图在 %s\n", kOutDir);
    return 0;
}
