// mochi_controls2.cpp —— 建会话后读取 mochizuki 控制块，定位风格字段偏移
// 已知默认：NRStyle=0, NRIntensity=1.0, NRLocalTone=1.0, NRLocalStructure=1.0,
//           NRSkinStructure=-1.0, NRAutoMask=0
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

// __try 不能出现在带析构对象的函数里 → 抽成纯 POD 的辅助函数
static int32_t Call3(void* fn, void* a, void* b, uint32_t c, bool* crashed) {
    __try {
        *crashed = false;
        return ((int32_t(*)(void*, void*, uint32_t))fn)(a, b, c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return -999;
    }
}
static int32_t Call2(void* fn, void* a, void* b, bool* crashed) {
    __try {
        *crashed = false;
        return ((int32_t(*)(void*, void*))fn)(a, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return -999;
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== mochizuki 控制块探测（带会话）===\n");

    // ---- 1. DXGI 选卡（DeviceId 0x7550）+ D3D12 设备 ----
    ComPtr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) { printf("CreateDXGIFactory2 失败\n"); return 1; }
    ComPtr<IDXGIAdapter1> ad;
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> c;
        if (fac->EnumAdapters1(i, c.GetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{}; c->GetDesc1(&d);
        printf("  候选[%u] %ls DeviceId=0x%04X\n", i, d.Description, d.DeviceId);
        if (d.DeviceId == 0x7550 && !ad) ad = c;
    }
    if (!ad) { printf("找不到 DeviceId 0x7550\n"); return 1; }
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf())))) { printf("设备创建失败\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf())))) { printf("队列失败\n"); return 1; }
    printf("  设备与队列就绪\n");

    // ---- 2. 载入运行时并建会话 ----
    HMODULE m = LoadLibraryW(kDll);
    if (!m) { printf("LoadLibrary 失败 %lu\n", GetLastError()); return 1; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(m, "LmxxfNrGetApi");
    static ApiTable api{};
    api.struct_size = 136;
    int32_t r = getApi ? getApi(1, &api) : -1;
    printf("  GetApi(136) -> %d\n", r);
    if (r != 0) return 1;

    CreateInfo ci{}; ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = dev.Get(); ci.queue = q.Get(); ci.assets_directory = kAssets;
    void* ctx = nullptr;
    r = api.Create(&ci, &ctx);
    printf("  Create -> %d ctx=%p\n", r, ctx);
    if (r != 0) { char eb[256] = {0}; api.GetLastError(eb, 256); printf("  err=%s\n", eb); return 1; }
    r = api.PrepareSession(ctx);
    printf("  PrepareSession -> %d\n", r);
    if (r != 0) { char eb[256] = {0}; api.GetLastError(eb, 256); printf("  err=%s\n", eb); return 1; }

    // ---- 3. 读控制块（试多种形态）----
    using Fn3 = int32_t(*)(void*, void*, uint32_t);
    using Fn2 = int32_t(*)(void*, void*);
    static uint8_t buf[4096];
    auto* getDef = GetProcAddress(m, "MochizukiNrGetControlDefaults");
    auto* getFeat = GetProcAddress(m, "MochizukiNrGetFeatures");

    auto dump = [&](const char* tag, size_t n) {
        printf("\n---- %s ----\n", tag);
        for (size_t off = 0; off + 8 <= n; off += 16) {
            printf("%04zX:", off);
            for (size_t k = 0; k < 16 && off + k + 4 <= n; k += 4) {
                uint32_t u; memcpy(&u, buf + off + k, 4);
                float f;    memcpy(&f, &u, 4);
                printf(" [%08X f=%8.3f i=%d]", u, f, (int32_t)u);
            }
            printf("\n");
        }
    };

    memset(buf, 0, sizeof(buf)); bool crash = false; r = Call3((void*)getDef, ctx, buf, (uint32_t)sizeof(buf), &crash);
    printf("\nGetControlDefaults(ctx,out,size) -> %d%s\n", r, crash ? " [崩溃]" : "");
    bool any = false; for (size_t i = 0; i < 512; ++i) if (buf[i]) { any = true; break; }
    if (r == 0 && any) dump("控制块 (ctx,out,size)", 320);

    if (!(r == 0 && any)) {
        memset(buf, 0, sizeof(buf)); crash = false; r = Call2((void*)getDef, ctx, buf, &crash);
        printf("\nGetControlDefaults(ctx,out) -> %d%s\n", r, crash ? " [崩溃]" : "");
        any = false; for (size_t i = 0; i < 512; ++i) if (buf[i]) { any = true; break; }
        if (r == 0 && any) dump("控制块 (ctx,out)", 320);
    }

    // 附带：GetFeatures(ctx,...)
    memset(buf, 0, sizeof(buf)); crash = false; r = Call3((void*)getFeat, ctx, buf, (uint32_t)sizeof(buf), &crash);
    printf("\nGetFeatures(ctx,out,size) -> %d%s\n", r, crash ? " [崩溃]" : "");
    if (r == 0) dump("Features", 128);

    api.Destroy(ctx);
    printf("\n完成\n");
    return 0;
}
