// queue_overlap_probe.cpp —— 在 7900 XT 上量"并行"到底能拿到多少
// -----------------------------------------------------------------------------
// 两代实验放在一起：
//   【A 组】同类负载：N 条 compute 队列各跑一份 ALU 密集负载 → 重叠比 ≈ n（=时间片）
//   【B 组】不同单元：ALU 密集 ∥ 纹理采样密集、ALU ∥ 拷贝引擎
//           → 重叠比 < 1 才有真收益（1.0 = 完全串行，等于没并行）
//
// 判读：
//   · A 组已经证明：RDNA 的两条 ACE 队列共享同一批 SM，吃满时只能轮转。
//   · B 组回答本题：换"工作类型"能不能让 SM 的不同子单元同时忙。
//
// 构建：cmake --build build --target queue_overlap_probe --config Release
// 注意：**必须在没有 dxgi.dll / d3d12.dll 代理的目录运行**，否则会被劫持。
// -----------------------------------------------------------------------------
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>

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

// ---------- 内核 A：纯 ALU（FMA 依赖链，不会被优化掉） ----------
static const char* kHLSLAlu = R"(
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

// ---------- 内核 B：纹理采样密集（打纹理单元/采样器） ----------
static const char* kHLSLTex = R"(
Texture2D<float4> gTex : register(t0);
SamplerState      gSmp : register(s0);
RWStructuredBuffer<float4> gBuf : register(u0);
cbuffer P : register(b0) { uint gThreads; uint gIters; };
[numthreads(256,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= gThreads) return;
    float2 uv = float2((i % 3840u) / 3840.0f, ((i / 3840u) % 2160u) / 2160.0f);
    float4 acc = float4(0,0,0,0);
    for (uint k = 0; k < gIters; ++k) {
        uv += float2(1e-6f, 1e-6f);
        acc += gTex.SampleLevel(gSmp, uv, 0);
    }
    gBuf[i] = acc;
}
)";

struct Q {
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;   // ★ 每条队列独立 fence（共用会让大值先完成时"跳过"别的队列）
    UINT64 fv = 0;
};

struct Ctx {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12RootSignature> rsAlu, rsTex;
    ComPtr<ID3D12PipelineState> psoAlu, psoTex;
    ComPtr<ID3D12Resource> buf, tex, bufA, bufB;   // buf = 工作缓冲；tex = 采样源；bufA/B = 拷贝用
    ComPtr<ID3D12DescriptorHeap> heapS;            // shader visible：0=UAV, 1=SRV
    ComPtr<ID3D12Fence> fence;
    UINT64 fv = 0;
    bool Init();
};

static bool MakeQ(Ctx& C, Q& o, D3D12_COMMAND_LIST_TYPE type) {
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = type;
    if (FAILED(C.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&o.q)))) return false;
    if (FAILED(C.dev->CreateCommandAllocator(type, IID_PPV_ARGS(&o.alloc)))) return false;
    if (FAILED(C.dev->CreateCommandList(0, type, o.alloc.Get(), nullptr, IID_PPV_ARGS(&o.cl)))) return false;
    if (FAILED(C.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&o.fence)))) return false;
    o.cl->Close();
    return true;
}

