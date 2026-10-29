// mochi_controls4.cpp -- probe MochizukiNrSetControls signature by trial
// Strategy: call SetControls with several arg forms and struct_size presets,
// record return codes. r==0 means that form/size was accepted.
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
};
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; void* reserved0; };

static int32_t CtxOut(void* fn, void* ctx, void* out, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, void*))fn)(ctx, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}
static int32_t CtxOutSize(void* fn, void* ctx, void* out, uint32_t sz, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, void*, uint32_t))fn)(ctx, out, sz); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}
static int32_t CtxSizeOut(void* fn, void* ctx, uint32_t sz, void* out, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, uint32_t, void*))fn)(ctx, sz, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}

static uint8_t g_buf[2048];

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== SetControls signature probe v4 ===\n");

    ComPtr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) { printf("factory fail\n"); return 1; }
    ComPtr<IDXGIAdapter1> ad;
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> c;
        if (fac->EnumAdapters1(i, c.GetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{}; c->GetDesc1(&d);
        if (d.DeviceId == 0x7550 && !ad) ad = c;
    }
    if (!ad) { printf("no 0x7550\n"); return 1; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf())))) { printf("dev fail\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf())))) { printf("queue fail\n"); return 1; }

    HMODULE m = LoadLibraryW(kDll);
    if (!m) { printf("LoadLibrary %lu\n", GetLastError()); return 1; }
    auto getApi = (int32_t(*)(uint32_t, void*))GetProcAddress(m, "LmxxfNrGetApi");
    static ApiTable api{}; api.struct_size = 136;
    if (!getApi || getApi(1, &api) != 0) { printf("GetApi fail\n"); return 1; }
    CreateInfo ci{}; ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = dev.Get(); ci.queue = q.Get(); ci.assets_directory = kAssets;
    void* ctx = nullptr;
    if (api.Create(&ci, &ctx) != 0 || !ctx) { printf("Create fail\n"); return 1; }
    if (api.PrepareSession(ctx) != 0) { printf("PrepareSession fail\n"); return 1; }
    printf("session ok ctx=%p\n", ctx);

    void* fnSet  = (void*)GetProcAddress(m, "MochizukiNrSetControls");
    void* fnDef  = (void*)GetProcAddress(m, "MochizukiNrGetControlDefaults");
    printf("SetControls=%p GetControlDefaults=%p\n", fnSet, fnDef);
    bool crash = false;

    // A) (ctx, ptr) with ptr[0] = size
    printf("\n[A] (ctx,ptr), ptr[0]=size\n");
    for (uint32_t sz = 8; sz <= 512; sz += 8) {
        memset(g_buf, 0, sizeof(g_buf));
        memcpy(g_buf, &sz, 4);
        crash = false;
        int32_t r = CtxOut(fnSet, ctx, g_buf, &crash);
        if (r == 0) printf("  size=%3u -> r=0 ACCEPTED%s\n", sz, crash ? " (crash)" : "");
    }
    // B) (ctx, ptr, size)
    printf("\n[B] (ctx,ptr,size)\n");
    for (uint32_t sz = 8; sz <= 512; sz += 8) {
        memset(g_buf, 0, sizeof(g_buf));
        memcpy(g_buf, &sz, 4);
        crash = false;
        int32_t r = CtxOutSize(fnSet, ctx, g_buf, sz, &crash);
        if (r == 0) printf("  size=%3u -> r=0 ACCEPTED%s\n", sz, crash ? " (crash)" : "");
    }
    // C) (ctx, size, ptr)
    printf("\n[C] (ctx,size,ptr)\n");
    for (uint32_t sz = 8; sz <= 512; sz += 8) {
        memset(g_buf, 0, sizeof(g_buf));
        memcpy(g_buf, &sz, 4);
        crash = false;
        int32_t r = CtxSizeOut(fnSet, ctx, sz, g_buf, &crash);
        if (r == 0) printf("  size=%3u -> r=0 ACCEPTED%s\n", sz, crash ? " (crash)" : "");
    }

    // D) also try GetControlDefaults again but check bytes >=4 only
    printf("\n[D] GetControlDefaults: data beyond first dword?\n");
    for (uint32_t sz = 8; sz <= 512; sz += 8) {
        memset(g_buf, 0, sizeof(g_buf));
        memcpy(g_buf, &sz, 4);
        crash = false;
        int32_t r = CtxOutSize(fnDef, ctx, g_buf, (uint32_t)sizeof(g_buf), &crash);
        if (r != 0) continue;
        bool beyond = false;
        for (uint32_t i = 4; i < sz && i < sizeof(g_buf); ++i) if (g_buf[i]) { beyond = true; break; }
        if (beyond) {
            printf("  size=%3u -> r=0, DATA BEYOND first dword\n", sz);
            for (uint32_t off = 0; off + 8 <= sz; off += 16) {
                printf("   %04X:", off);
                for (uint32_t k = 0; k < 16 && off + k + 4 <= sz; k += 4) {
                    uint32_t u; memcpy(&u, g_buf + off + k, 4);
                    float f;    memcpy(&f, &u, 4);
                    printf(" [%08X f=%8.3f i=%d]", u, f, (int32_t)u);
                }
                printf("\n");
            }
            break;
        }
    }

    api.Destroy(ctx);
    printf("\ndone\n");
    return 0;
}
