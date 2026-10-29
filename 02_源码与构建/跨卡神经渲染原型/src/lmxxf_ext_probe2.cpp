// lmxxf_ext_probe2.cpp -- create a real lmxxf session, then probe extended API
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;

static const wchar_t* kDll =
    L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.dll";
static const wchar_t* kPak =
    L"E:\\SteamLibrary\\steamapps\\common\\RESIDENT EVIL 2  BIOHAZARD RE2\\LmxxfNrRuntime.pak";

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

static uint8_t g_out[1024];

static int32_t Call3(void* fn, void* a, void* b, uint32_t c, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, void*, uint32_t))fn)(a, b, c); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}
static int32_t Call2(void* fn, void* a, void* b, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, void*))fn)(a, b); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}


static int32_t Call3S(void* fn, void* ctx, uint32_t sz, void* out, bool* crash) {
    __try { *crash = false; return ((int32_t(*)(void*, uint32_t, void*))fn)(ctx, sz, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *crash = true; return -999; }
}

static void Dump(const char* tag, uint32_t n) {
    printf("\n---- %s (%u bytes) ----\n", tag, n);
    for (uint32_t off = 0; off + 16 <= n; off += 16) {
        printf("%04X:", off);
        for (uint32_t k = 0; k < 16; k += 4) {
            uint32_t u; memcpy(&u, g_out + off + k, 4);
            float f;    memcpy(&f, &u, 4);
            printf(" [%08X f=%8.3f i=%d]", u, f, (int32_t)u);
        }
        printf("\n");
    }
    printf("  ascii: ");
    for (uint32_t i = 0; i < n && i < 300; ++i) {
        char c = (char)g_out[i];
        putchar((c >= 32 && c < 127) ? c : '.');
    }
    printf("\n");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== lmxxf ext probe v2 (with session) ===\n");

    // device
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
    void* fQ  = (void*)GetProcAddress(m, "LmxxfNrQueryContextCapabilities");
    void* fS  = (void*)GetProcAddress(m, "LmxxfNrSetRuntimeSettings");
    auto getApi = (int32_t(*)(uint32_t, void*))GetProcAddress(m, "LmxxfNrGetApi");
    printf("Query=%p Set=%p GetApi=%p\n", fQ, fS, (void*)getApi);

    static ApiTable api{}; api.struct_size = 160;
    int32_t r = getApi ? getApi(1, &api) : -1;
    printf("GetApi(160) -> %d\n", r);
    if (r != 0) return 1;

    CreateInfo ci{}; ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = dev.Get(); ci.queue = q.Get(); ci.assets_directory = kPak;
    void* ctx = nullptr;
    r = api.Create(&ci, &ctx);
    printf("Create -> %d ctx=%p\n", r, ctx);
    if (r != 0 || !ctx) { char eb[256] = {0}; api.GetLastError(eb, 256); printf("err=%s\n", eb); return 1; }

    bool crash = false;
    // Create 之后先试一次
    memset(g_out, 0, sizeof(g_out));
    r = Call3(fQ, ctx, g_out, (uint32_t)sizeof(g_out), &crash);
    printf("\n[after Create] Query(ctx,out,size) -> %d%s\n", r, crash ? " CRASH" : "");
    if (r == 0) Dump("Query after Create", 192);

    r = api.PrepareSession(ctx);
    printf("\nPrepareSession -> %d\n", r);
    if (r != 0) { char eb[256] = {0}; api.GetLastError(eb, 256); printf("err=%s\n", eb); }

    memset(g_out, 0, sizeof(g_out));
    crash = false;
    r = Call3(fQ, ctx, g_out, (uint32_t)sizeof(g_out), &crash);
    printf("\n[after Session] Query(ctx,out,size) -> %d%s\n", r, crash ? " CRASH" : "");
    if (r == 0) Dump("Query after Session", 192);

    memset(g_out, 0, sizeof(g_out));
    crash = false;
    r = Call2(fQ, ctx, g_out, &crash);
    printf("\n[after Session] Query(ctx,out) -> %d%s\n", r, crash ? " CRASH" : "");
    if (r == 0) Dump("Query(ctx,out)", 192);

    // SetRuntimeSettings: struct_size x size 参数 矩阵
    printf("\n=== SetRuntimeSettings matrix (struct_size x sizeArg) ===\n");
    const uint32_t sss[2] = { 16u, 20u };
    const uint32_t args[3] = { 0u, 16u, 20u };
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 3; ++j) {
            uint32_t ss = sss[i], sa = args[j];
            static uint8_t st[64];
            memset(st, 0, sizeof(st));
            memcpy(st, &ss, 4);
            float mm = 1.0f; memcpy(st + 4, &mm, 4);
            uint32_t flag = 1; memcpy(st + 8, &flag, 4);
            uint32_t rp = 0; memcpy(st + 12, &rp, 4);
            if (ss == 20) { uint32_t ft = 0; memcpy(st + 16, &ft, 4); }
            crash = false;
            int32_t rr = Call3(fS, ctx, st, sa, &crash);
            uint32_t ssAfter = 0; memcpy(&ssAfter, st, 4);
            printf("  struct_size=%u sizeArg=%2u -> %d%s  (调用后 st[0]=%u)\n",
                   ss, sa, rr, crash ? " CRASH" : "", ssAfter);
            if (rr == 0) Dump("Set HIT", 24);
        }
    }

    // SetRuntimeSettings 形态
    memset(g_out, 0, sizeof(g_out));
    crash = false;
    r = Call3(fS, ctx, g_out, (uint32_t)sizeof(g_out), &crash);
    printf("\nSet(ctx,out,size) -> %d%s\n", r, crash ? " CRASH" : "");
    if (r == 0) Dump("Set(ctx,out,size)", 128);

    api.Destroy(ctx);
    printf("\ndone\n");
    return 0;
}
