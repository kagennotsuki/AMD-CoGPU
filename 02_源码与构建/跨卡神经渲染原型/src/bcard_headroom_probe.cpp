// bcard_headroom_probe.cpp —— B 卡（NR 那张）还剩多少算力？
// -----------------------------------------------------------------------------
// 目的：NR(mochizuki) 在 B 卡上跑的时候，再加一份计算负载，看 NR 的 network ms
//       从哪一档开始变慢 —— 那个点就是"剩余能力"。用来判断 FSR4 能否与 NR 并行。
//
// 做法：在 B 卡（按 DeviceId 选，默认 0x7550 = 9070 XT）上建独立 D3D12 计算队列，
//       按档位持续灌计算负载：档 0 = 空转，1..4 逐档加大。
//       每档 12 秒，两轮。当前档位写到同目录的 bcard_probe_level.txt。
//
// 用法：bcard_headroom_probe.exe           （自动两轮）
//       bcard_headroom_probe.exe 0x7550    （指定 DeviceId）
// 注意：必须在没有 dxgi.dll / d3d12.dll 代理的目录运行。
// -----------------------------------------------------------------------------
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

static void Log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fflush(stdout);
}
static double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static const char* kHLSL = R"(
RWStructuredBuffer<float4> gBuf : register(u0);
cbuffer P : register(b0) { uint gThreads; uint gIters; };
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= gThreads) return;
    float4 v = gBuf[i];
    for (uint k = 0; k < gIters; ++k)
        v = v * 1.0000001f + float4(1e-7f, 2e-7f, 3e-7f, 1e-7f);
    gBuf[i] = v;
}
)";

static std::string g_levelPath;

static void SetLevel(int lv, double t) {
    FILE* f = nullptr;
    if (fopen_s(&f, g_levelPath.c_str(), "w") == 0 && f) {
        fprintf(f, "%d %.3f\n", lv, t);
        fclose(f);
    }
}

int main(int argc, char** argv) {
    UINT wantDevId = 0x7550;                       // 9070 XT
    if (argc > 1) { UINT v = 0; if (sscanf_s(argv[1], "0x%X", &v) == 1) wantDevId = v; }

    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t s = dir.find_last_of("\\/");
    if (s != std::string::npos) dir = dir.substr(0, s + 1);
    g_levelPath = dir + "bcard_probe_level.txt";

    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { Log("工厂失败（目录里有 dxgi 代理？）\n"); return 1; }

    ComPtr<IDXGIAdapter1> target;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) continue;
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        Log("适配器[%u] %ls  DeviceId=0x%04X  LUID=%08X:%08X  %.1fGB\n", i, d.Description,
            d.DeviceId, d.AdapterLuid.HighPart, d.AdapterLuid.LowPart,
            (double)d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0));
        if (d.DeviceId == wantDevId && !target) target = a;
    }
    if (!target) { Log("找不到 DeviceId=0x%04X 的适配器\n", wantDevId); return 1; }

    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(target.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        Log("D3D12CreateDevice 失败\n"); return 1;
    }

    D3D12_DESCRIPTOR_RANGE rg{};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rg.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &rg;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 0;
    rp[1].Constants.Num32BitValues = 2;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2; rsd.pParameters = rp;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Log("根签名失败: %s\n", err ? (char*)err->GetBufferPointer() : "?"); return 1;
    }
    ComPtr<ID3D12RootSignature> rs;
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&rs)))) return 1;

    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "probe.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("编译失败: %s\n", e2 ? (char*)e2->GetBufferPointer() : "?"); return 1;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { Log("PSO 失败\n"); return 1; }

    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return 1;

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 16ull * 1024 * 1024; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> buf;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&buf)))) { Log("建 buffer 失败\n"); return 1; }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = (UINT)(rd.Width / 16);
    ud.Buffer.StructureByteStride = 16;
    dev->CreateUnorderedAccessView(buf.Get(), nullptr, &ud, heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) return 1;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc)))) return 1;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc.Get(), nullptr, IID_PPV_ARGS(&cl)))) return 1;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 1;
    cl->Close();

    Log("\n开始：档 0..4，每档 14 秒，两轮（共约 140 秒）\n");
    Log("档位写到: %s\n\n", g_levelPath.c_str());

    const UINT iters = 20000;
    UINT64 fv = 0;
    for (int round = 0; round < 2; ++round) {
        for (int lv = 0; lv <= 4; ++lv) {
            UINT threads = (UINT)lv * 262144u;         // 0 / 256K / 512K / 768K / 1M
            SetLevel(lv, NowMs());
            Log("[轮%d] 档 %d  threads=%u  开始\n", round + 1, lv, threads);
            double t0 = NowMs();
            if (lv == 0) {
                // 空转：只保持档位，不发 dispatch
                while (NowMs() - t0 < 14000) Sleep(200);
            } else {
                while (NowMs() - t0 < 14000) {
                    alloc->Reset();
                    cl->Reset(alloc.Get(), nullptr);
                    ID3D12DescriptorHeap* hs[] = { heap.Get() };
                    cl->SetDescriptorHeaps(1, hs);
                    cl->SetComputeRootSignature(rs.Get());
                    cl->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
                    cl->SetPipelineState(pso.Get());
                    UINT c[2] = { threads, iters };
                    cl->SetComputeRoot32BitConstants(1, 2, c, 0);
                    cl->Dispatch((threads + 255) / 256, 1, 1);
                    cl->Close();
                    ID3D12CommandList* ls[] = { cl.Get() };
                    q->ExecuteCommandLists(1, ls);
                    q->Signal(fence.Get(), ++fv);
                    // 最多领先 8 帧，不阻塞
                    while (fv - fence->GetCompletedValue() > 8) Sleep(1);
                }
            }
            // 本档结束，等清空
            while (fence->GetCompletedValue() < fv) Sleep(1);
        }
    }
    SetLevel(9, NowMs());    // 9 = 结束
    Log("\n完成。\n");
    return 0;
}
