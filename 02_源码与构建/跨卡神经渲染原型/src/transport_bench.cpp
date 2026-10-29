// 跨卡神经渲染原型 —— 阶段 1：跨适配器传输基准测试（独立可执行，不依赖游戏）
//
// 目的：在 7900 XT（adapter 0）与 9070 XT（adapter 1）之间，
//       测量 "共享 buffer + 跨适配器 fence" 的每帧往返延迟。
//       这是决定整套方案是否值得做的唯一硬指标。
//
// 构建：见 README.md
// 运行：TransportBench.exe          默认 14.7MB payload, 200 次
//       TransportBench.exe 8 300    8MB payload, 300 次

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(expr)                                                          \
    do {                                                                     \
        HRESULT _hr = (expr);                                                \
        if (FAILED(_hr)) {                                                   \
            std::printf("FAILED %hs at line %d  hr=0x%08X\n", #expr,         \
                        __LINE__, (unsigned)_hr);                            \
            return _hr;                                                      \
        }                                                                    \
    } while (0)

// 供返回 bool 的 lambda 使用（CHECK 会 return HRESULT，类型不匹配）
#define CHECKB(expr)                                                         \
    do {                                                                     \
        HRESULT _hrb = (expr);                                               \
        if (FAILED(_hrb)) {                                                  \
            std::printf("FAILED %hs at line %d  hr=0x%08X\n", #expr,         \
                        __LINE__, (unsigned)_hrb);                           \
            return false;                                                    \
        }                                                                    \
    } while (0)

static ComPtr<IDXGIAdapter1> PickAdapter(IDXGIFactory6* factory, UINT index) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) != S_OK) return nullptr;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
        std::printf("adapter %u: %ls  (VRAM %llu MB, LUID %08X:%08X)\n", index,
                     desc.Description,
                     (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)),
                     desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
    }
    return adapter;
}

int main(int argc, char** argv) {
    UINT64 payload = 14ull * 1024 * 1024;   // 1440p RGBA8 约 14.7MB，取 14MB 近似
    int iterations = 200;
    if (argc > 1) payload = (UINT64)_atoi64(argv[1]) * 1024 * 1024;
    if (argc > 2) iterations = atoi(argv[2]);

    std::printf("payload = %llu MB, iterations = %d\n",
                (unsigned long long)(payload / (1024 * 1024)), iterations);

    // ---- 枚举两张卡 ----
    ComPtr<IDXGIFactory6> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapterA = PickAdapter(factory.Get(), 0);   // 7900 XT
    ComPtr<IDXGIAdapter1> adapterB = PickAdapter(factory.Get(), 1);   // 9070 XT
    if (!adapterA || !adapterB) { std::printf("need two adapters\n"); return 1; }

    // ---- 两个独立设备 ----
    ComPtr<ID3D12Device> devA, devB;
    CHECK(D3D12CreateDevice(adapterA.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devA)));
    CHECK(D3D12CreateDevice(adapterB.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&devB)));
    std::printf("devices created; A nodes=%u B nodes=%u\n",
                devA->GetNodeCount(), devB->GetNodeCount());

    // ---- 跨适配器共享 buffer（只能在 adapter A 上创建 buffer）----
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;   // AMD 驱动拒绝 NOT_AVAILABLE，必须 UNKNOWN
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Alignment = 0;
    bufDesc.Width = payload;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.SampleDesc.Quality = 0;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> sharedOnA;
    CHECK(devA->CreateCommittedResource(&heapProps,
                                        D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER,
                                        &bufDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(&sharedOnA)));
    std::printf("shared cross-adapter buffer created on A\n");

    HANDLE sharedHandle = nullptr;
    CHECK(devA->CreateSharedHandle(sharedOnA.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle));

    ComPtr<ID3D12Resource> sharedOnB;
    CHECK(devB->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&sharedOnB)));
    std::printf(">>> cross-adapter share established (handle %p) <<<\n", sharedHandle);

    // ---- 本卡缓冲：A 上的源，B 上的目标 ----
    ComPtr<ID3D12Resource> srcOnA, dstOnB;
    CHECK(devA->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(&srcOnA)));
    CHECK(devB->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                        IID_PPV_ARGS(&dstOnB)));

    // ---- 每个设备：队列 / 分配器 / 命令列表 ----
    struct Ctx {
        ComPtr<ID3D12Device> dev;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
    } a, b;

    auto initCtx = [](Ctx& c, ID3D12Device* dev) -> HRESULT {
        c.dev = dev;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HRESULT hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.queue));
        if (FAILED(hr)) return hr;
        hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc));
        if (FAILED(hr)) return hr;
        return dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc.Get(),
                                      nullptr, IID_PPV_ARGS(&c.list));
    };
    CHECK(initCtx(a, devA.Get()));
    CHECK(initCtx(b, devB.Get()));

    // ---- 共享跨适配器 fence（A 创建，B 打开）----
    ComPtr<ID3D12Fence> fenceA;
    CHECK(devA->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                            IID_PPV_ARGS(&fenceA)));
    HANDLE fenceHandle = nullptr;
    CHECK(devA->CreateSharedHandle(fenceA.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle));
    ComPtr<ID3D12Fence> fenceB;
    CHECK(devB->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&fenceB)));

    // ---- 往返循环：A 复制到 shared → signal；B 等 → 从 shared 复制到本地 → signal；A 等 ----
    UINT64 v = 1;
    auto oneRound = [&]() -> bool {
        // A: src -> shared
        CHECKB(a.alloc->Reset());
        CHECKB(a.list->Reset(a.alloc.Get(), nullptr));
        a.list->CopyBufferRegion(sharedOnA.Get(), 0, srcOnA.Get(), 0, payload);
        CHECKB(a.list->Close());
        ID3D12CommandList* listsA[] = { a.list.Get() };
        a.queue->ExecuteCommandLists(1, listsA);
        CHECKB(a.queue->Signal(fenceA.Get(), v));

        // B: 等 A，然后 shared -> dst
        CHECKB(b.queue->Wait(fenceB.Get(), v));
        CHECKB(b.alloc->Reset());
        CHECKB(b.list->Reset(b.alloc.Get(), nullptr));
        b.list->CopyBufferRegion(dstOnB.Get(), 0, sharedOnB.Get(), 0, payload);
        CHECKB(b.list->Close());
        ID3D12CommandList* listsB[] = { b.list.Get() };
        b.queue->ExecuteCommandLists(1, listsB);
        CHECKB(b.queue->Signal(fenceB.Get(), v + 1));

        // A: 等 B 完成
        CHECKB(a.queue->Wait(fenceA.Get(), v + 1));
        v += 2;
        return true;
    };

    // 预热
    for (int i = 0; i < 3; ++i) {
        if (!oneRound()) { std::printf("warm-up failed\n"); return 1; }
    }
    while (fenceA->GetCompletedValue() < v - 1) Sleep(1);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (!oneRound()) { std::printf("benchmark aborted at iteration %d\n", i); return 1; }
    }
    while (fenceA->GetCompletedValue() < v - 1) Sleep(0);
    auto t1 = std::chrono::high_resolution_clock::now();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iterations;
    double gbs = (payload / (1024.0 * 1024.0 * 1024.0)) / (ms / 1000.0);
    std::printf("RESULT: %.3f ms per round-trip, %.2f GB/s  (%d rounds)\n", ms, gbs, iterations);

    if (sharedHandle) CloseHandle(sharedHandle);
    if (fenceHandle) CloseHandle(fenceHandle);
    return 0;
}
