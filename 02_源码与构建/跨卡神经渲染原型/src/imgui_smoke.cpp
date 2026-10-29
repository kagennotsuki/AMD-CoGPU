// ImGui + D3D12 冒烟测试：不碰游戏，先验证 ImGui 能在这台机器上完成
// CreateContext → 后端 Init → NewFrame → Render → RenderDrawData → 回读
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

using Microsoft::WRL::ComPtr;

static int g_fail = 0;
#define CHECK(x, msg) do { if (!(x)) { printf("[FAIL] %s\n", msg); ++g_fail; } else printf("[ ok ] %s\n", msg); } while (0)

int main() {
    printf("=== ImGui + D3D12 冒烟测试 ===\n");

    // 1) D3D12 基础对象
    ComPtr<IDXGIFactory6> fac;
    CHECK(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf()))), "创建 DXGI 工厂");
    ComPtr<IDXGIAdapter1> ad;
    CHECK(SUCCEEDED(fac->EnumAdapters1(0, ad.GetAddressOf())), "枚举适配器");
    DXGI_ADAPTER_DESC adesc{};
    if (ad) { ad->GetDesc(&adesc); printf("       适配器: %ls\n", adesc.Description); }

    ComPtr<ID3D12Device> dev;
    CHECK(SUCCEEDED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(dev.GetAddressOf()))), "创建 D3D12 设备");

    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> q;
    CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(q.GetAddressOf()))), "创建命令队列");
    ComPtr<ID3D12CommandAllocator> al;
    CHECK(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(al.GetAddressOf()))), "创建分配器");
    ComPtr<ID3D12GraphicsCommandList> cl;
    CHECK(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(cl.GetAddressOf()))), "创建命令列表");
    cl->Close();

    ComPtr<ID3D12Fence> fe;
    CHECK(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fe.GetAddressOf()))), "创建围栏");

    // 2) 渲染目标（和游戏一样用 R10G10B10A2）
    const UINT W = 640, H = 360;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R10G10B10A2_UNORM; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> rt;
    CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(rt.GetAddressOf()))), "创建渲染目标");

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 1;
    CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(rtvHeap.GetAddressOf()))), "创建 RTV 堆");
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt.Get(), nullptr, rtv);

    // 3) ImGui：这一步是上次崩溃的疑点
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();                      // ★ 上次漏了这句
    printf("[ ok ] ImGui::CreateContext\n");
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2((float)W, (float)H);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ComPtr<ID3D12DescriptorHeap> srvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 8;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(SUCCEEDED(dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(srvHeap.GetAddressOf()))), "创建 SRV 堆");

    bool initOk = ImGui_ImplDX12_Init(dev.Get(), 4, DXGI_FORMAT_R10G10B10A2_UNORM, srvHeap.Get(),
                                      srvHeap->GetCPUDescriptorHandleForHeapStart(),
                                      srvHeap->GetGPUDescriptorHandleForHeapStart());
    CHECK(initOk, "ImGui_ImplDX12_Init");

    // 4) 记录一帧 UI
    CHECK(SUCCEEDED(al->Reset()), "分配器 Reset");
    CHECK(SUCCEEDED(cl->Reset(al.Get(), nullptr)), "命令列表 Reset");

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20, 20));
    ImGui::SetNextWindowSize(ImVec2(300, 120));
    ImGui::Begin("smoke", nullptr, ImGuiWindowFlags_NoSavedSettings);
    static float v = 0.66f;
    ImGui::SliderFloat("gain", &v, 0.0f, 2.0f);
    ImGui::Text("hello imgu");
    ImGui::End();
    ImGui::Render();
    printf("[ ok ] NewFrame/Render（绘制列表 %d 个）\n", ImGui::GetDrawData()->CmdListsCount);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->ResourceBarrier(1, &b);

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_RECT sc{ 0, 0, (LONG)W, (LONG)H };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl.Get());
    printf("[ ok ] ImGui_ImplDX12_RenderDrawData\n");

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    // 5) 回读到 READBACK 缓冲，确认真的画上了东西
    ComPtr<ID3D12Resource> rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rs = 0, total = 0;
    D3D12_RESOURCE_DESC pl = td; pl.Flags = D3D12_RESOURCE_FLAG_NONE;
    dev->GetCopyableFootprints(&pl, 0, 1, 0, &fp, &rows, &rs, &total);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp2{}; hp2.Type = D3D12_HEAP_TYPE_READBACK;
    hp2.CreationNodeMask = 1; hp2.VisibleNodeMask = 1;
    CHECK(SUCCEEDED(dev->CreateCommittedResource(&hp2, D3D12_HEAP_FLAG_NONE, &bd,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb.GetAddressOf()))), "创建回读缓冲");

    D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = rb.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = rt.Get();
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
    cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    CHECK(SUCCEEDED(cl->Close()), "命令列表 Close");
    ID3D12CommandList* lists[] = { cl.Get() };
    q->ExecuteCommandLists(1, lists);
    q->Signal(fe.Get(), 1);
    while (fe->GetCompletedValue() < 1) Sleep(0);

    void* m = nullptr;
    D3D12_RANGE rr{ 0, (SIZE_T)total };
    if (SUCCEEDED(rb->Map(0, &rr, &m)) && m) {
        const unsigned* px = (const unsigned*)((unsigned char*)m + fp.Footprint.RowPitch * 3);
        int nonzero = 0;
        for (UINT x = 0; x < 200; ++x) if ((px[x] & 0x3FFu) != 0) ++nonzero;
        printf("       第 3 行前 200 像素中非黑数量 = %d\n", nonzero);
        CHECK(nonzero > 0, "ImGui 确实画出了内容");
        D3D12_RANGE nw{ 0, 0 };
        rb->Unmap(0, &nw);
    } else {
        printf("[FAIL] 回读 Map 失败\n"); ++g_fail;
    }

    ImGui_ImplDX12_Shutdown();
    ImGui::DestroyContext();
    printf("\n=== 结果: %s（失败项 %d）===\n", g_fail == 0 ? "全部通过" : "有失败", g_fail);
    return g_fail ? 1 : 0;
}
