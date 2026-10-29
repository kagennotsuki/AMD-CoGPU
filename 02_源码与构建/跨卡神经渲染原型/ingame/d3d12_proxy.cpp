// d3d12.dll 代理 —— 目的：抓游戏真实的 ID3D12Device 与它在用的命令队列
//
// 为什么必须要它：DXGI 的后备缓冲只能由「创建交换链时传入的那个队列」访问，
// 用别的队列去读写会 DXGI_ERROR_DEVICE_REMOVED（v2 代理已实测）。
// 而 IDXGISwapChain::GetDevice 在 RE2 上返回的是设备而不是队列，
// 所以只能在 D3D12CreateDevice + ID3D12Device::CreateCommandQueue 处拦截。
//
// 对外提供（供 dxgi 代理 GetProcAddress）：
//   int32_t CrossGpuQueueCount();
//   ID3D12CommandQueue* CrossGpuGetQueue(int32_t index);
//
// 配置沿用游戏目录的 crossgpu.ini（Log=1 时写 crossgpu_d3d12.log）

#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool g_log = false;
static FILE* g_logFile = nullptr;

static char g_selfAnchor = 0;

static HMODULE SelfModule() {
    static HMODULE hm = nullptr;
    if (!hm)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&g_selfAnchor, &hm);
    return hm;
}

static std::wstring SelfDir() {
    wchar_t path[MAX_PATH]{};
    HMODULE hm = SelfModule();
    if (!hm || !GetModuleFileNameW(hm, path, MAX_PATH)) return std::wstring();
    std::wstring p(path);
    size_t s = p.find_last_of(L'\\');
    return (s == std::wstring::npos) ? std::wstring() : p.substr(0, s);
}

static void LogInit() {
    g_log = false;
    std::wstring dir = SelfDir();
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\crossgpu.ini").c_str(), L"r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            if (_stricmp(line, "Log") == 0 && atoi(eq + 1) != 0) g_log = true;
        }
        fclose(f);
    }
    if (g_log) _wfopen_s(&g_logFile, (dir + L"\\crossgpu_d3d12.log").c_str(), L"w");
}

static void Log(const char* fmt, ...) {
    if (!g_logFile) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

// ---------- 真实 d3d12 ----------
static HMODULE RealD3D12() {
    static HMODULE m = nullptr;
    if (m) return m;
    std::wstring dir = SelfDir();
    m = LoadLibraryW((dir + L"\\d3d12_orig.dll").c_str());
    if (!m) m = LoadLibraryW(L"d3d12_orig.dll");
    Log("d3d12_orig -> %p (err %lu)", m, m ? 0 : GetLastError());
    return m;
}

template <typename Fn> static Fn RealProc(const char* name) {
    HMODULE m = RealD3D12();
    return m ? (Fn)GetProcAddress(m, name) : nullptr;
}

extern "C" void* d3d12FwdTargets[17] = {};
static const char* kFwdNames[17] = {
#include "d3d12_fwd_names.inc"
};

static void InitForwarders() {
    HMODULE m = RealD3D12();
    for (int i = 0; i < 17; ++i) {
        d3d12FwdTargets[i] = m ? (void*)GetProcAddress(m, kFwdNames[i]) : nullptr;
        if (!d3d12FwdTargets[i]) Log("forwarder missing: %s", kFwdNames[i]);
    }
}

// ---------- 队列捕获 ----------
static const int SLOT_CreateCommandQueue = 8;   // ID3D12Device::CreateCommandQueue

static const int kMaxQueues = 8;
static ID3D12CommandQueue* g_queues[kMaxQueues] = {};
static int g_queueCount = 0;
static ID3D12Device* g_device = nullptr;

// 原地补丁记录（每张 vtable 一个）
struct VtblRec { void** vtbl; void* origCreateQueue; bool patched; };
static std::vector<VtblRec*> g_recs;
static void* g_realCreateCommandQueue = nullptr;

static bool IsOurDetour(void* p);

static VtblRec* RecFor(void** vtbl) {
    for (VtblRec* r : g_recs) if (r->vtbl == vtbl) return r;
    VtblRec* r = new VtblRec();
    r->vtbl = vtbl;
    r->origCreateQueue = nullptr;
    r->patched = false;
    g_recs.push_back(r);
    return r;
}

static HRESULT STDMETHODCALLTYPE Detour_CreateCommandQueue(ID3D12Device* self,
    const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** queue) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
    HRESULT hr = ((Fn)g_realCreateCommandQueue)(self, desc, riid, queue);
    if (SUCCEEDED(hr) && queue && *queue) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(((IUnknown*)*queue)->QueryInterface(IID_PPV_ARGS(&q))) && q) {
            if (g_queueCount < kMaxQueues) {
                q->AddRef();                       // 我们长期持有
                g_queues[g_queueCount++] = q;
            } else {
                q->Release();
            }
            Log("captured queue #%d type=%d nodeMask=0x%X  total=%d",
                g_queueCount - 1, desc ? (int)desc->Type : -1,
                desc ? desc->NodeMask : 0, g_queueCount);
        }
    }
    return hr;
}

