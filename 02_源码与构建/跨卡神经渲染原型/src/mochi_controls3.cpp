// mochi_controls3.cpp   v3struct_size  +  + GetFeatures
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;

static const wchar_t* kDll    = L"E:\\AI\\mochi_bare\\MochizukiNrRuntime.dll";
static const wchar_t* kAssets = L"E:\\AI\\mochi_bare\\dlssnr-amd";

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
    int32_t (*GetLastError)(char*, uint32_t);
};
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; void* reserved0; };

static int32_t CallSS(void* fn, void* ctx, void* out, uint32_t sz, bool* crashed) {
    __try { *crashed = false; return ((int32_t(*)(void*, void*, uint32_t))fn)(ctx, out, sz); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crashed = true; return -999; }
}
static int32_t CallS2(void* fn, void* ctx, uint32_t sz, void* out, bool* crashed) {
    __try { *crashed = false; return ((int32_t(*)(void*, uint32_t, void*))fn)(ctx, sz, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crashed = true; return -999; }
}
static int32_t CallOO(void* fn, void* ctx, void* out, bool* crashed) {
    __try { *crashed = false; return ((int32_t(*)(void*, void*))fn)(ctx, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crashed = true; return -999; }
}

static uint8_t g_buf[4096];
static bool AnyData(uint32_t n) { for (uint32_t i = 0; i < n && i < sizeof(g_buf); ++i) if (g_buf[i]) return true; return false; }

static void Dump(const char* tag, uint32_t n) {
    printf("\n==== %s%u ====\n", tag, n);
    for (uint32_t off = 0; off + 8 <= n; off += 16) {
        printf("%04X:", off);
        for (uint32_t k = 0; k < 16 && off + k + 4 <= n; k += 4) {
            uint32_t u; memcpy(&u, g_buf + off + k, 4);
            float f;    memcpy(&f, &u, 4);
            printf(" [%08X f=%8.3f i=%d]", u, f, (int32_t)u);
        }
        printf("\n");
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== mochizuki  v3 ===\n");

    ComPtr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) { printf("\n"); return 1; }
    ComPtr<IDXGIAdapter1> ad;
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> c;
        if (fac->EnumAdapters1(i, c.GetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{}; c->GetDesc1(&d);
        if (d.DeviceId == 0x7550 && !ad) ad = c;
    }
    if (!ad) { printf(" 0x7550\n"); return 1; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf())))) { printf("\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf())))) { printf("\n"); return 1; }

    HMODULE m = LoadLibraryW(kDll);
    if (!m) { printf("LoadLibrary %lu\n", GetLastError()); return 1; }
    auto getApi = (int32_t(*)(uint32_t, void*))GetProcAddress(m, "LmxxfNrGetApi");
    static ApiTable api{}; api.struct_size = 136;
    if (!getApi || getApi(1, &api) != 0) { printf("GetApi \n"); return 1; }
    CreateInfo ci{}; ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = dev.Get(); ci.queue = q.Get(); ci.assets_directory = kAssets;
    void* ctx = nullptr;
    if (api.Create(&ci, &ctx) != 0 || !ctx) { printf("Create \n"); return 1; }
    if (api.PrepareSession(ctx) != 0) { printf("PrepareSession \n"); return 1; }
    printf(" ctx=%p\n", ctx);

    void* fnDef  = (void*)GetProcAddress(m, "MochizukiNrGetControlDefaults");
    void* fnFeat = (void*)GetProcAddress(m, "MochizukiNrGetFeatures");
    bool crash = false;
    int32_t r;

    //  1(ctx,out,size) + out  struct_size
    printf("\n--- 1 (ctx,out,size) +  ---\n");
    for (uint32_t sz = 8; sz <= 512; sz += 8) {
        memset(g_buf, 0, sizeof(g_buf));
        memcpy(g_buf, &sz, 4);
        crash = false;
        r = CallSS(fnDef, ctx, g_buf, (uint32_t)sizeof(g_buf), &crash);
        if (r == 0 && AnyData(sz)) {
            printf(" struct_size=%ur=0\n", sz);
            Dump("", sz > 384 ? 384 : sz);
            api.Destroy(ctx);
            return 0;
        }
        if (sz <= 64 || (sz % 64) == 0) printf("  sz=%u r=%d%s\n", sz, r, crash ? " CRASH" : "");
    }
    printf("  1 \n");

    //  2(ctx,size,out)
    printf("\n--- 2 (ctx,size,out) ---\n");
    for (uint32_t sz = 8; sz <= 512; sz += 8) {
        memset(g_buf, 0, sizeof(g_buf));
        crash = false;
        r = CallS2(fnDef, ctx, sz, g_buf, &crash);
        if (r == 0 && AnyData(sz)) {
            printf(" size=%ur=0\n", sz);
            Dump("", sz > 384 ? 384 : sz);
            api.Destroy(ctx);
            return 0;
        }
    }
    printf("  2 \n");

    //  3(ctx,out) + 
    printf("\n--- 3 (ctx,out) +  ---\n");
    for (uint32_t sz = 8; sz <= 512; sz += 8) {
        memset(g_buf, 0, sizeof(g_buf));
        memcpy(g_buf, &sz, 4);
        crash = false;
        r = CallOO(fnDef, ctx, g_buf, &crash);
        if (r == 0 && AnyData(sz)) {
            printf(" struct_size=%ur=0\n", sz);
            Dump("", sz > 384 ? 384 : sz);
            api.Destroy(ctx);
            return 0;
        }
    }
    printf("  3 \n");

    // GetFeatures 
    printf("\n--- GetFeatures  ---\n");
    for (uint32_t sz = 8; sz <= 256; sz += 8) {
        memset(g_buf, 0, sizeof(g_buf));
        memcpy(g_buf, &sz, 4);
        crash = false;
        r = CallSS(fnFeat, ctx, g_buf, (uint32_t)sizeof(g_buf), &crash);
        if (r == 0 && AnyData(sz)) { printf("Features  sz=%u\n", sz); Dump("Features", sz); break; }
    }

    api.Destroy(ctx);
    printf("\n\n");
    return 0;
}
