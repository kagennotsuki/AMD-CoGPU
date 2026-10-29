// 跨适配器共享能力探针：分别尝试三种堆标志组合，打印各自 HRESULT
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static bool TryCreate(ID3D12Device* dev, D3D12_HEAP_FLAGS flags, UINT64 bytes,
                      D3D12_RESOURCE_FLAGS resFlags, const char* label) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.SampleDesc.Quality = 0;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = resFlags;

    ComPtr<ID3D12Resource> res;
    HRESULT hr = dev->CreateCommittedResource(&hp, flags, &rd, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, IID_PPV_ARGS(&res));
    std::printf("  %-46s heapFlags=0x%02X resFlags=0x%02X -> 0x%08X%s\n", label,
                (unsigned)flags, (unsigned)resFlags, (unsigned)hr, SUCCEEDED(hr) ? "  OK" : "");
    if (FAILED(hr)) return false;

    // 试着取句柄
    HANDLE h = nullptr;
    HRESULT hh = dev->CreateSharedHandle(res.Get(), nullptr, GENERIC_ALL, nullptr, &h);
    std::printf("      CreateSharedHandle -> 0x%08X%s\n", (unsigned)hh, SUCCEEDED(hh) ? "  OK" : "");
    if (SUCCEEDED(hh) && h) CloseHandle(h);
    return true;
}

int main() {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { std::printf("factory failed\n"); return 1; }

    ComPtr<IDXGIAdapter1> adapters[2];
    for (UINT i = 0; i < 2; ++i) {
        if (factory->EnumAdapters1(i, &adapters[i]) != S_OK) { std::printf("adapter %u missing\n", i); return 1; }
        DXGI_ADAPTER_DESC1 d{};
        adapters[i]->GetDesc1(&d);
        std::printf("adapter %u: %ls\n", i, d.Description);
    }

    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapters[0].Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        std::printf("device failed\n");
        return 1;
    }
    std::printf("device on adapter 0, nodes=%u\n", dev->GetNodeCount());

    const UINT64 mb = 1024ull * 1024ull;
    std::printf("\n--- 4KB buffer, 各种标志 ---\n");
    TryCreate(dev.Get(), D3D12_HEAP_FLAG_NONE, 4096, D3D12_RESOURCE_FLAG_NONE, "plain committed buffer");
    TryCreate(dev.Get(), D3D12_HEAP_FLAG_SHARED, 4096, D3D12_RESOURCE_FLAG_NONE, "SHARED only");
    TryCreate(dev.Get(), D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, 4096, D3D12_RESOURCE_FLAG_NONE, "CROSS_ADAPTER only");
    TryCreate(dev.Get(), D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, 4096,
              D3D12_RESOURCE_FLAG_NONE, "SHARED | CROSS_ADAPTER");

    std::printf("\n--- 14MB buffer ---\n");
    TryCreate(dev.Get(), D3D12_HEAP_FLAG_NONE, 14 * mb, D3D12_RESOURCE_FLAG_NONE, "plain 14MB");
    TryCreate(dev.Get(), D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, 14 * mb,
              D3D12_RESOURCE_FLAG_NONE, "shared cross-adapter 14MB");

    std::printf("\n--- 跨适配器 + ALLOW_SIMULTANEOUS_ACCESS ---\n");
    TryCreate(dev.Get(), D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, 4096,
              D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, "cross-adapter + simultaneous");

    return 0;
}
