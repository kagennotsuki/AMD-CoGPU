// 离线实验：能不能打开 lmxxf 的 history（hist=off → on）
//   变体：不传 mask / 传 R8_UNORM / R32_FLOAT / R16G16B16A16_FLOAT
//   每个变体跑两次同一输入，比较两次 private_output 的差异，并打印 GetStatus 里的 hist 字段
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
    uint64_t session_id, frame_id, list_generation;
    void* command_list;
    uint32_t color_width, color_height;
    void* color;
    uint32_t color_state, flags;
    float transfer_strength, color_strength;
    uint32_t debug_view; float model_scale;
    void* exposure; uint32_t exposure_state;
    float pre_exposure, exposure_scale;
    void* depth; void* velocity;
    float ctlTone, ctlStructure, ctlSkin, ctlOther, ctlStyle;
    uint32_t ctlPad;
    void* historyMask;
    void* reserved152;
};
struct Job { uint32_t struct_size; void* handle; void* private_output; };
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; void* reserved0; };

static const UINT kCapW = 2227, kCapH = 1253, kCapPitchC = 8908, kCapPitchMv = 17816;
static const UINT kOutW = 3840, kOutH = 2160, kOutPitch = 15360;
static const UINT kNetW = 1920, kNetH = 1080;
static const char* kDir = "E:\\AI\\跨卡神经渲染原型\\offline\\";
static const wchar_t* kPak = L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";

static bool LoadFile(const std::string& p, std::vector<uint8_t>& v) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) { printf("打不开 %s\n", p.c_str()); return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    v.resize((size_t)n); fread(v.data(), 1, v.size(), f); fclose(f);
    return true;
}
static void NetFromOut4K(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kNetW * kNetH * 4, 0);
    for (UINT y = 0; y < kNetH; ++y)
        for (UINT x = 0; x < kNetW; ++x) {
            uint32_t acc[3] = { 0,0,0 };
            for (int dy = 0; dy < 2; ++dy) {
                const uint32_t* row = (const uint32_t*)(src.data() + (size_t)(y * 2 + dy) * kOutPitch);
                for (int dx = 0; dx < 2; ++dx) {
                    uint32_t v = row[x * 2 + dx];
                    acc[0] += (v & 0x3FF); acc[1] += ((v >> 10) & 0x3FF); acc[2] += ((v >> 20) & 0x3FF);
                }
            }
            uint8_t* o = &dst[((size_t)y * kNetW + x) * 4];
            o[0] = (uint8_t)((acc[0] / 4) >> 2); o[1] = (uint8_t)((acc[1] / 4) >> 2);
            o[2] = (uint8_t)((acc[2] / 4) >> 2); o[3] = 255;
        }
}
static void NetMv(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst) {
    dst.assign((size_t)kNetW * kNetH * 8, 0);
    const float sx = (float)kNetW / (float)kCapW, sy = (float)kNetH / (float)kCapH;
    const int16_t* s = (const int16_t*)src.data();
    int16_t* d = (int16_t*)dst.data();
    for (UINT y = 0; y < kNetH; ++y) {
        int sy0 = std::min<int>((int)(((float)y + 0.5f) * (float)kCapH / (float)kNetH), kCapH - 1);
        for (UINT x = 0; x < kNetW; ++x) {
            int sx0 = std::min<int>((int)(((float)x + 0.5f) * (float)kCapW / (float)kNetW), kCapW - 1);
            size_t si = (size_t)sy0 * (kCapPitchMv / 2) + (size_t)sx0 * 4;
            size_t di = ((size_t)y * kNetW + x) * 4;
            for (int c = 0; c < 4; ++c) {
                float v = (float)s[si + c] / 32767.0f;
                if (c == 0) v *= sx; if (c == 1) v *= sy;
                int iv = (int)(v * 32767.0f);
                if (iv < -32768) iv = -32768; if (iv > 32767) iv = 32767;
                d[di + c] = (int16_t)iv;
            }
        }
    }
}