// 造根签名：UAV 表 + 32bit 常量；wantSrv 时再加一张 SRV 表 + 静态采样器
static bool MakeRS(Ctx& C, bool wantSrv, ID3D12RootSignature** out) {
    D3D12_DESCRIPTOR_RANGE rgU{};
    rgU.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rgU.NumDescriptors = 1; rgU.BaseShaderRegister = 0; rgU.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE rgS{};
    rgS.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rgS.NumDescriptors = 1; rgS.BaseShaderRegister = 0; rgS.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &rgU;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 0; rp[1].Constants.Num32BitValues = 2;
    UINT n = 2;
    D3D12_STATIC_SAMPLER_DESC ss[1]{};
    if (wantSrv) {
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable.NumDescriptorRanges = 1;
        rp[2].DescriptorTable.pDescriptorRanges = &rgS;
        n = 3;
        ss[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        ss[0].AddressU = ss[0].AddressV = ss[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        ss[0].MaxLOD = D3D12_FLOAT32_MAX;
        ss[0].ShaderRegister = 0;
        ss[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC d{};
    d.NumParameters = n; d.pParameters = rp;
    d.NumStaticSamplers = wantSrv ? 1u : 0u; d.pStaticSamplers = wantSrv ? ss : nullptr;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Log("序列化根签名失败: %s\n", err ? (char*)err->GetBufferPointer() : "?"); return false;
    }
    if (FAILED(C.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(out)))) {
        Log("建根签名失败\n"); return false;
    }
    return true;
}

static bool MakePSO(Ctx& C, const char* hlsl, ID3D12RootSignature* rs, ID3D12PipelineState** out) {
    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(hlsl, strlen(hlsl), "probe.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("编译失败: %s\n", e2 ? (char*)e2->GetBufferPointer() : "?"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs;
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    return SUCCEEDED(C.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(out)));
}

bool Ctx::Init() {
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { Log("工厂失败（当前目录有 dxgi 代理？）\n"); return false; }
    ComPtr<IDXGIAdapter1> best; SIZE_T bestMem = 0;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) continue;
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (d.DedicatedVideoMemory > bestMem) { bestMem = d.DedicatedVideoMemory; best = a; }
    }
    if (!best) { Log("找不到硬件适配器\n"); return false; }
    DXGI_ADAPTER_DESC1 d{};
    best->GetDesc1(&d);
    Log("适配器: %ls  %.1f GB\n", d.Description, (double)d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0));
    if (FAILED(D3D12CreateDevice(best.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) { Log("建设备失败\n"); return false; }

    if (!MakeRS(*this, false, &rsAlu)) return false;
    if (!MakeRS(*this, true, &rsTex)) return false;
    if (!MakePSO(*this, kHLSLAlu, rsAlu.Get(), &psoAlu)) { Log("ALU PSO 失败\n"); return false; }
    if (!MakePSO(*this, kHLSLTex, rsTex.Get(), &psoTex)) { Log("TEX PSO 失败\n"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heapS)))) { Log("建堆失败\n"); return false; }
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h0 = heapS->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = h0; h1.ptr += inc;

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    const UINT64 bytes = 32ull * 1024ull * 1024ull;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&buf)))) { Log("建 buffer 失败\n"); return false; }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = (UINT)(bytes / 16);
    ud.Buffer.StructureByteStride = 16;
    dev->CreateUnorderedAccessView(buf.Get(), nullptr, &ud, h0);

    // 4K 采样源
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 3840; td.Height = 2160; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&tex)))) { Log("建 texture 失败\n"); return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(tex.Get(), &sd, h1);

    // 拷贝对照用的两块大 buffer
    D3D12_RESOURCE_DESC bd = rd; bd.Flags = D3D12_RESOURCE_FLAG_NONE; bd.Width = 64ull * 1024 * 1024;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&bufA)))) { Log("建 bufA 失败\n"); return false; }
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&bufB)))) { Log("建 bufB 失败\n"); return false; }

    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    return true;
}

static void RecordAlu(Ctx& C, Q& o, UINT threads, UINT iters) {
    o.alloc->Reset(); o.cl->Reset(o.alloc.Get(), nullptr);
    ID3D12DescriptorHeap* hs[] = { C.heapS.Get() };
    o.cl->SetDescriptorHeaps(1, hs);
    o.cl->SetComputeRootSignature(C.rsAlu.Get());
    o.cl->SetComputeRootDescriptorTable(0, C.heapS->GetGPUDescriptorHandleForHeapStart());
    o.cl->SetPipelineState(C.psoAlu.Get());
    UINT c[2] = { threads, iters };
    o.cl->SetComputeRoot32BitConstants(1, 2, c, 0);
    o.cl->Dispatch((threads + 255) / 256, 1, 1);
    o.cl->Close();
}

static void RecordTex(Ctx& C, Q& o, UINT threads, UINT iters) {
    o.alloc->Reset(); o.cl->Reset(o.alloc.Get(), nullptr);
    ID3D12DescriptorHeap* hs[] = { C.heapS.Get() };
    o.cl->SetDescriptorHeaps(1, hs);
    o.cl->SetComputeRootSignature(C.rsTex.Get());
    o.cl->SetComputeRootDescriptorTable(0, C.heapS->GetGPUDescriptorHandleForHeapStart());
    D3D12_GPU_DESCRIPTOR_HANDLE s = C.heapS->GetGPUDescriptorHandleForHeapStart();
    s.ptr += C.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    o.cl->SetComputeRootDescriptorTable(2, s);
    o.cl->SetPipelineState(C.psoTex.Get());
    UINT c[2] = { threads, iters };
    o.cl->SetComputeRoot32BitConstants(1, 2, c, 0);
    o.cl->Dispatch((threads + 255) / 256, 1, 1);
    o.cl->Close();
}

