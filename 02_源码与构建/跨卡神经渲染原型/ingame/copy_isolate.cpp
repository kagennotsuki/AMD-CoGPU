// 隔离测试：普通纹理（非交换链）→ 读回 buffer，跑 N 次
// 目的：回答"纹理→buffer 的读回在这台机器上到底能不能用"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>

#define CHK(expr, msg) do { HRESULT _h = (expr); std::printf("%-46s 0x%08X\n", msg, (unsigned)_h); \
    std::fflush(stdout); if (FAILED(_h)) return 1; } while (0)

int main() {
    IDXGIFactory6* f = nullptr;
    CHK(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)), "CreateDXGIFactory2");
    if (!f) return 1;

    IDXGIAdapter1* ad = nullptr;
    HRESULT hr = f->EnumAdapters1(0, &ad);
    std::printf("%-46s 0x%08X\n", "EnumAdapters1", (unsigned)hr);
    if (FAILED(hr) || !ad) return 1;

    ID3D12Device* dev = nullptr;
    CHK(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)), "D3D12CreateDevice");
    if (!dev) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{};
    ID3D12CommandQueue* q = nullptr;
    CHK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)), "CreateCommandQueue");
    if (!q) return 1;

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1;
    dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES rh = dh;
    rh.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 640;
    td.Height = 480;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* tex = nullptr;
    CHK(dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&tex)), "CreateCommittedResource(texture)");
    if (!tex) return 1;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &total);
    std::printf("footprint: %ux%u rowPitch=%u rows=%u total=%llu\n",
                fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.RowPitch, rows,
                (unsigned long long)total);

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* rb = nullptr;
    CHK(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)), "CreateCommittedResource(readback buf)");
    if (!rb) return 1;

    ID3D12CommandAllocator* al = nullptr;
    CHK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "CreateCommandAllocator");
    ID3D12GraphicsCommandList* cl = nullptr;
    CHK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)), "CreateCommandList");
    cl->Close();
    ID3D12Fence* fe = nullptr;
    CHK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fe)), "CreateFence");

    UINT64 v = 0;
    int okCount = 0;
    for (int i = 0; i < 10; ++i) {
        al->Reset();
        cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        cl->Close();

        ID3D12CommandList* ls[] = { cl };
        q->ExecuteCommandLists(1, ls);
        q->Signal(fe, ++v);
        while (fe->GetCompletedValue() < v) Sleep(0);

        HRESULT rr = dev->GetDeviceRemovedReason();
        std::printf("iter %2d: deviceRemovedReason=0x%08X\n", i, (unsigned)rr);
        std::fflush(stdout);
        if (FAILED(rr)) break;
        okCount++;
    }

    std::printf("\nRESULT: %d/10 iterations succeeded\n", okCount);
    return (okCount == 10) ? 0 : 1;
}