int main() {
    SetConsoleOutputCP(65001);
    std::vector<uint8_t> rawOut, rawMv;
    if (!LoadFile(std::string(kDir) + "nr_cap_out.bin", rawOut)) return 1;
    if (!LoadFile(std::string(kDir) + "nr_cap_mv.bin", rawMv)) return 1;
    std::vector<uint8_t> netColor, netMv;
    NetFromOut4K(rawOut, netColor);
    NetMv(rawMv, netMv);
    printf("输入就绪 1920x1080\n");

    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) return 1;
    ComPtr<IDXGIAdapter1> ad;
    if (fac->EnumAdapters1(1, ad.GetAddressOf()) != S_OK) return 1;
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf())))) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> q; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf()));
    ComPtr<ID3D12CommandAllocator> al; ComPtr<ID3D12GraphicsCommandList> cl;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(al.GetAddressOf()));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf()));
    cl->Close();
    ComPtr<ID3D12Fence> fe; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fe.GetAddressOf()));
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
    auto texMask8 = mkTex(kNetW, kNetH, DXGI_FORMAT_R8_UNORM);
    auto texMaskF = mkTex(kNetW, kNetH, DXGI_FORMAT_R32_FLOAT);
    auto texMaskH = mkTex(kNetW, kNetH, DXGI_FORMAT_R16G16B16A16_FLOAT);

    auto upload = [&](ID3D12Resource* tex, const uint8_t* data, UINT rowBytes, UINT h) {
        D3D12_RESOURCE_DESC td = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0; UINT64 rs = 0, total = 0;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rs, &total);
        auto buf = mkBuf(total, true);
        D3D12_RANGE nr{ 0, 0 }; void* m = nullptr;
        if (FAILED(buf->Map(0, &nr, &m))) return;
        for (UINT y = 0; y < h; ++y)
            memcpy((uint8_t*)m + (size_t)y * fp.Footprint.RowPitch, data + (size_t)y * rowBytes, rowBytes);
        D3D12_RANGE wr{ 0, (SIZE_T)total }; buf->Unmap(0, &wr);
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
    {   // mask 全 1
        std::vector<uint8_t> m8((size_t)kNetW * kNetH, 255);
        upload(texMask8.Get(), m8.data(), kNetW, kNetH);
        std::vector<float> mf((size_t)kNetW * kNetH, 1.0f);
        upload(texMaskF.Get(), (const uint8_t*)mf.data(), kNetW * 4, kNetH);
        std::vector<uint16_t> mh((size_t)kNetW * kNetH * 4);
        for (size_t i = 0; i < mh.size(); ++i) mh[i] = 0x3C00;   // 1.0h
        upload(texMaskH.Get(), (const uint8_t*)mh.data(), kNetW * 8, kNetH);
    }

    std::wstring dir(kPak);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    HMODULE mod = LoadLibraryW((dir + L"LmxxfNrRuntime.dll").c_str());
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
    printf("会话就绪\n\n");

    auto readPriv = [&](ID3D12Resource* po, std::vector<uint8_t>& out) -> bool {
        if (!po) return false;
        D3D12_RESOURCE_DESC pd = po->GetDesc();
        D3D12_RESOURCE_DESC plain = pd; plain.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT r2 = 0; UINT64 rs2 = 0, tot = 0;
        dev->GetCopyableFootprints(&plain, 0, 1, 0, &fp, &r2, &rs2, &tot);
        auto rb = mkBuf(tot, false);
        al->Reset(); cl->Reset(al.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = po;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = rb.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = po;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cl->ResourceBarrier(1, &b);
        submit();
        D3D12_RANGE rr{ 0, (SIZE_T)tot }; void* m = nullptr;
        if (FAILED(rb->Map(0, &rr, &m)) || !m) return false;
        out.resize((size_t)kNetW * kNetH * 4);
        UINT pitch = fp.Footprint.RowPitch;
        for (UINT y = 0; y < kNetH; ++y)
            memcpy(&out[(size_t)y * kNetW * 4], (uint8_t*)m + (size_t)y * pitch, (size_t)kNetW * 4);
        D3D12_RANGE nw{ 0, 0 }; rb->Unmap(0, &nw);
        return true;
    };

    struct V { const char* name; ID3D12Resource* mask; };
    V vs[] = {
        { "EnqueueHip 无 mask ", nullptr },
        { "EnqueueHip + R8mask", texMask8.Get() },
    };

    for (auto& v : vs) {
        printf("===== %s =====\n", v.name);
        std::vector<uint8_t> outs[2];
        int okRuns = 0;
        for (int run = 0; run < 2; ++run) {
            Job job{};
            job.struct_size = (uint32_t)sizeof(Job);
            FrameInfo160 fi{};
            fi.struct_size = (uint32_t)sizeof(FrameInfo160);
            fi.session_id = 1; fi.frame_id = (uint64_t)run + 1; fi.list_generation = 1;
            fi.command_list = cl.Get();
            fi.color_width = kNetW; fi.color_height = kNetH;
            fi.color = texColor.Get();
            fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
            fi.transfer_strength = 1.0f; fi.color_strength = 1.0f;
            fi.model_scale = 1.0f; fi.pre_exposure = 1.0f; fi.exposure_scale = 1.0f;
            fi.depth = nullptr;
            fi.velocity = texMv.Get();
            fi.ctlTone = 1.0f; fi.ctlStructure = 1.0f; fi.ctlSkin = 3.0f; fi.ctlOther = 2.0f; fi.ctlStyle = 0.0f;
            fi.historyMask = v.mask;

            int32_t r = api.PrepareFrame(ctx, &fi, &job);
            if (r != 0) {
                char eb[256] = { 0 }; api.GetLastError(eb, 256);
                printf("  run%d PrepareFrame -> %d err=%s\n", run + 1, r, eb);
                break;
            }
            al->Reset(); cl->Reset(al.Get(), nullptr);
            api.RecordInputs(ctx, job.handle, cl.Get());
            submit();
            if (api.EnqueueHip(ctx, job.handle, q.Get()) != 0) { printf("  EnqueueHip 失败\n"); break; }
            al->Reset(); cl->Reset(al.Get(), nullptr);
            api.RecordOutputs(ctx, job.handle, cl.Get());
            submit();
            bool got = readPriv((ID3D12Resource*)job.private_output, outs[run]);
            char st[1024] = { 0 };
            if (api.GetStatus) api.GetStatus(ctx, st, 1024);
            const char* h = strstr(st, "hist=");
            printf("  run%d %s  hist=%.12s\n", run + 1, got ? "ok " : "无输出", h ? h + 5 : "(无)");
            api.Retire(ctx, job.handle);
            if (got) okRuns++;
        }
        if (okRuns == 2) {
            double sum = 0; int mx = 0; size_t cnt = 0;
            for (size_t i = 0; i < outs[0].size(); ++i) {
                if ((i & 3) == 3) continue;
                int dv = (int)outs[1][i] - (int)outs[0][i];
                int ad = dv < 0 ? -dv : dv;
                sum += ad / 255.0; if (ad > mx) mx = ad; cnt++;
            }
            printf("  → 两次输出差：平均 %.4f (%.2f/255) 峰值 %d/255  %s\n",
                cnt ? sum / cnt : 0.0, cnt ? 255.0 * sum / cnt : 0.0, mx,
                mx == 0 ? "（完全一致 = 无历史记忆）" : "（有差异 = 历史在起作用）");
        }
        printf("\n");
    }

    api.Destroy(ctx);
    return 0;
}
