// 资源创建路径诊断：区分"描述符被拒" / "堆被拒" / "设备状态异常"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static ID3D12Device* g_dev = nullptr;

static void TryCommitted(D3D12_HEAP_TYPE type, D3D12_CPU_PAGE_PROPERTY page,
                         D3D12_MEMORY_POOL pool, D3D12_RESOURCE_STATES state,
                         UINT64 bytes, const char* label) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    hp.CPUPageProperty = page;
    hp.MemoryPoolPreference = pool;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> res;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                IID_PPV_ARGS(&res));
    std::printf("  %-52s -> 0x%08X\n", label, (unsigned)hr);
}

int main() {
    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapters1(0, &adapter);

    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        std::printf("device failed\n");
        return 1;
    }
    g_dev = dev.Get();

    std::printf("device ok, nodes=%u, removedReason=0x%08X\n",
                dev->GetNodeCount(), (unsigned)dev->GetDeviceRemovedReason());

    D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
    HRESULT ho = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt));
    std::printf("CheckFeatureSupport(OPTIONS) -> 0x%08X  ResourceBindingTier=%d TiledResourcesTier=%d\n",
                (unsigned)ho, (int)opt.ResourceBindingTier, (int)opt.TiledResourcesTier);

    // 适配器能力
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    arch.NodeIndex = 0;
    HRESULT ha = dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch));
    std::printf("CheckFeatureSupport(ARCH) -> 0x%08X  UMA=%d CacheCoherent=%d\n",
                (unsigned)ha, (int)arch.UMA, (int)arch.CacheCoherentUMA);

    std::printf("\n--- CreateHeap（不走资源描述符）---\n");
    {
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = 1024 * 1024;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        hd.Properties.CreationNodeMask = 1;
        hd.Properties.VisibleNodeMask = 1;
        hd.Alignment = 0;
        hd.Flags = D3D12_HEAP_FLAG_NONE;
        ComPtr<ID3D12Heap> heap;
        HRESULT hr = dev->CreateHeap(&hd, IID_PPV_ARGS(&heap));
        std::printf("  CreateHeap(DEFAULT 1MB) -> 0x%08X\n", (unsigned)hr);
    }

    std::printf("\n--- CreateCommittedResource 各种组合 ---\n");
    TryCommitted(D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN,
                 D3D12_RESOURCE_STATE_GENERIC_READ, 64 * 1024, "UPLOAD 64KB, page=UNKNOWN, pool=UNKNOWN");
    TryCommitted(D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN,
                 D3D12_RESOURCE_STATE_GENERIC_READ, 4 * 1024 * 1024, "UPLOAD 4MB");
    TryCommitted(D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN,
                 D3D12_RESOURCE_STATE_COPY_DEST, 4 * 1024 * 1024, "READBACK 4MB");
    TryCommitted(D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE, D3D12_MEMORY_POOL_UNKNOWN,
                 D3D12_RESOURCE_STATE_COMMON, 4 * 1024 * 1024, "DEFAULT 4MB, page=NOT_AVAILABLE");
    TryCommitted(D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN,
                 D3D12_RESOURCE_STATE_COMMON, 4 * 1024 * 1024, "DEFAULT 4MB, page=UNKNOWN");

    std::printf("\n--- 纹理（非 buffer）---\n");
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = 256;
        rd.Height = 256;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ComPtr<ID3D12Resource> tex;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                  IID_PPV_ARGS(&tex));
        std::printf("  TEXTURE2D 256x256 RGBA8 -> 0x%08X\n", (unsigned)hr);
    }

    std::printf("\nremovedReason after tests = 0x%08X\n", (unsigned)dev->GetDeviceRemovedReason());
    return 0;
}
