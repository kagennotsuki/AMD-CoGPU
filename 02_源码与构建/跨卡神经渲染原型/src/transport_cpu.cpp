// 跨卡传输（CPU 中转版）—— AMD 驱动不支持 SHARED_CROSS_ADAPTER 时的唯一可行路径
//
// 每帧流程（单向 A -> B）：
//   A: src -> readback 缓冲（GPU 复制）→ CPU Map → memcpy 到系统内存 → Unmap
//   B: memcpy 系统内存 -> Map(upload) → Unmap → upload -> dst（GPU 复制）
//
// 这正是 HIP 那边测到的 "host bounce" 路径，本程序用 D3D12 复现并计时。

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(expr)                                                          \
    do {                                                                     \
        HRESULT _hr = (expr);                                                \
        if (FAILED(_hr)) {                                                   \
            std::printf("FAILED %hs at line %d  hr=0x%08X\n", #expr,         \
                        __LINE__, (unsigned)_hr);                            \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static ComPtr<IDXGIAdapter1> PickAdapter(IDXGIFactory6* f, UINT i) {
    ComPtr<IDXGIAdapter1> a;
    if (f->EnumAdapters1(i, &a) != S_OK) return nullptr;
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    std::printf("adapter %u: %ls\n", i, d.Description);
    return a;
}

struct Ctx {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
};

static int InitCtx(Ctx& c, ID3D12Device* dev) {
    c.dev = dev;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.queue)))) return 1;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc)))) return 1;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc.Get(), nullptr,
                                      IID_PPV_ARGS(&c.list)))) return 1;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)))) return 1;
    c.list->Close();
    return 0;
}

static void WaitFence(Ctx& c, UINT64 target) {
    while (c.fence->GetCompletedValue() < target) Sleep(0);
}

int main(int argc, char** argv) {
    UINT64 payload = 14ull * 1024 * 1024;
    int iterations = 100;
    if (argc > 1) payload = (UINT64)_atoi64(argv[1]) * 1024 * 1024;
    if (argc > 2) iterations = atoi(argv[2]);
    std::printf("payload = %llu MB, iterations = %d\n",
                (unsigned long long)(payload / (1024 * 1024)), iterations);

    ComPtr<IDXGIFactory6> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adA = PickAdapter(factory.Get(), 0);
    ComPtr<IDXGIAdapter1> adB = PickAdapter(factory.Get(), 1);
    if (!adA || !adB) { std::printf("need two adapters\n"); return 1; }

    ComPtr<ID3D12Device> devA, devB;
    CHECK(D3D12CreateDevice(adA.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devA)));
    CHECK(D3D12CreateDevice(adB.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devB)));
    std::printf("devices ok\n");

    // AMD 驱动要求 CPUPageProperty = UNKNOWN（NOT_AVAILABLE 会被拒，E_INVALIDARG）
    D3D12_HEAP_PROPERTIES defHeap{};
    defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defHeap.CreationNodeMask = 1;
    defHeap.VisibleNodeMask = 1;

    D3D12_HEAP_PROPERTIES uploadHeap = defHeap;
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readbackHeap = defHeap;
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = payload;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> srcA, readbackA, uploadB, dstB;
    CHECK(devA->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&srcA)));
    D3D12_RESOURCE_DESC rbDesc = bufDesc;
    CHECK(devA->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackA)));
    CHECK(devB->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadB)));
    CHECK(devB->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&dstB)));
    std::printf("resources ok (src A, readback A, upload B, dst B)\n");

    Ctx a, b;
    if (InitCtx(a, devA.Get()) || InitCtx(b, devB.Get())) { std::printf("ctx init failed\n"); return 1; }

    // 系统内存中转缓冲
    void* host = VirtualAlloc(nullptr, (SIZE_T)payload, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!host) { std::printf("VirtualAlloc failed\n"); return 1; }

    std::vector<double> samples;
    samples.reserve(iterations);

    for (int i = 0; i < iterations + 3; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();

        // --- A: src -> readback ---
        CHECK(a.alloc->Reset());
        CHECK(a.list->Reset(a.alloc.Get(), nullptr));
        a.list->CopyBufferRegion(readbackA.Get(), 0, srcA.Get(), 0, payload);
        CHECK(a.list->Close());
        ID3D12CommandList* la[] = { a.list.Get() };
        a.queue->ExecuteCommandLists(1, la);
        a.fence->Signal(++a.value);
        WaitFence(a, a.value);

        // --- CPU: 从 readback 读回 ---
        D3D12_RANGE readRange{ 0, (SIZE_T)payload };
        void* mapped = nullptr;
        CHECK(readbackA->Map(0, &readRange, &mapped));
        std::memcpy(host, mapped, (size_t)payload);
        D3D12_RANGE noWrite{ 0, 0 };
        readbackA->Unmap(0, &noWrite);

        // --- CPU: 写入 upload ---
        D3D12_RANGE noRead{ 0, 0 };
        void* upd = nullptr;
        CHECK(uploadB->Map(0, &noRead, &upd));
        std::memcpy(upd, host, (size_t)payload);
        D3D12_RANGE written{ 0, (SIZE_T)payload };
        uploadB->Unmap(0, &written);

        // --- B: upload -> dst ---
        CHECK(b.alloc->Reset());
        CHECK(b.list->Reset(b.alloc.Get(), nullptr));
        b.list->CopyBufferRegion(dstB.Get(), 0, uploadB.Get(), 0, payload);
        CHECK(b.list->Close());
        ID3D12CommandList* lb[] = { b.list.Get() };
        b.queue->ExecuteCommandLists(1, lb);
        b.fence->Signal(++b.value);
        WaitFence(b, b.value);

        auto t1 = std::chrono::high_resolution_clock::now();
        if (i >= 3) samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    double sum = 0, mn = 1e9, mx = 0;
    for (double s : samples) { sum += s; if (s < mn) mn = s; if (s > mx) mx = s; }
    double avg = sum / samples.size();
    double gbs = (payload / (1024.0 * 1024.0 * 1024.0)) / (avg / 1000.0);
    std::printf("RESULT: avg %.3f ms  (min %.3f / max %.3f)  -> %.2f GB/s  one-way A->B\n",
                avg, mn, mx, gbs);
    std::printf("  每帧估算（颜色出 + 深度出 + 结果回 = 3 次单向）: %.1f ms\n", avg * 3.0);
    std::printf("  与 NR 17.1ms 做流水线: 上限 %.1f ms -> %.0f fps\n",
                avg * 3.0 > 17.1 ? avg * 3.0 : 17.1,
                1000.0 / (avg * 3.0 > 17.1 ? avg * 3.0 : 17.1));

    VirtualFree(host, 0, MEM_RELEASE);
    return 0;
}
