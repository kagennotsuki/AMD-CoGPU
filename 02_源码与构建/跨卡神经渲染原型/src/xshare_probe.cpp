// xshare_probe.cpp —— 验证"跨卡共享系统内存 buffer"在双 A 卡上是否可行
// 背景：CROSS_ADAPTER 被 AMD 拒；但 CUSTOM/L0（系统内存）+ SHARED 可能可行，
//       若可行，两块 AMD 卡能直接访问同一 buffer，不必走 readback+memcpy+upload。
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct Dev {
    ComPtr<IDXGIAdapter1> ad;
    ComPtr<ID3D12Device>  dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fe;
    UINT64 fv = 0;
    std::wstring name;
};

static bool MakeDev(Dev& d, const wchar_t* want, int fallbackIdx) {
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) return false;
    for (UINT i = 0; i < 8; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (fac->EnumAdapters1(i, a.GetAddressOf()) != S_OK) break;
        DXGI_ADAPTER_DESC dd{};
        a->GetDesc(&dd);
        if (want && wcsstr(dd.Description, want)) { d.ad = a; d.name = dd.Description; break; }
        if (want == nullptr && (int)i == fallbackIdx) { d.ad = a; d.name = dd.Description; break; }
    }
    if (!d.ad) return false;
    if (FAILED(D3D12CreateDevice(d.ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d.dev.GetAddressOf())))) return false;
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (FAILED(d.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(d.q.GetAddressOf())))) return false;
    if (FAILED(d.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(d.al.GetAddressOf())))) return false;
    if (FAILED(d.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.al.Get(), nullptr, IID_PPV_ARGS(d.cl.GetAddressOf())))) return false;
    d.cl->Close();
    if (FAILED(d.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(d.fe.GetAddressOf())))) return false;
    return true;
}
static void WaitDev(Dev& d) {
    d.q->Signal(d.fe.Get(), ++d.fv);
    while (d.fe->GetCompletedValue() < d.fv) Sleep(0);
}

