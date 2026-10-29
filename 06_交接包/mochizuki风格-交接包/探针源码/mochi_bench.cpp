// 我自己的四组对照探针（离线，不动游戏）
//   ① pColor(2227x1253) → 1080p  + MV
//   ② pOutput(3840x2160) → 1080p + MV
//   ③ pColor → 1080p  + MV + depth
//   ④ pOutput → 1080p + MV + depth
// 每组：跑一次 lmxxf → 回读 private_output → edit = 答案 − 该组喂进去的输入
//      → 4K 叠加图 + edit/锐度数值
#include <chrono>
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;

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

static const UINT kCapW = 2227, kCapH = 1253, kCapPitchC = 8908, kCapPitchMv = 17816, kCapPitchD = 8908;
static const UINT kOutW = 3840, kOutH = 2160, kOutPitch = 15360;
static const UINT kNetW = 1920, kNetH = 1080;
static const char* kDir = "E:\\AI\\跨卡神经渲染原型\\offline\\";
static const char* kOutDir = "E:\\AI\\tmp\\mynr3\\";
static const wchar_t* kPak = L"E:\\AI\\mochizuki-裸文件\\dlssnr-amd";   // 裸副本验证   // RE3 里的完整套装   // mochizuki 资源目录（无尾反斜杠，便于推导 DLL 目录）

static bool LoadFile(const std::string& p, std::vector<uint8_t>& v) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) { printf("打不开 %s\n", p.c_str()); return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    v.resize((size_t)n); fread(v.data(), 1, v.size(), f); fclose(f);
    printf("读入 %s : %ld 字节\n", p.c_str(), n);
    return true;
}

// 2227x1253 BGRA8 → 1920x1080 RGBA8
static void NetFromPColor(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kNetW * kNetH * 4, 0);
    for (UINT y = 0; y < kNetH; ++y) {
        float fy = ((float)y + 0.5f) * (float)kCapH / (float)kNetH - 0.5f;
        int y0 = (int)floorf(fy); float wy = fy - y0; int y1 = std::min<int>(y0 + 1, kCapH - 1); if (y0 < 0) y0 = 0;
        for (UINT x = 0; x < kNetW; ++x) {
            float fx = ((float)x + 0.5f) * (float)kCapW / (float)kNetW - 0.5f;
            int x0 = (int)floorf(fx); float wx = fx - x0; int x1 = std::min<int>(x0 + 1, kCapW - 1); if (x0 < 0) x0 = 0;
            for (int c = 0; c < 4; ++c) {
                auto px = [&](int xx, int yy) { return (float)src[(size_t)yy * kCapPitchC + (size_t)xx * 4 + c]; };
                float v = px(x0,y0)*(1-wx)*(1-wy) + px(x1,y0)*wx*(1-wy) + px(x0,y1)*(1-wx)*wy + px(x1,y1)*wx*wy;
                int oc = (c == 0) ? 2 : (c == 2) ? 0 : c;   // BGRA → RGBA
                dst[((size_t)y*kNetW+x)*4+oc] = (uint8_t)std::min(255.0f, std::max(0.0f, v + 0.5f));
            }
        }
    }
}

// 3840x2160 R10G10B10A2 → 1920x1080 RGBA8（取每 2x2 平均）
static void NetFromOut4K(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kNetW * kNetH * 4, 0);
    for (UINT y = 0; y < kNetH; ++y) {
        for (UINT x = 0; x < kNetW; ++x) {
            uint32_t acc[3] = { 0,0,0 };
            for (int dy = 0; dy < 2; ++dy) {
                const uint32_t* row = (const uint32_t*)(src.data() + (size_t)(y*2+dy) * kOutPitch);
                for (int dx = 0; dx < 2; ++dx) {
                    uint32_t v = row[x*2+dx];
                    acc[0] += (v & 0x3FF); acc[1] += ((v >> 10) & 0x3FF); acc[2] += ((v >> 20) & 0x3FF);
                }
            }
            uint8_t* o = &dst[((size_t)y*kNetW+x)*4];
            o[0] = (uint8_t)((acc[0] / 4) >> 2);
            o[1] = (uint8_t)((acc[1] / 4) >> 2);
            o[2] = (uint8_t)((acc[2] / 4) >> 2);
            o[3] = 255;
        }
    }
}

