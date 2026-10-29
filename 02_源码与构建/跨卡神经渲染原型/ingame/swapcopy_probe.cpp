// 交换链抓帧变体探测：找出哪种"抓后备缓冲"的做法在 AMD + flip 模型下不会挂 GPU
//
// 用法: swapcopy_probe.exe <variant> [frames]
//   variant 0 = 不抓（基线，只渲染+Present）
//   variant 1 = 屏障 PRESENT->COPY_SOURCE + 纹理->buffer + 屏障回 PRESENT   （当前做法）
//   variant 2 = 不加屏障，直接纹理->buffer
//   variant 3 = CopyResource 到自有纹理（PRESENT->COPY_SOURCE 屏障），再从自有纹理->buffer
//   variant 4 = 同 1，但用 bitblt 交换链（DXGI_SWAP_EFFECT_DISCARD）
//   variant 5 = 同 1，但跳过前 2 帧不抓

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>

static void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    l->ResourceBarrier(1, &br);
}

int main(int argc, char** argv) {
    int variant = (argc > 1) ? atoi(argv[1]) : 1;
    int frames = (argc > 2) ? atoi(argv[2]) : 10;
    std::printf("variant=%d frames=%d\n", variant, frames);

    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return 1;
    IDXGIAdapter1* ad = nullptr;
    f->EnumAdapters1(0, &ad);
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    ID3D12CommandQueue* q = nullptr;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SwapCopyProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"SwapCopyProbe", L"t", WS_OVERLAPPEDWINDOW,
                                0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = 640; scd.Height = 480;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = (variant == 4) ? 1 : 2;
    scd.SwapEffect = (variant == 4) ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap = nullptr;
    HRESULT hr = f->CreateSwapChainForHwnd(q, hwnd, &scd, nullptr, nullptr, &swap);
    std::printf("swapchain (effect=%d buffers=%u) -> 0x%08X\n", (int)scd.SwapEffect, scd.BufferCount, (unsigned)hr);
    if (FAILED(hr)) return 1;

    ID3D12Resource* bb = nullptr;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&bb)))) return 1;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtvh = nullptr;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh));
    dev->CreateRenderTargetView(bb, nullptr, rtvh->GetCPUDescriptorHandleForHeapStart());

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1;
    dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES rh = dh;
    rh.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC td = bb->GetDesc();
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &total);

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(dev->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) {
        std::printf("readback buffer failed\n"); return 1;
    }

    // variant 3 需要一张自有纹理
    ID3D12Resource* own = nullptr;
    if (variant == 3) {
        D3D12_RESOURCE_DESC od = td;
        od.Flags = D3D12_RESOURCE_FLAG_NONE;
        dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &od,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&own));
    }

    ID3D12CommandAllocator* al = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ID3D12Fence* fe = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fe));
    UINT64 v = 0;

    auto doCopy = [&]() {
        if (variant == 0) return;
        if (variant == 5 && v < 4) return;      // 跳过前两帧
        if (variant == 3) {
            al->Reset(); cl->Reset(al, nullptr);
            Barrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->CopyResource(own, bb);
            Barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            Barrier(cl, own, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = rb; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = own; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
            cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            cl->Close();
            ID3D12CommandList* ls[] = { cl };
            q->ExecuteCommandLists(1, ls);
            q->Signal(fe, ++v);
            while (fe->GetCompletedValue() < v) Sleep(0);
            return;
        }
        al->Reset(); cl->Reset(al, nullptr);
        if (variant != 2) Barrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = rb; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = bb; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        if (variant != 2) Barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        cl->Close();
        ID3D12CommandList* ls[] = { cl };
        q->ExecuteCommandLists(1, ls);
        q->Signal(fe, ++v);
        while (fe->GetCompletedValue() < v) Sleep(0);
    };

    int ok = 0;
    for (int i = 0; i < frames; ++i) {
        // 每帧重新取后备缓冲并重建 RTV（flip 模型下这是标准做法）
        ID3D12Resource* cur = nullptr;
        if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&cur))) && cur) {
            if (cur != bb) { std::printf("frame %d: buffer changed %p -> %p\n", i, (void*)bb, (void*)cur); bb = cur; }
            else cur->Release();
        }
        dev->CreateRenderTargetView(bb, nullptr, rtvh->GetCPUDescriptorHandleForHeapStart());
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (variant == 19 || variant == 20) {
            // 19: 每帧按 GetCurrentBackBufferIndex 取缓冲并写入
            // 20: 同 19，但写 + 抓
            IDXGISwapChain3* sc3 = nullptr;
            swap->QueryInterface(IID_PPV_ARGS(&sc3));
            if (!sc3) { std::printf("variant19: no IDXGISwapChain3\n"); break; }
            UINT idx = sc3->GetCurrentBackBufferIndex();
            ID3D12Resource* curBb = nullptr;
            if (FAILED(swap->GetBuffer(idx, IID_PPV_ARGS(&curBb))) || !curBb) {
                std::printf("variant19: GetBuffer(%u) failed\n", idx); sc3->Release(); break;
            }
            dev->CreateRenderTargetView(curBb, nullptr, rtvh->GetCPUDescriptorHandleForHeapStart());
            al->Reset(); cl->Reset(al, nullptr);
            Barrier(cl, curBb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float c4[4] = { 0.1f, 0.3f, 0.9f, 1.0f };
            cl->ClearRenderTargetView(rtvh->GetCPUDescriptorHandleForHeapStart(), c4, 0, nullptr);
            Barrier(cl, curBb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            if (variant == 20) {
                Barrier(cl, curBb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dc{};
                dc.pResource = rb; dc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dc.PlacedFootprint = fp;
                D3D12_TEXTURE_COPY_LOCATION sc_{};
                sc_.pResource = curBb; sc_.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sc_.SubresourceIndex = 0;
                cl->CopyTextureRegion(&dc, 0, 0, 0, &sc_, nullptr);
                Barrier(cl, curBb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            }
            cl->Close();
            ID3D12CommandList* lw3[] = { cl };
            q->ExecuteCommandLists(1, lw3);
            q->Signal(fe, ++v);
            HRESULT rh = swap->Present(0, 0);
            HRESULT rr = dev->GetDeviceRemovedReason();
            std::printf("frame %d (v%d idx=%u): Present=0x%08X reason=0x%08X\n", i, variant, idx, (unsigned)rh, (unsigned)rr);
            std::fflush(stdout);
            curBb->Release(); sc3->Release();
            if (FAILED(rr) || FAILED(rh)) break;
            ok++;
            Sleep(16);
            continue;
        }

        if (variant == 17 || variant == 18) {
            // 17: 只在第 0 帧写一次；18: 每 3 帧写一次
            bool doWrite = (variant == 17) ? (i == 0) : (i % 3 == 0);
            if (doWrite) {
                al->Reset(); cl->Reset(al, nullptr);
                Barrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
                const float c3[4] = { 0.1f, 0.9f, 0.1f, 1.0f };
                cl->ClearRenderTargetView(rtvh->GetCPUDescriptorHandleForHeapStart(), c3, 0, nullptr);
                Barrier(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                cl->Close();
                ID3D12CommandList* lw2[] = { cl };
                q->ExecuteCommandLists(1, lw2);
                q->Signal(fe, ++v);
            }
            HRESULT rh = swap->Present(0, 0);
            HRESULT rr = dev->GetDeviceRemovedReason();
            std::printf("frame %d (v%d write=%d): Present=0x%08X reason=0x%08X\n", i, variant, (int)doWrite, (unsigned)rh, (unsigned)rr);
            std::fflush(stdout);
            if (FAILED(rr) || FAILED(rh)) break;
            ok++;
            Sleep(16);
            continue;
        }

        if (variant == 14 || variant == 15) {
            // 写回实验：先往上贴/画，再 Present
            al->Reset(); cl->Reset(al, nullptr);
            if (variant == 14) {
                // 用 COPY_DEST 把一整帧从 upload buffer 贴到后备缓冲
                static ID3D12Resource* up = nullptr;
                static D3D12_PLACED_SUBRESOURCE_FOOTPRINT upfp{};
                static UINT64 uptotal = 0;
                if (!up) {
                    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
                    D3D12_RESOURCE_DESC ubd = bd; ubd.Width = total;
                    if (FAILED(dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &ubd,
                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) {
                        std::printf("variant14: upload buffer create failed\n"); variant = 0;
                    } else {
                        upfp = fp; uptotal = total;
                        void* m = nullptr; D3D12_RANGE nr{0,0};
                        up->Map(0, &nr, &m);
                        if (m) memset(m, 0x40, (size_t)uptotal);
                        std::printf("variant14: upload %llu bytes ready\n", (unsigned long long)uptotal);
                    }
                }
                if (up) {
                    Barrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION wd{};
                    wd.pResource = bb; wd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; wd.SubresourceIndex = 0;
                    D3D12_TEXTURE_COPY_LOCATION ws{};
                    ws.pResource = up; ws.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; ws.PlacedFootprint = upfp;
                    cl->CopyTextureRegion(&wd, 0, 0, 0, &ws, nullptr);
                    Barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
                }
            } else {
                // 用 RENDER_TARGET 清屏写入（渲染路径）
                Barrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
                const float c2[4] = { 0.9f, 0.1f, 0.1f, 1.0f };
                cl->ClearRenderTargetView(rtvh->GetCPUDescriptorHandleForHeapStart(), c2, 0, nullptr);
                Barrier(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            }
            cl->Close();
            ID3D12CommandList* lw[] = { cl };
            q->ExecuteCommandLists(1, lw);
            q->Signal(fe, ++v);
            HRESULT rh = swap->Present(0, 0);
            HRESULT rr = dev->GetDeviceRemovedReason();
            std::printf("frame %d (write v%d): Present=0x%08X reason=0x%08X\n", i, variant, (unsigned)rh, (unsigned)rr);
            std::fflush(stdout);
            if (FAILED(rr) || FAILED(rh)) break;
            ok++;
            Sleep(16);
            continue;
        }

        if (variant == 12 || variant == 13) {
            // 不渲染，只在 Present 前抓帧
            al->Reset(); cl->Reset(al, nullptr);
            if (variant == 12) Barrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = rb; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = fp;
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = bb; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
            cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            if (variant == 12) Barrier(cl, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            cl->Close();
            ID3D12CommandList* lc[] = { cl };
            q->ExecuteCommandLists(1, lc);
            q->Signal(fe, ++v);
            while (fe->GetCompletedValue() < v) Sleep(0);
            HRESULT rrc = dev->GetDeviceRemovedReason();
            if (FAILED(rrc)) { std::printf("frame %d: 抓帧拷贝后设备移除 0x%08X\n", i, (unsigned)rrc); break; }

            hr = swap->Present(0, 0);
            HRESULT rrp = dev->GetDeviceRemovedReason();
            std::printf("frame %d (present+copy v%d): Present=0x%08X reason=0x%08X\n", i, variant, (unsigned)hr, (unsigned)rrp);
            std::fflush(stdout);
            if (FAILED(rrp) || FAILED(hr)) break;
            ok++;
            Sleep(16);
            continue;
        }
        if (variant == 6) {
            // 只 Present，完全不渲染
            hr = swap->Present(0, 0);
            HRESULT rr6 = dev->GetDeviceRemovedReason();
            std::printf("frame %d (present-only): Present=0x%08X reason=0x%08X\n", i, (unsigned)hr, (unsigned)rr6);
            std::fflush(stdout);
            if (FAILED(rr6) || FAILED(hr)) break;
            ok++;
            Sleep(16);
            continue;
        }
        al->Reset(); cl->Reset(al, nullptr);
        const float c[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
        if (variant == 8) {
            // 完全不加屏障
            cl->ClearRenderTargetView(rtvh->GetCPUDescriptorHandleForHeapStart(), c, 0, nullptr);
        } else if (variant == 7 && i > 0) {
            // Present 之后按 COMMON 处理
            Barrier(cl, bb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            cl->ClearRenderTargetView(rtvh->GetCPUDescriptorHandleForHeapStart(), c, 0, nullptr);
            Barrier(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        } else {
            Barrier(cl, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            cl->ClearRenderTargetView(rtvh->GetCPUDescriptorHandleForHeapStart(), c, 0, nullptr);
            Barrier(cl, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
        cl->Close();
        ID3D12CommandList* ls[] = { cl };
        q->ExecuteCommandLists(1, ls);
        q->Signal(fe, ++v);
        while (fe->GetCompletedValue() < v) Sleep(0);
        {
            HRESULT rr5 = dev->GetDeviceRemovedReason();
            if (FAILED(rr5)) { std::printf("frame %d: 渲染提交后设备移除 0x%08X\n", i, (unsigned)rr5); break; }
        }

        doCopy();
        HRESULT rr = dev->GetDeviceRemovedReason();
        if (FAILED(rr)) { std::printf("frame %d: copy 后设备移除 0x%08X\n", i, (unsigned)rr); break; }

        hr = swap->Present(0, 0);
        rr = dev->GetDeviceRemovedReason();
        if (FAILED(rr)) { std::printf("frame %d: Present 后设备移除 0x%08X\n", i, (unsigned)rr); break; }
        if (FAILED(hr)) { std::printf("frame %d: Present 0x%08X\n", i, (unsigned)hr); break; }
        ok++;
        Sleep(16);
    }
    std::printf("RESULT variant=%d: %d/%d frames OK\n", variant, ok, frames);
    return ok == frames ? 0 : 1;
}