// 一次完整实验：在 A 上建 heap→placed buffer→共享→B 上打开→A 写→B 读
static void TryCombo(const char* label, Dev& A, Dev& B,
                     D3D12_HEAP_TYPE ht, D3D12_CPU_PAGE_PROPERTY cpp,
                     D3D12_MEMORY_POOL mp, D3D12_HEAP_FLAGS flags, const UINT64 size) {
    printf("\n--- %s ---\n", label);
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = size;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Properties.Type = ht;
    hd.Properties.CPUPageProperty = cpp;
    hd.Properties.MemoryPoolPreference = mp;
    hd.Properties.CreationNodeMask = 1;
    hd.Properties.VisibleNodeMask = 1;
    hd.Flags = flags;

    ComPtr<ID3D12Heap> heapA;
    HRESULT hr = A.dev->CreateHeap(&hd, IID_PPV_ARGS(heapA.GetAddressOf()));
    printf("  1. A.CreateHeap          = 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "");
    if (FAILED(hr)) return;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;   // 跨适配器资源不能带 UAV

    ComPtr<ID3D12Resource> bufA;
    hr = A.dev->CreatePlacedResource(heapA.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(bufA.GetAddressOf()));
    printf("  2. A.CreatePlacedRes     = 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "");
    if (FAILED(hr)) return;

    HANDLE sh = nullptr;
    hr = A.dev->CreateSharedHandle(heapA.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
    printf("  3. A.CreateSharedHandle  = 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "");
    if (FAILED(hr)) return;

    ComPtr<ID3D12Heap> heapB;
    hr = B.dev->OpenSharedHandle(sh, IID_PPV_ARGS(heapB.GetAddressOf()));
    printf("  4. B.OpenSharedHandle    = 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "");
    if (FAILED(hr)) { CloseHandle(sh); return; }

    ComPtr<ID3D12Resource> bufB;
    hr = B.dev->CreatePlacedResource(heapB.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(bufB.GetAddressOf()));
    printf("  5. B.CreatePlacedRes     = 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "");
    if (FAILED(hr)) { CloseHandle(sh); return; }

    // 6. A 侧写一个模式：用 GPU 从 upload 拷过去
    const size_t bytes = (size_t)size;
    std::vector<unsigned char> pat(bytes), got(bytes, 0);
    for (size_t i = 0; i < bytes; ++i) pat[i] = (unsigned char)(i * 7 + 3);

    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    up.CreationNodeMask = 1; up.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC ub = bd; ub.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> upA;
    if (FAILED(A.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upA.GetAddressOf())))) {
        printf("  !! A upload 创建失败\n"); CloseHandle(sh); return;
    }
    void* pm = nullptr;
    D3D12_RANGE zr{ 0, 0 };
    if (FAILED(upA->Map(0, &zr, &pm))) { printf("  !! A upload Map 失败\n"); CloseHandle(sh); return; }
    memcpy(pm, pat.data(), bytes);
    D3D12_RANGE nw{ 0, 0 };
    upA->Unmap(0, &nw);

    A.al->Reset(); A.cl->Reset(A.al.Get(), nullptr);
    A.cl->CopyBufferRegion(bufA.Get(), 0, upA.Get(), 0, size);
    A.cl->Close();
    { ID3D12CommandList* l[] = { A.cl.Get() }; A.q->ExecuteCommandLists(1, l); }
    WaitDev(A);
    printf("  6. A.GPU 写入            = OK\n");

    // 7. B 侧读回：GPU 拷到 readback
    D3D12_HEAP_PROPERTIES rb{};
    rb.Type = D3D12_HEAP_TYPE_READBACK;
    rb.CreationNodeMask = 1; rb.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> rbB;
    if (FAILED(B.dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &ub,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rbB.GetAddressOf())))) {
        printf("  !! B readback 创建失败\n"); CloseHandle(sh); return;
    }
    B.al->Reset(); B.cl->Reset(B.al.Get(), nullptr);
    B.cl->CopyBufferRegion(rbB.Get(), 0, bufB.Get(), 0, size);
    B.cl->Close();
    { ID3D12CommandList* l[] = { B.cl.Get() }; B.q->ExecuteCommandLists(1, l); }
    WaitDev(B);

    void* rm = nullptr;
    D3D12_RANGE rr{ 0, bytes };
    if (SUCCEEDED(rbB->Map(0, &rr, &rm)) && rm) {
        memcpy(got.data(), rm, bytes);
        D3D12_RANGE n2{ 0, 0 };
        rbB->Unmap(0, &n2);
        size_t bad = 0;
        for (size_t i = 0; i < bytes; ++i) if (got[i] != pat[i]) ++bad;
        printf("  7. B 读到 A 写的数据      = %s（不一致 %zu / %zu 字节）\n",
               bad == 0 ? "★ 完全一致 → 跨卡共享可用" : "内容不符", bad, bytes);
    } else {
        printf("  !! B readback Map 失败\n");
    }
    CloseHandle(sh);
}

int main() {
    printf("=== 跨卡共享 buffer 探针（双 A 卡）===\n");
    Dev A, B;
    if (!MakeDev(A, L"7900", -1)) { printf("!! 找不到 7900 XT\n"); return 1; }
    if (!MakeDev(B, L"9070", -1)) { printf("!! 找不到 9070 XT\n"); return 1; }
    printf("A = %ls\nB = %ls\n", A.name.c_str(), B.name.c_str());

    const UINT64 MB4 = 4ull * 1024 * 1024;
    TryCombo("① CUSTOM / WRITE_BACK / L0 / SHARED            （系统内存共享，最可能是 MGPU 的路）",
             A, B, D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
             D3D12_MEMORY_POOL_L0, D3D12_HEAP_FLAG_SHARED, MB4);
    TryCombo("② CUSTOM / NOT_AVAILABLE / L0 / SHARED          （L0 但声明 CPU 不可见）",
             A, B, D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE,
             D3D12_MEMORY_POOL_L0, D3D12_HEAP_FLAG_SHARED, MB4);
    TryCombo("③ DEFAULT / - / - / SHARED                      （基线：显存共享，预期同卡才可用）",
             A, B, D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
             D3D12_MEMORY_POOL_UNKNOWN, D3D12_HEAP_FLAG_SHARED, MB4);
    TryCombo("④ CUSTOM / WRITE_BACK / L0 / SHARED|CROSS_ADAPTER",
             A, B, D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
             D3D12_MEMORY_POOL_L0, (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER), MB4);
    const UINT64 MB1 = 1ull * 1024 * 1024;
    TryCombo("⑤ DEFAULT / CROSS_ADAPTER 单独",
             A, B, D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
             D3D12_MEMORY_POOL_UNKNOWN, D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER, MB1);
    TryCombo("⑥ DEFAULT / SHARED|CROSS_ADAPTER",
             A, B, D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
             D3D12_MEMORY_POOL_UNKNOWN, (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER), MB1);
    TryCombo("⑦ DEFAULT / CROSS_ADAPTER|ALLOW_ONLY_BUFFERS   ★MGPU 可能用这个",
             A, B, D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
             D3D12_MEMORY_POOL_UNKNOWN, (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS), MB1);
    TryCombo("⑧ DEFAULT / SHARED|CROSS_ADAPTER|ALLOW_ONLY_BUFFERS",
             A, B, D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
             D3D12_MEMORY_POOL_UNKNOWN, (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS), MB1);
    TryCombo("⑨ DEFAULT / SHARED|ALLOW_ONLY_BUFFERS（对照）",
             A, B, D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
             D3D12_MEMORY_POOL_UNKNOWN, (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS), MB1);
    printf("\n--- ⑪ committed buffer + SHARED|CROSS_ADAPTER（不走 heap）---\n");
    {
        D3D12_HEAP_PROPERTIES hp3{};
        hp3.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp3.CreationNodeMask = 1; hp3.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC b3{};
        b3.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b3.Width = MB1; b3.Height = 1; b3.DepthOrArraySize = 1; b3.MipLevels = 1;
        b3.Format = DXGI_FORMAT_UNKNOWN; b3.SampleDesc.Count = 1;
        b3.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        b3.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> rA;
        HRESULT h1 = A.dev->CreateCommittedResource(&hp3,
            (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER),
            &b3, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(rA.GetAddressOf()));
        printf("  1. CreateCommitted(buffer, SHARED|CROSS) = 0x%08X %s\n", (unsigned)h1, SUCCEEDED(h1) ? "OK" : "");
        if (SUCCEEDED(h1)) {
            HANDLE h = nullptr;
            HRESULT h2 = A.dev->CreateSharedHandle(rA.Get(), nullptr, GENERIC_ALL, nullptr, &h);
            printf("  2. CreateSharedHandle(resource)          = 0x%08X %s\n", (unsigned)h2, SUCCEEDED(h2) ? "OK" : "");
            if (SUCCEEDED(h2)) {
                ComPtr<ID3D12Resource> rB;
                HRESULT h3 = B.dev->OpenSharedHandle(h, IID_PPV_ARGS(rB.GetAddressOf()));
                printf("  3. B.OpenSharedHandle(resource)          = 0x%08X %s\n", (unsigned)h3, SUCCEEDED(h3) ? "★ 跨卡共享资源可用" : "");
                CloseHandle(h);
            }
        }
    }
    printf("\n--- ⑫ MGPU 精确复刻：heap(SHARED|CROSS,64K对齐) + placed(ALLOW_CROSS_ADAPTER) ---\n");
    {
        const UINT64 ALIGN = 65536;
        const UINT64 xbytes = ((MB1 + ALIGN - 1) / ALIGN) * ALIGN;
        D3D12_HEAP_PROPERTIES xhp{};
        xhp.Type = D3D12_HEAP_TYPE_DEFAULT;
        xhp.CreationNodeMask = 1; xhp.VisibleNodeMask = 1;
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = xbytes;
        hd.Properties = xhp;
        hd.Alignment = ALIGN;
        hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
        ComPtr<ID3D12Heap> hA;
        HRESULT h1 = A.dev->CreateHeap(&hd, IID_PPV_ARGS(hA.GetAddressOf()));
        printf("  1. A.CreateHeap                        = 0x%08X %s\n", (unsigned)h1, SUCCEEDED(h1) ? "OK" : "");
        if (SUCCEEDED(h1)) {
            D3D12_RESOURCE_DESC xd{};
            xd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xd.Width = xbytes; xd.Height = 1; xd.DepthOrArraySize = 1; xd.MipLevels = 1;
            xd.Format = DXGI_FORMAT_UNKNOWN; xd.SampleDesc.Count = 1;
            xd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            xd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;      // ★ 关键
            ComPtr<ID3D12Resource> bA;
            HRESULT h2 = A.dev->CreatePlacedResource(hA.Get(), 0, &xd, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(bA.GetAddressOf()));
            printf("  2. A.CreatePlaced(ALLOW_CROSS_ADAPTER) = 0x%08X %s\n", (unsigned)h2, SUCCEEDED(h2) ? "OK" : "");
            if (SUCCEEDED(h2)) {
                HANDLE sh = nullptr;
                HRESULT h3 = A.dev->CreateSharedHandle(hA.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
                printf("  3. A.CreateSharedHandle(heap)          = 0x%08X %s\n", (unsigned)h3, SUCCEEDED(h3) ? "OK" : "");
                if (SUCCEEDED(h3)) {
                    ComPtr<ID3D12Heap> hB;
                    HRESULT h4 = B.dev->OpenSharedHandle(sh, IID_PPV_ARGS(hB.GetAddressOf()));
                    printf("  4. B.OpenSharedHandle(heap)            = 0x%08X %s\n", (unsigned)h4, SUCCEEDED(h4) ? "OK" : "");
                    if (SUCCEEDED(h4)) {
                        ComPtr<ID3D12Resource> bB;
                        HRESULT h5 = B.dev->CreatePlacedResource(hB.Get(), 0, &xd, D3D12_RESOURCE_STATE_COMMON,
                                                                 nullptr, IID_PPV_ARGS(bB.GetAddressOf()));
                        printf("  5. B.CreatePlaced                      = 0x%08X %s\n", (unsigned)h5, SUCCEEDED(h5) ? "OK" : "");
                        if (SUCCEEDED(h5)) {
                            // A 写、B 读，验证真的跨卡可见
                            std::vector<unsigned char> pat((size_t)xbytes), got((size_t)xbytes, 0);
                            for (size_t i = 0; i < pat.size(); ++i) pat[i] = (unsigned char)(i * 13 + 7);
                            D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
                            up.CreationNodeMask = 1; up.VisibleNodeMask = 1;
                            D3D12_RESOURCE_DESC ub{}; ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                            ub.Width = xbytes; ub.Height = 1; ub.DepthOrArraySize = 1; ub.MipLevels = 1;
                            ub.Format = DXGI_FORMAT_UNKNOWN; ub.SampleDesc.Count = 1;
                            ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                            ComPtr<ID3D12Resource> uA;
                            A.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(uA.GetAddressOf()));
                            void* pm = nullptr; D3D12_RANGE zr{ 0, 0 };
                            uA->Map(0, &zr, &pm); memcpy(pm, pat.data(), pat.size());
                            D3D12_RANGE nw{ 0, 0 }; uA->Unmap(0, &nw);
                            A.al->Reset(); A.cl->Reset(A.al.Get(), nullptr);
                            A.cl->CopyBufferRegion(bA.Get(), 0, uA.Get(), 0, xbytes);
                            A.cl->Close();
                            { ID3D12CommandList* l[] = { A.cl.Get() }; A.q->ExecuteCommandLists(1, l); }
                            WaitDev(A);
                            D3D12_HEAP_PROPERTIES rh{}; rh.Type = D3D12_HEAP_TYPE_READBACK;
                            rh.CreationNodeMask = 1; rh.VisibleNodeMask = 1;
                            ComPtr<ID3D12Resource> rB;
                            B.dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &ub, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rB.GetAddressOf()));
                            B.al->Reset(); B.cl->Reset(B.al.Get(), nullptr);
                            B.cl->CopyBufferRegion(rB.Get(), 0, bB.Get(), 0, xbytes);
                            B.cl->Close();
                            { ID3D12CommandList* l[] = { B.cl.Get() }; B.q->ExecuteCommandLists(1, l); }
                            WaitDev(B);
                            void* rm = nullptr; D3D12_RANGE rr{ 0, (SIZE_T)xbytes };
                            if (SUCCEEDED(rB->Map(0, &rr, &rm)) && rm) {
                                memcpy(got.data(), rm, got.size());
                                D3D12_RANGE n2{ 0, 0 }; rB->Unmap(0, &n2);
                                size_t bad = 0;
                                for (size_t i = 0; i < got.size(); ++i) if (got[i] != pat[i]) ++bad;
                                printf("  6. B 读到 A 写的数据                    = %s（不一致 %zu 字节）\n",
                                       bad == 0 ? "★★ 跨卡直连共享成功！" : "内容不符", bad);
                            }
                        }
                    }
                    CloseHandle(sh);
                }
            }
        }
    }
    printf("\n=== 完成 ===\n");
    return 0;
}