static void PatchDeviceVtables(ID3D12Device* dev) {
    auto patchOne = [](void* obj) {
        if (!obj) return;
        void** vtbl = *(void***)obj;
        VtblRec* r = RecFor(vtbl);
        if (!r->patched) {
            r->origCreateQueue = vtbl[SLOT_CreateCommandQueue];
            if (!g_realCreateCommandQueue && r->origCreateQueue &&
                (void*)r->origCreateQueue != (void*)&Detour_CreateCommandQueue) {
                g_realCreateCommandQueue = r->origCreateQueue;
                Log("real CreateCommandQueue = %p", g_realCreateCommandQueue);
            }
            r->patched = true;
        }
        DWORD old = 0;
        if (VirtualProtect(&vtbl[SLOT_CreateCommandQueue], sizeof(void*), PAGE_READWRITE, &old)) {
            vtbl[SLOT_CreateCommandQueue] = (void*)&Detour_CreateCommandQueue;
            VirtualProtect(&vtbl[SLOT_CreateCommandQueue], sizeof(void*), old, &old);
        }
    };
    patchOne(dev);
    ID3D12Device1* d1 = nullptr; if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d1))) && d1) { patchOne(d1); d1->Release(); }
    ID3D12Device2* d2 = nullptr; if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d2))) && d2) { patchOne(d2); d2->Release(); }
    ID3D12Device3* d3 = nullptr; if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d3))) && d3) { patchOne(d3); d3->Release(); }
    ID3D12Device4* d4 = nullptr; if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d4))) && d4) { patchOne(d4); d4->Release(); }
    Log("device vtables patched (%zu recs)", g_recs.size());
}

static bool IsOurDetour(void* p) {
    return p == (void*)&Detour_CreateCommandQueue;
}

// ---------- 导出 ----------
extern "C" {

HRESULT WINAPI ProxyD3D12CreateDevice(IUnknown* adapter,
    D3D_FEATURE_LEVEL minFeatureLevel, REFIID riid, void** device) {
    using Fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    Fn real = RealProc<Fn>("D3D12CreateDevice");
    if (!real) return E_FAIL;
    HRESULT hr = real(adapter, minFeatureLevel, riid, device);
    Log("D3D12CreateDevice(fl=0x%X) -> 0x%08X", (unsigned)minFeatureLevel, (unsigned)hr);
    if (SUCCEEDED(hr) && device && *device) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(((IUnknown*)*device)->QueryInterface(IID_PPV_ARGS(&dev))) && dev) {
            dev->AddRef();
            g_device = dev;
            PatchDeviceVtables(dev);
            dev->Release();
        }
    }
    return hr;
}

int32_t CrossGpuQueueCount() { return g_queueCount; }

void* CrossGpuGetQueue(int32_t index) {
    if (index < 0 || index >= g_queueCount) return nullptr;
    return g_queues[index];
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LogInit();
        Log("d3d12 proxy attached (log=%d)", (int)g_log);
        InitForwarders();
    }
    return TRUE;
}
