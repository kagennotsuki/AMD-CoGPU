// lmxxf_frame_probe.cpp -- run real frames through LmxxfNrRuntime and print GetStatus (model=...)
// ASCII only. Loads LmxxfNrRuntime.dll from its own directory; assets via LMXXF_ASSETS env.
#include <windows.h>
#include <objbase.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

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
struct FrameInfo {
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
    void* historyMask; void* reserved152;
};
struct Job { uint32_t struct_size; void* handle; void* private_output; };
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; uint32_t flags; uint32_t pad1; };

static const UINT W_ = 1920, H_ = 1080;

//  SEH  POD C2712
static int32_t Sx3(void* fn, void* a, void* b, void* c, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, void*, void*))fn)(a, b, c); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}
static int32_t SxStatus(void* fn, void* ctx, char* buf, uint32_t n, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, char*, uint32_t))fn)(ctx, buf, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}

static ComPtr<ID3D12Resource> MakeTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, bool uav) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
    return r;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("CoInitializeEx -> 0x%08lX\n", (unsigned long)cohr);
    int frames = (argc > 1) ? atoi(argv[1]) : 6;
    int fsOverride = (argc > 2) ? atoi(argv[2]) : 0;
    printf("=== lmxxf frame probe (frames=%d, fsOverride=%d) ===\n", frames, fsOverride);
    char mp[64] = { 0 };
    DWORD mn = GetEnvironmentVariableA("DLSS5_MULTI_PASS", mp, sizeof(mp));
    printf("DLSS5_MULTI_PASS = %s\n", mn ? mp : "(unset)");

    wchar_t dllPath[MAX_PATH]{}; GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
    std::wstring p = dllPath; size_t sl = p.find_last_of(L'\\');
    std::wstring dir = (sl == std::wstring::npos) ? L"." : p.substr(0, sl);
    std::wstring dll = dir + L"\\LmxxfNrRuntime.dll";
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto getApi = (int32_t(*)(uint32_t, void*))GetProcAddress(m, "LmxxfNrGetApi");
    if (!getApi) { printf("no LmxxfNrGetApi\n"); return 1; }

    static ApiTable api{};
    uint32_t sizes[3] = { 160u, 144u, 136u };
    uint32_t acc = 0; int32_t rc = -1;
    for (int i = 0; i < 3; ++i) {
        memset(&api, 0, sizeof(api)); api.struct_size = sizes[i];
        rc = getApi(1, &api);
        printf("  GetApi(%u) -> %d\n", sizes[i], rc);
        if (rc == 0) { acc = sizes[i]; break; }
    }
    if (rc != 0) { char eb[256] = { 0 }; printf("GetApi all rejected\n"); return 1; }
    uint32_t frameSize = (acc == 160u) ? 160u : (acc == 144u ? 104u : 80u);
    printf("  accepted ABI=%u, FrameInfo size=%u\n", acc, frameSize);

    // device: prefer 0x7550
    ComPtr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) { printf("factory fail\n"); return 1; }
    ComPtr<IDXGIAdapter1> ad;
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> c;
        if (fac->EnumAdapters1(i, c.GetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{}; c->GetDesc1(&d);
        if (!ad || d.DeviceId == 0x7550) { ad = c; if (d.DeviceId == 0x7550) break; }
    }
    if (!ad) { printf("no adapter\n"); return 1; }
    { DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d); printf("  adapter: %ls (DeviceId 0x%04X)\n", d.Description, d.DeviceId); }
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf())))) { printf("dev fail\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> q; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf()));
    ComPtr<ID3D12CommandAllocator> al; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(al.GetAddressOf()));
    ComPtr<ID3D12GraphicsCommandList> cl; dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, al.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf()));
    cl->Close();
    ComPtr<ID3D12Fence> fe; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fe.GetAddressOf()));

    // assets
    wchar_t assets[MAX_PATH]{}; DWORD an = GetEnvironmentVariableW(L"LMXXF_ASSETS", assets, MAX_PATH);
    if (!an) { printf("LMXXF_ASSETS not set\n"); return 1; }
    printf("  assets = %ls\n", assets);
    CreateInfo ci{}; ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = dev.Get(); ci.queue = q.Get(); ci.assets_directory = assets;
    void* ctx = nullptr;
    int32_t r = api.Create(&ci, &ctx);
    printf("  Create -> %d ctx=%p\n", r, ctx);
    if (r != 0 || !ctx) { char eb[256] = { 0 }; api.GetLastError(eb, 256); printf("  err=%s\n", eb); return 1; }
    r = api.PrepareSession(ctx);
    printf("  PrepareSession -> %d\n", r);

    // color texture + upload
    ComPtr<ID3D12Resource> color = MakeTex(dev.Get(), W_, H_, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    D3D12_RESOURCE_DESC cd = color->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&cd, 0, 1, 0, &fp, &rows, &rowSize, &total);
    ComPtr<ID3D12Resource> up;
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD; hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(up.GetAddressOf()));
    }
    void* upPtr = nullptr; up->Map(0, nullptr, &upPtr);
    for (UINT y = 0; y < H_; ++y) {
        uint8_t* dst = (uint8_t*)upPtr + fp.Offset + (UINT64)y * fp.Footprint.RowPitch;
        for (UINT x = 0; x < W_; ++x) {
            dst[x * 4 + 0] = (uint8_t)(x & 0xFF);
            dst[x * 4 + 1] = (uint8_t)(y & 0xFF);
            dst[x * 4 + 2] = (uint8_t)((x ^ y) & 0xFF);
            dst[x * 4 + 3] = 255;
        }
    }
    up->Unmap(0, nullptr);

    for (int f = 0; f < frames; ++f) {
        al->Reset(); cl->Reset(al.Get(), nullptr);
        {
            D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = color.Get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = up.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = fp;
            cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        }
        Job job{}; job.struct_size = (uint32_t)sizeof(Job);
        FrameInfo fi{}; fi.struct_size = (fsOverride > 0) ? (uint32_t)fsOverride : frameSize;
        fi.session_id = 1; fi.frame_id = (uint64_t)f + 1; fi.list_generation = 1;
        fi.command_list = cl.Get();
        fi.color_width = W_; fi.color_height = H_;
        fi.color = color.Get();
        fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
        fi.flags = 0;
        fi.transfer_strength = 1.0f; fi.color_strength = 1.0f;
        fi.debug_view = 0; fi.model_scale = 1.0f;
        fi.pre_exposure = 1.0f; fi.exposure_scale = 1.0f;
        fi.depth = nullptr; fi.velocity = nullptr;
        fi.ctlTone = 1.0f; fi.ctlStructure = 1.0f; fi.ctlSkin = 3.0f; fi.ctlOther = 2.0f; fi.ctlStyle = 0.0f;

        bool crash = false;
        printf("  frame %d: [stage] PrepareFrame (fi.struct_size=%u)\n", f, fi.struct_size);
        int32_t rf = Sx3((void*)api.PrepareFrame, ctx, &fi, &job, &crash);
        if (crash) { printf("  frame %d: *** CRASH in PrepareFrame ***\n", f); break; }
        if (rf != 0) { char eb[256] = { 0 }; api.GetLastError(eb, 256); printf("  frame %d PrepareFrame -> %d err=%s\n", f, rf, eb); }
        else {
            printf("  frame %d: [stage] RecordInputs (job.handle=%p)\n", f, job.handle);
            Sx3((void*)api.RecordInputs, ctx, job.handle, cl.Get(), &crash);
            if (crash) { printf("  frame %d: *** CRASH in RecordInputs ***\n", f); break; }
            cl->Close(); { ID3D12CommandList* l[] = { cl.Get() }; q->ExecuteCommandLists(1, l); }
            q->Signal(fe.Get(), (UINT64)f + 1);
            for (int i = 0; i < 4000 && fe->GetCompletedValue() < (UINT64)f + 1; ++i) Sleep(0);
            printf("  frame %d: [stage] ExecuteAfterProducer\n", f);
            int32_t ra = Sx3((void*)api.ExecuteAfterProducer, ctx, job.handle, q.Get(), &crash);
            if (crash) { printf("  frame %d: *** CRASH in ExecuteAfterProducer ***\n", f); break; }
            al->Reset(); cl->Reset(al.Get(), nullptr);
            printf("  frame %d: [stage] RecordOutputs\n", f);
            Sx3((void*)api.RecordOutputs, ctx, job.handle, cl.Get(), &crash);
            if (crash) { printf("  frame %d: *** CRASH in RecordOutputs ***\n", f); break; }
            cl->Close(); { ID3D12CommandList* l[] = { cl.Get() }; q->ExecuteCommandLists(1, l); }
            q->Signal(fe.Get(), (UINT64)f + 2000);
            for (int i = 0; i < 4000 && fe->GetCompletedValue() < (UINT64)f + 2000; ++i) Sleep(0);
            printf("  frame %d: PrepareFrame=0 ExecuteAfterProducer=%d  private_output=%p\n", f, ra, job.private_output);
        }
        if (api.GetStatus) {
            char sb[1024] = { 0 };
            SxStatus((void*)api.GetStatus, ctx, sb, 1024, &crash);
            if (crash) { printf("  frame %d: *** CRASH in GetStatus ***\n", f); break; }
            printf("  [status] %s\n", sb[0] ? sb : "(empty)");
        }
    }
    if (api.Destroy) api.Destroy(ctx);
    printf("done\n");
    return 0;
}