// 2227x1253 D32（反向Z，float）→ 1920x1080 R32F
static void NetDepth(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kNetW * kNetH * 4, 0);
    float* d = (float*)dst.data();
    for (UINT y = 0; y < kNetH; ++y) {
        int sy = std::min<int>((int)(((float)y + 0.5f) * (float)kCapH / (float)kNetH), kCapH - 1);
        const float* row = (const float*)(src.data() + (size_t)sy * kCapPitchD);
        for (UINT x = 0; x < kNetW; ++x) {
            int sx = std::min<int>((int)(((float)x + 0.5f) * (float)kCapW / (float)kNetW), kCapW - 1);
            d[(size_t)y*kNetW+x] = row[sx];
        }
    }
}

// 2227x1253 RGBA16 SNORM → 1920x1080 SNORM（xy 按比例缩放）
static void NetMv(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kNetW * kNetH * 8, 0);
    const float sx = (float)kNetW / (float)kCapW, sy = (float)kNetH / (float)kCapH;
    const int16_t* s = (const int16_t*)src.data();
    int16_t* d = (int16_t*)dst.data();
    for (UINT y = 0; y < kNetH; ++y) {
        int sy0 = std::min<int>((int)(((float)y+0.5f)*(float)kCapH/(float)kNetH), kCapH-1);
        for (UINT x = 0; x < kNetW; ++x) {
            int sx0 = std::min<int>((int)(((float)x+0.5f)*(float)kCapW/(float)kNetW), kCapW-1);
            size_t si = (size_t)sy0*(kCapPitchMv/2) + (size_t)sx0*4;
            size_t di = ((size_t)y*kNetW+x)*4;
            for (int c = 0; c < 4; ++c) {
                float v = (float)s[si+c]/32767.0f;
                if (c == 0) v *= sx; if (c == 1) v *= sy;
                int iv = (int)(v*32767.0f);
                if (iv < -32768) iv = -32768; if (iv > 32767) iv = 32767;
                d[di+c] = (int16_t)iv;
            }
        }
    }
}

static void SaveBmp(const char* path, const uint8_t* rgba, UINT w, UINT h) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return;
    UINT row = ((w*3+3)/4)*4, dataSize = row*h;
    uint8_t hdr[54] = { 'B','M' };
    UINT fileSize = 54+dataSize, off = 54, ihs = 40;
    memcpy(hdr+2,&fileSize,4); memcpy(hdr+10,&off,4); memcpy(hdr+14,&ihs,4);
    memcpy(hdr+18,&w,4); memcpy(hdr+22,&h,4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr+26,&planes,2); memcpy(hdr+28,&bpp,2); memcpy(hdr+34,&dataSize,4);
    fwrite(hdr,1,54,f);
    std::vector<uint8_t> line(row,0);
    for (int y = (int)h-1; y >= 0; --y) {
        for (UINT x = 0; x < w; ++x) {
            const uint8_t* p = rgba + ((size_t)y*w+x)*4;
            line[x*3+0]=p[2]; line[x*3+1]=p[1]; line[x*3+2]=p[0];
        }
        fwrite(line.data(),1,row,f);
    }
    fclose(f);
    printf("写出 %s\n", path);
}

static void UnpackOut4K(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kOutW*kOutH*4, 0);
    for (UINT y = 0; y < kOutH; ++y) {
        const uint32_t* row = (const uint32_t*)(src.data() + (size_t)y*kOutPitch);
        for (UINT x = 0; x < kOutW; ++x) {
            uint32_t v = row[x];
            uint8_t* o = &dst[((size_t)y*kOutW+x)*4];
            o[0]=(uint8_t)((v & 0x3FF)>>2); o[1]=(uint8_t)(((v>>10)&0x3FF)>>2);
            o[2]=(uint8_t)(((v>>20)&0x3FF)>>2); o[3]=255;
        }
    }
}