static void RecordCopy(Ctx& C, Q& o) {
    o.alloc->Reset(); o.cl->Reset(o.alloc.Get(), nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = C.bufA.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    o.cl->ResourceBarrier(1, &b);
    b.Transition.pResource = C.bufB.Get();
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    o.cl->ResourceBarrier(1, &b);
    o.cl->CopyResource(C.bufB.Get(), C.bufA.Get());
    b.Transition.pResource = C.bufA.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    o.cl->ResourceBarrier(1, &b);
    b.Transition.pResource = C.bufB.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    o.cl->ResourceBarrier(1, &b);
    o.cl->Close();
}

// 依次跑 n 个队列，计"第一次提交 → 全部完成"；每条队列用**自己的** fence 等待
static double Run(Ctx& C, const std::vector<Q*>& qs) {
    double t0 = NowMs();
    for (Q* o : qs) {
        ID3D12CommandList* ls[] = { o->cl.Get() };
        o->q->ExecuteCommandLists(1, ls);
        o->q->Signal(o->fence.Get(), ++o->fv);
    }
    for (Q* o : qs) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        o->fence->SetEventOnCompletion(o->fv, ev);
        WaitForSingleObject(ev, 60000);
        CloseHandle(ev);
    }
    return NowMs() - t0;
}

static double Avg3(Ctx& C, const char* tag, const std::vector<Q*>& qs) {
    double s = 0;
    for (int i = 0; i < 3; ++i) s += Run(C, qs);
    double a = s / 3.0;
    Log("  %-22s = %6.2f ms\n", tag, a);
    return a;
}

int main() {
    Ctx C;
    if (!C.Init()) return 1;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    Q qc0, qc1, qc2, qcopy;
    if (!MakeQ(C, qc0, D3D12_COMMAND_LIST_TYPE_COMPUTE)) return 1;
    if (!MakeQ(C, qc1, D3D12_COMMAND_LIST_TYPE_COMPUTE)) return 1;
    if (!MakeQ(C, qc2, D3D12_COMMAND_LIST_TYPE_COMPUTE)) return 1;
    if (!MakeQ(C, qcopy, D3D12_COMMAND_LIST_TYPE_COPY)) return 1;
    Log("队列就绪\n\n");

    const UINT threads = 1u << 20;
    // 先把两种负载各自校准到 ~3ms
    UINT ia = 200, it = 100;
    for (int r = 0; r < 12; ++r) {
        RecordAlu(C, qc0, threads, ia);
        double t = Run(C, { &qc0 });
        if (t >= 3.0) break;
        ia = (UINT)(ia * (t < 0.8 ? 4.0 : (t < 2.0 ? 2.0 : 1.4)) + 1);
    }
    for (int r = 0; r < 12; ++r) {
        RecordTex(C, qc1, threads, it);
        double t = Run(C, { &qc1 });
        if (t >= 3.0) break;
        it = (UINT)(it * (t < 0.8 ? 4.0 : (t < 2.0 ? 2.0 : 1.4)) + 1);
    }

    Log("== 单独跑 ==\n");
    RecordAlu(C, qc0, threads, ia);   double Talu  = Avg3(C, "ALU 密集", { &qc0 });
    RecordTex(C, qc1, threads, it);   double Ttex  = Avg3(C, "纹理采样密集", { &qc1 });
    RecordCopy(C, qcopy);             double Tcopy = Avg3(C, "拷贝 64MB", { &qcopy });

    Log("\n== 并发（不同工作类型）==\n");
    RecordAlu(C, qc0, threads, ia);
    RecordTex(C, qc1, threads, it);
    double T_alu_tex = Avg3(C, "ALU ∥ 纹理", { &qc0, &qc1 });
    Log("    → 重叠比 T/(Talu+Ttex) = %.2f   （1.00 = 完全串行；<1 = 有重叠）\n", T_alu_tex / (Talu + Ttex));

    RecordAlu(C, qc0, threads, ia);
    RecordCopy(C, qcopy);
    double T_alu_copy = Avg3(C, "ALU ∥ 拷贝", { &qc0, &qcopy });
    Log("    → 重叠比 T/(Talu+Tcopy) = %.2f  （对照组：不同引擎，应显著 <1）\n", T_alu_copy / (Talu + Tcopy));

    RecordTex(C, qc1, threads, it);
    RecordCopy(C, qcopy);
    double T_tex_copy = Avg3(C, "纹理 ∥ 拷贝", { &qc1, &qcopy });
    Log("    → 重叠比 T/(Ttex+Tcopy) = %.2f\n", T_tex_copy / (Ttex + Tcopy));

    Log("\n判读：\n");
    Log("  · ALU ∥ 纹理 明显 <1  ⇒ 换工作类型能让 SM 子单元同时忙（NR/上采样可试）\n");
    Log("  · ALU ∥ 纹理 ≈1      ⇒ 同批 SM 只能轮转，并行只能靠「不同卡」\n");
    Log("  · ALU ∥ 拷贝 应 <1    ⇒ 若也 ≈1，说明本探针测不出重叠，结论不可信\n");
    return 0;
}
