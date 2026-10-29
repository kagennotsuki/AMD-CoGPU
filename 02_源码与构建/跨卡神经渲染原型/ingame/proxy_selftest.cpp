// 代理自检 v2 —— 忠实模拟一帧游戏：
//   取后备缓冲 → PRESENT→RENDER_TARGET → Clear → RENDER_TARGET→PRESENT
//   → [可选] 应用侧做同样的拷贝 → Present
//
// 用法：proxy_selftest.exe <游戏目录> [帧数]

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <cstdio>
#include <string>

using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: proxy_selftest.exe <game_dir> [frames]\n"); return 1; }
    std::string a = argv[1];
    std::wstring dir(a.begin(), a.end());
    int frames = (argc > 2) ? atoi(argv[2]) : 20;
    int appCopy = (argc > 3) ? atoi(argv[3]) : 0;

    HMODULE d3dMod = LoadLibraryW((dir + L"\\d3d12.dll").c_str());
    if (!d3dMod) { std::printf("load d3d12 proxy failed %lu\n", GetLastError()); return 1; }
    auto createDevice = (CreateDeviceFn)GetProcAddress(d3dMod, "D3D12CreateDevice");
    HMODULE dxgiMod = LoadLibraryW((dir + L"\\dxgi.dll").c_str());
    if (!dxgiMod) { std::printf("load dxgi proxy failed %lu\n", GetLastError()); return 1; }
    auto createFactory2 = (CreateFactory2Fn)GetProcAddress(dxgiMod, "CreateDXGIFactory2");
    if (!createDevice || !createFactory2) { std::printf("missing exports\n"); return 1; }

    IDXGIFactory2* factory = nullptr;
    if (FAILED(createFactory2(0, IID_PPV_ARGS(&factory)))) return 1;
    IDXGIAdapter1* adapter = nullptr;
    factory->EnumAdapters1(0, &adapter);

    ID3D12Device* device = nullptr;
    HRESULT hr = createDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    std::printf("D3D12CreateDevice -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{};
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return 1;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ProxySelfTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"ProxySelfTest", L"t", WS_OVERLAPPEDWINDOW,
                                0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = 640; scd.Height = 480;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap = nullptr;
    hr = factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &swap);
    std::printf("CreateSwapChainForHwnd -> 0x%08X\n", (unsigned)hr);
    if (FAILED(hr)) return 1;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)))) return 1;
    ID3D12Resource* backbuffer = nullptr;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) return 1;
    device->CreateRenderTargetView(backbuffer, nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());

    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fv = 0;

    int ok = 0;
    ID3D12Resource* prevBb = nullptr;
    for (int i = 0; i < frames; ++i) {
        // 每帧重新取后备缓冲：flip 模型下指针可能变化
        ID3D12Resource* curBb = nullptr;
        if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&curBb)))) { std::printf("GetBuffer failed\n"); break; }
        if (curBb != backbuffer) {
            std::printf("frame %d: backbuffer changed %p -> %p\n", i, (void*)backbuffer, (void*)curBb);
            if (prevBb) prevBb->Release();
            prevBb = backbuffer;
            backbuffer = curBb;
            device->CreateRenderTargetView(backbuffer, nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
        } else {
            curBb->Release();
        }
        alloc->Reset();
        list->Reset(alloc, nullptr);
        Barrier(list, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float color[4] = { 0.15f + 0.02f * i, 0.35f, 0.55f, 1.0f };
        list->ClearRenderTargetView(rtvHeap->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
        Barrier(list, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->Close();
        ID3D12CommandList* l[] = { list };
        queue->ExecuteCommandLists(1, l);
        queue->Signal(fence, ++fv);
        while (fence->GetCompletedValue() < fv) Sleep(0);

        // 对照实验：应用侧做与代理完全相同的拷贝
        if (appCopy) {
            static ID3D12Resource* rb = nullptr;
            static D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            static UINT64 total = 0;
            if (!rb) {
                D3D12_RESOURCE_DESC texDesc = backbuffer->GetDesc();
                texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
                UINT rows = 0; UINT64 rowSize = 0;
                device->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, &rows, &rowSize, &total);
                D3D12_HEAP_PROPERTIES rbHeap{};
                rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
                rbHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                rbHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                rbHeap.CreationNodeMask = 1;
                rbHeap.VisibleNodeMask = 1;
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
                bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bd,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) {
                    std::printf("app-copy: readback create failed\n");
                    appCopy = 0;
                } else {
                    std::printf("app-copy: readback %llu bytes rowPitch %u\n",
                                (unsigned long long)total, fp.Footprint.RowPitch);
                }
            }
            if (rb && appCopy) {
                alloc->Reset();
                list->Reset(alloc, nullptr);
                Barrier(list, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = rb;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = fp;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = backbuffer;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                Barrier(list, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
                list->Close();
                ID3D12CommandList* l2[] = { list };
                queue->ExecuteCommandLists(1, l2);
                queue->Signal(fence, ++fv);
                while (fence->GetCompletedValue() < fv) Sleep(0);
                HRESULT rr2 = device->GetDeviceRemovedReason();
                if (FAILED(rr2)) { std::printf("app-copy #%d: DEVICE REMOVED 0x%08X\n", i, (unsigned)rr2); break; }
                if (i == 0) std::printf("app-copy #0: OK\n");
            }
        }

        hr = swap->Present(0, 0);
        if (FAILED(hr)) {
            std::printf("Present #%d -> 0x%08X (FAILED)\n", i, (unsigned)hr);
            break;
        }
        ok++;
        Sleep(16);
    }
    std::printf("Present 成功 %d / %d\n", ok, frames);

    HRESULT reason = device->GetDeviceRemovedReason();
    std::printf("GetDeviceRemovedReason -> 0x%08X\n", (unsigned)reason);

    swap->Release(); backbuffer->Release(); rtvHeap->Release();
    list->Release(); alloc->Release(); fence->Release(); queue->Release();
    device->Release(); adapter->Release(); factory->Release();
    FreeLibrary(dxgiMod); FreeLibrary(d3dMod);
    return (ok == frames && SUCCEEDED(reason)) ? 0 : 1;
}