// 梯度能量（锐度）：|dx|+|dy| 的平均
static double GradEnergy(const uint8_t* rgba, UINT w, UINT h) {
    double s = 0; size_t n = 0;
    for (UINT y = 1; y < h; ++y) {
        for (UINT x = 1; x < w; ++x) {
            const uint8_t* p = rgba + ((size_t)y*w+x)*4;
            const uint8_t* l = p - 4;
            const uint8_t* u = p - (size_t)w*4;
            for (int c = 0; c < 3; ++c) {
                s += std::abs((int)p[c]-(int)l[c]) + std::abs((int)p[c]-(int)u[c]);
                n++;
            }
        }
    }
    return n ? s/n : 0.0;
}

int main() {
    SetConsoleOutputCP(65001);
    CreateDirectoryA("E:\\AI\\tmp\\mynr3", nullptr);

    std::vector<uint8_t> rawColor, rawMv, rawDepth, rawOut;
    if (!LoadFile(std::string(kDir)+"nr_cap_color.bin", rawColor)) return 1;
    if (!LoadFile(std::string(kDir)+"nr_cap_mv.bin", rawMv)) return 1;
    if (!LoadFile(std::string(kDir)+"nr_cap_depth.bin", rawDepth)) return 1;
    if (!LoadFile(std::string(kDir)+"nr_cap_out.bin", rawOut)) return 1;

    std::vector<uint8_t> netA, netB, netMvBuf, netDep, before4k;
    NetFromPColor(rawColor, netA);
    NetFromOut4K(rawOut, netB);
    NetMv(rawMv, netMvBuf);
    NetDepth(rawDepth, netDep);
    UnpackOut4K(rawOut, before4k);
    printf("输入准备完成；4K 底图锐度 %.3f\n", GradEnergy(before4k.data(), kOutW, kOutH));
    SaveBmp(std::string(std::string(kOutDir)+"base4k.bmp").c_str(), before4k.data(), kOutW, kOutH);
    SaveBmp(std::string(std::string(kOutDir)+"in_pcolor.bmp").c_str(), netA.data(), kNetW, kNetH);
    SaveBmp(std::string(std::string(kOutDir)+"in_pout4k.bmp").c_str(), netB.data(), kNetW, kNetH);

    // 设备
    ComPtr<IDXGIFactory6> fac;
    HRESULT hfac = CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf()));
    printf("CreateDXGIFactory2 hr=0x%08X\n", (unsigned)hfac);
    if (FAILED(hfac)) {
        // 回退到 DXGI 1.4 工厂（部分环境 1.6 会 E_FAIL）
        ComPtr<IDXGIFactory2> f2;
        hfac = CreateDXGIFactory1(IID_PPV_ARGS(f2.GetAddressOf()));
        printf("CreateDXGIFactory1 hr=0x%08X\n", (unsigned)hfac);
        if (FAILED(hfac)) return 1;
        ComPtr<IDXGIFactory6> f6;
        if (SUCCEEDED(f2.As(&f6))) fac = f6;
    }
    ComPtr<IDXGIAdapter1> ad;
    for (UINT ai = 0; ai < 8; ++ai) {
        ComPtr<IDXGIAdapter1> cand;
        if (fac->EnumAdapters1(ai, cand.GetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC d{};
        cand->GetDesc(&d);
        printf("  候选[%u] %ls\n", ai, d.Description);
        // 按 DeviceId 选卡（0x7550 = RX 9070 XT）；名字可能被改过，不能按名字匹配
        printf("      DeviceId=0x%04X VendorId=0x%04X\n", d.DeviceId, d.VendorId);
        if (d.DeviceId == 0x7550) { ad = cand; break; }
        if (!ad && d.VendorId == 0x1002) ad = cand;   // 兜底：第一张 A 卡
    }
    if (!ad) { printf("找不到 9070 XT\n"); return 1; }
    DXGI_ADAPTER_DESC ad0{}; ad->GetDesc(&ad0);
    printf("适配器: %ls\n", ad0.Description);
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf())))) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> q;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf()));
    ComPtr<ID3D12CommandAllocator> al; ComPtr<ID3D12GraphicsCommandList> cl;
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
            upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        return r;
    };

    auto texColor = mkTex(kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto texMv    = mkTex(kNetW, kNetH, DXGI_FORMAT_R16G16B16A16_SNORM);
    auto texDep   = mkTex(kNetW, kNetH, DXGI_FORMAT_R32_FLOAT);

    auto upload = [&](ID3D12Resource* tex, const uint8_t* data, UINT rowBytes, UINT h) {
        D3D12_RESOURCE_DESC td = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0; UINT64 rs = 0, total = 0;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rs, &total);
        auto buf = mkBuf(total, true);
        D3D12_RANGE nr{ 0, 0 }; void* m = nullptr;
        if (FAILED(buf->Map(0, &nr, &m))) return;
        for (UINT y = 0; y < h; ++y)
            memcpy((uint8_t*)m + (size_t)y*fp.Footprint.RowPitch, data + (size_t)y*rowBytes, rowBytes);
        D3D12_RANGE wr{ 0, (SIZE_T)total }; buf->Unmap(0, &wr);
        al->Reset(); cl->Reset(al.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = tex;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = buf.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = fp;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        submit();
    };

    // lmxxf
    std::wstring dir(kPak);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    HMODULE mod = LoadLibraryW((dir + L"MochizukiNrRuntime.dll").c_str());
    if (!mod) { printf("DLL 加载失败\n"); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(mod, "LmxxfNrGetApi");
    ApiTable api{};
    api.struct_size = 136;   // mochizuki = 官方 v1（lmxxf 用 160）
    if (!getApi || getApi(1, &api) != 0) { printf("GetApi 失败\n"); return 1; }
    CreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = dev.Get(); ci.queue = q.Get(); ci.assets_directory = kPak;
    void* ctx = nullptr;
    if (api.Create(&ci, &ctx) != 0) { printf("Create 失败\n"); return 1; }
    if (api.PrepareSession(ctx) != 0) { printf("PrepareSession 失败\n"); return 1; }
    printf("会话就绪\n\n");

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
        D3D12_RANGE rr{ 0, (SIZE_T)total }; void* m = nullptr;
        if (FAILED(rb->Map(0, &rr, &m)) || !m) return false;
        out.resize((size_t)kNetW*kNetH*4);
        UINT pitch = fp.Footprint.RowPitch;
        for (UINT y = 0; y < kNetH; ++y)
            memcpy(&out[(size_t)y*kNetW*4], (uint8_t*)m + (size_t)y*pitch, (size_t)kNetW*4);
        D3D12_RANGE nw{ 0, 0 }; rb->Unmap(0, &nw);
        return true;
    };

    struct Variant { const char* name; float ts; float cs; float cStyle, cTone, cStruct, cSkin; int passes; };

// 风格化扫描：一次只改一个 ctl 字段，和基线 ctl_all_default 对比
static const float D_TONE = 1.0f, D_STRUCT = 1.0f, D_SKIN = 3.0f;
    Variant vs[] = {
    { "p1", 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 3.0f, 1 },
    { "p2", 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 3.0f, 2 },
};

    const int NV = (int)(sizeof(vs)/sizeof(vs[0]));
    for (int vi = 0; vi < NV; ++vi) {
        Variant& v = vs[vi];
        const std::vector<uint8_t>& fed = netB;   // 固定用 pOutput4K
        upload(texColor.Get(), fed.data(), kNetW*4, kNetH);
        upload(texMv.Get(), netMvBuf.data(), kNetW*8, kNetH);
        

        Job job{};
        job.struct_size = (uint32_t)sizeof(Job);
        FrameInfo160 fi{};
        const bool useDepth = (v.cStyle > 0.5f);
        fi.struct_size = useDepth ? 160u : 80u;
        fi.depth = useDepth ? texDep.Get() : nullptr;
        printf("[%s] struct_size=%u depth=%p\n", v.name, fi.struct_size, fi.depth);
        fi.session_id = 1; fi.frame_id = (uint64_t)vi+1; fi.list_generation = 1;
        fi.command_list = cl.Get();
        fi.color_width = kNetW; fi.color_height = kNetH;
        fi.color = texColor.Get();
        fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        fi.transfer_strength = v.ts; fi.color_strength = v.cs;
        fi.flags = 1u;   // LMXXF_NR_FRAME_FLAG_STRENGTH
        fi.model_scale = 1.0f; fi.pre_exposure = 1.0f; fi.exposure_scale = 1.0f;
        fi.depth = nullptr;
        fi.velocity = texMv.Get();
        fi.ctlStyle = v.cStyle; fi.ctlTone = v.cTone; fi.ctlStructure = v.cStruct; fi.ctlSkin = v.cSkin; fi.ctlOther = 2.0f;

        // 等待网络构建完成（首次约 30 秒）
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            int tries = 0;
            for (;;) {
                Job jw{}; jw.struct_size = (uint32_t)sizeof(Job);
                FrameInfo160 fw = fi;
                fw.frame_id = 1;
                int32_t rr = api.PrepareFrame(ctx, &fw, &jw);
                if (rr == 0) { api.Retire(ctx, jw.handle);
                    auto t1 = std::chrono::high_resolution_clock::now();
                    printf("[预热] 网络就绪，用时 %.1f 秒（尝试 %d 次）\n",
                        std::chrono::duration<double>(t1 - t0).count(), tries + 1);
                    break; }
                char eb[256] = { 0 }; api.GetLastError(eb, 256);
                if ((tries % 10) == 0) printf("[预热] %d 次: r=%d %s\n", tries + 1, rr, eb);
                ++tries;
                if (tries > 240) { printf("[预热] 超时放弃\n"); break; }
                Sleep(500);
            }
        }

        // 计时：跑 20 帧取平均（网络 + 编解码 + 交接）
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            const int N = 20;
            int okCnt = 0;
            for (int it = 0; it < N; ++it) {
                Job j2{}; j2.struct_size = (uint32_t)sizeof(Job);
                FrameInfo160 f2 = fi;
                f2.frame_id = (uint64_t)(100 + it);
                if (api.PrepareFrame(ctx, &f2, &j2) != 0) continue;
                al->Reset(); cl->Reset(al.Get(), nullptr);
                api.RecordInputs(ctx, j2.handle, cl.Get());
                submit();
                if (api.ExecuteAfterProducer(ctx, j2.handle, q.Get()) != 0) { api.Retire(ctx, j2.handle); continue; }
                al->Reset(); cl->Reset(al.Get(), nullptr);
                api.RecordOutputs(ctx, j2.handle, cl.Get());
                submit();
                api.Retire(ctx, j2.handle);
                ++okCnt;
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            printf("[计时] %d 帧成功 %d，平均 %.2f ms/帧（含提交与同步）\n", N, okCnt, okCnt ? ms / okCnt : 0.0);
        }

        int32_t r = api.PrepareFrame(ctx, &fi, &job);
        if (r != 0) {
            char eb[256] = { 0 }; api.GetLastError(eb, 256);
            printf("[%s] PrepareFrame -> %d err=%s\n", v.name, r, eb);
            continue;
        }
        al->Reset(); cl->Reset(al.Get(), nullptr);
        api.RecordInputs(ctx, job.handle, cl.Get());
        submit();
        if (api.ExecuteAfterProducer(ctx, job.handle, q.Get()) != 0) { printf("[%s] 推理失败\n", v.name); continue; }
        al->Reset(); cl->Reset(al.Get(), nullptr);
        api.RecordOutputs(ctx, job.handle, cl.Get());
        submit();

        std::vector<uint8_t> priv;
        bool ok = readTex((ID3D12Resource*)job.private_output, priv);
        api.Retire(ctx, job.handle);
        if (!ok) { printf("[%s] private_output 读不到\n", v.name); continue; }

        // 第二遍（串联）：把第一遍输出当作输入再跑一次
        if (v.passes >= 2) {
            upload(texColor.Get(), priv.data(), kNetW*4, kNetH);
            Job j3{}; j3.struct_size = (uint32_t)sizeof(Job);
            FrameInfo160 f3 = fi; f3.frame_id = (uint64_t)(900 + vi); f3.color = texColor.Get();
            if (api.PrepareFrame(ctx, &f3, &j3) == 0) {
                al->Reset(); cl->Reset(al.Get(), nullptr);
                api.RecordInputs(ctx, j3.handle, cl.Get()); submit();
                if (api.ExecuteAfterProducer(ctx, j3.handle, q.Get()) == 0) {
                    al->Reset(); cl->Reset(al.Get(), nullptr);
                    api.RecordOutputs(ctx, j3.handle, cl.Get()); submit();
                    std::vector<uint8_t> p2;
                    if (readTex((ID3D12Resource*)j3.private_output, p2)) priv.swap(p2);
                }
                api.Retire(ctx, j3.handle);
            }
            printf("[%s] 第二遍完成\n", v.name);
        }

        // 自动增益：先测 gain=1 的编辑量，再按目标幅度换算
        const double kTarget = 10.0/255.0;
        double sum0 = 0; size_t cnt0 = 0;
        for (size_t i = 0; i < priv.size(); ++i) { if ((i & 3) == 3) continue;
            int dv = (int)priv[i]-(int)fed[i]; sum0 += (dv<0?-dv:dv)/255.0; ++cnt0; }
        double editMean = cnt0 ? sum0/cnt0 : 0.0;
        float g = editMean > 1e-6 ? (float)(kTarget/editMean) : 1.0f;
        printf("[%s] gain=1 时 edit=%.4f -> 自动增益 %.3f（目标 %.1f/255）\n", v.name, editMean, g, kTarget*255.0);

        // 统计 + 4K 叠加
        double sum = 0; int mx = 0; size_t cnt = 0;
        std::vector<uint8_t> after = before4k;
        for (UINT y = 0; y < kOutH; ++y) {
            float fy = ((float)y+0.5f)*(float)kNetH/(float)kOutH - 0.5f;
            int y0 = (int)floorf(fy); float wy = fy - y0; int y1 = std::min<int>(y0+1, kNetH-1); if (y0 < 0) y0 = 0;
            for (UINT x = 0; x < kOutW; ++x) {
                float fx = ((float)x+0.5f)*(float)kNetW/(float)kOutW - 0.5f;
                int x0 = (int)floorf(fx); float wx = fx - x0; int x1 = std::min<int>(x0+1, kNetW-1); if (x0 < 0) x0 = 0;
                for (int c = 0; c < 3; ++c) {
                    auto ep = [&](int xx, int yy) {
                        size_t i = ((size_t)yy*kNetW+xx)*4 + c;
                        return (float)((int)priv[i] - (int)fed[i]) / 255.0f;
                    };
                    float e = (ep(x0,y0)*(1-wx)*(1-wy) + ep(x1,y0)*wx*(1-wy) + ep(x0,y1)*(1-wx)*wy + ep(x1,y1)*wx*wy) * g;   // 自动增益（幅度对齐）
                    int iv = (int)((float)before4k[((size_t)y*kOutW+x)*4+c] + e*255.0f + 0.5f);
                    if (iv < 0) iv = 0; if (iv > 255) iv = 255;
                    after[((size_t)y*kOutW+x)*4+c] = (uint8_t)iv;
                }
            }
        }
        for (size_t i = 0; i < priv.size(); ++i) {
            if ((i & 3) == 3) continue;
            int dv = (int)priv[i] - (int)fed[i];
            int ad = dv < 0 ? -dv : dv;
            sum += ad/255.0; if (ad > mx) mx = ad; cnt++;
        }
        double gb = GradEnergy(before4k.data(), kOutW, kOutH);
        double ga = GradEnergy(after.data(), kOutW, kOutH);
        printf("[%s] edit 平均 %.4f (%.1f/255) 峰值 %d/255 | 锐度 %.3f → %.3f (%+.1f%%)\n",
            v.name, cnt ? sum/cnt : 0.0, cnt ? 255.0*sum/cnt : 0.0, mx, gb, ga, gb > 0 ? 100.0*(ga-gb)/gb : 0.0);
        {
            char path[512];
            sprintf_s(path, "%sstr_%s.bmp", kOutDir, v.name);
            SaveBmp(path, after.data(), kOutW, kOutH);
        }
    }

    api.Destroy(ctx);
    printf("\n完成，图在 %s\n", kOutDir);
    return 0;
}
