// pcie_bandwidth_probe.cpp —— 跨卡（PCIe）搬数据的真实带宽与抖动
// -----------------------------------------------------------------------------
// 目的：新形态（B 形态）每帧要搬 色+MV+深度 ≈ 44MB。同卡内那 33MB 实测是免费的，
//       但跨卡要走 PCIe —— 有效带宽、特别是 p99 抖动，是 B 形态的生死判据。
//
// 方法：两张卡各建一个 D3D12 设备，用跨适配器共享堆（buffer）连起来：
//         A: localA --copy--> sharedA          （A 的队列）
//         B: sharedB --copy--> localB          （B 的队列）
//       两种模式：
//         · 串行   ：A 提交→等完→B 提交→等完（最坏情况，相当于每帧两段不重叠）
//         · 流水   ：A、B 连续提交，最后一起等（允许两段重叠）
//       每个尺寸跑 12 次，报告 均值 / p99 / 最大。
//
// 注意：跨卡没有 fence（已知限制），所以用 CPU 等待做同步；这正好量出"每帧的墙钟成本"。
//       必须在没有 dxgi.dll / d3d12.dll 代理的目录运行。
// -----------------------------------------------------------------------------
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

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

struct Side {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> local;    // 本卡的普通 buffer
    UINT64 fv = 0;
    std::wstring name;
};

static bool MakeSide(Side& s, IDXGIAdapter1* ad, UINT64 bytes, const wchar_t* nm) {
    DXGI_ADAPTER_DESC1 d{};
    ad->GetDesc1(&d);
    s.name = nm;
    if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.dev)))) {
        Log("[%ls] 建设备失败\n", nm); return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(s.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.q)))) return false;
    if (FAILED(s.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&s.alloc)))) return false;
    if (FAILED(s.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, s.alloc.Get(), nullptr, IID_PPV_ARGS(&s.cl)))) return false;
    if (FAILED(s.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)))) return false;
    s.cl->Close();

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.local)))) {
        Log("[%ls] 建本地 buffer 失败\n", nm); return false;
    }
    return true;
}

int main() {
    const UINT64 MAXB = 64ull * 1024 * 1024;
    const UINT64 ALIGN = 65536;
    const UINT64 SZ = ((MAXB + ALIGN - 1) / ALIGN) * ALIGN;

    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { Log("工厂失败（目录里有 dxgi 代理？）\n"); return 1; }

    // 按 DeviceId 找两张卡：0x744C = 7900 XT(游戏卡)，0x7550 = 9070 XT(NR 卡)
    ComPtr<IDXGIAdapter1> adA, adB;
    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (fac->EnumAdapters1(i, &a) != S_OK) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) continue;
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        Log("适配器[%u] %ls DeviceId=0x%04X LUID=%08X:%08X\n", i, d.Description, d.DeviceId,
            d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
        if (d.DeviceId == 0x744C && !adA) adA = a;
        if (d.DeviceId == 0x7550 && !adB) adB = a;
    }
    if (!adA || !adB) { Log("找不到两张卡（需要 0x744C 与 0x7550）\n"); return 1; }

    Side A, B;
    if (!MakeSide(A, adA.Get(), SZ, L"7900XT")) return 1;
    if (!MakeSide(B, adB.Get(), SZ, L"9070XT")) return 1;

    // 跨适配器共享堆（照抄代理里已验证的写法）
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = SZ;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Properties.CreationNodeMask = 1;
    hd.Properties.VisibleNodeMask = 1;
    hd.Alignment = ALIGN;
    hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
    ComPtr<ID3D12Heap> heapA, heapB;
    HRESULT hr = A.dev->CreateHeap(&hd, IID_PPV_ARGS(&heapA));
    if (FAILED(hr)) { Log("建共享堆失败 0x%08X\n", (unsigned)hr); return 1; }

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = SZ; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

    ComPtr<ID3D12Resource> shA, shB;
    hr = A.dev->CreatePlacedResource(heapA.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&shA));
    if (FAILED(hr)) { Log("A 侧 placed 失败 0x%08X\n", (unsigned)hr); return 1; }
    HANDLE sh = nullptr;
    hr = A.dev->CreateSharedHandle(heapA.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
    if (FAILED(hr)) { Log("导出共享句柄失败 0x%08X\n", (unsigned)hr); return 1; }
    hr = B.dev->OpenSharedHandle(sh, IID_PPV_ARGS(&heapB));
    CloseHandle(sh);
    if (FAILED(hr)) { Log("B 打开共享堆失败 0x%08X\n", (unsigned)hr); return 1; }
    hr = B.dev->CreatePlacedResource(heapB.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&shB));
    if (FAILED(hr)) { Log("B 侧 placed 失败 0x%08X\n", (unsigned)hr); return 1; }
    Log("跨卡共享堆就绪: %.1f MB\n", (double)SZ / (1024.0 * 1024.0));

    // ---- 记录一次搬运：A: localA→shA ，B: shB→localB ----
    auto recA = [&](UINT64 bytes) {
        A.alloc->Reset();
        A.cl->Reset(A.alloc.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = A.local.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        A.cl->ResourceBarrier(1, &b);
        b.Transition.pResource = shA.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        A.cl->ResourceBarrier(1, &b);
        A.cl->CopyBufferRegion(shA.Get(), 0, A.local.Get(), 0, bytes);
        b.Transition.pResource = A.local.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        A.cl->ResourceBarrier(1, &b);
        b.Transition.pResource = shA.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        A.cl->ResourceBarrier(1, &b);
        A.cl->Close();
    };
    auto recB = [&](UINT64 bytes) {
        B.alloc->Reset();
        B.cl->Reset(B.alloc.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = shB.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        B.cl->ResourceBarrier(1, &b);
        b.Transition.pResource = B.local.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        B.cl->ResourceBarrier(1, &b);
        B.cl->CopyBufferRegion(B.local.Get(), 0, shB.Get(), 0, bytes);
        b.Transition.pResource = B.local.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        B.cl->ResourceBarrier(1, &b);
        b.Transition.pResource = shB.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        B.cl->ResourceBarrier(1, &b);
        B.cl->Close();
    };
    auto waitSide = [&](Side& s) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s.fence->SetEventOnCompletion(s.fv, ev);
        WaitForSingleObject(ev, 20000);
        CloseHandle(ev);
    };

    // 一种模式跑 N 次，返回耗时数组
    auto run = [&](UINT64 bytes, bool pipelined, int n, std::vector<double>& out) {
        out.clear();
        for (int i = 0; i < n; ++i) {
            recA(bytes); recB(bytes);
            double t0 = NowMs();
            if (pipelined) {
                ID3D12CommandList* la[] = { A.cl.Get() };
                A.q->ExecuteCommandLists(1, la);
                A.q->Signal(A.fence.Get(), ++A.fv);
                ID3D12CommandList* lb[] = { B.cl.Get() };
                B.q->ExecuteCommandLists(1, lb);
                B.q->Signal(B.fence.Get(), ++B.fv);
                waitSide(A); waitSide(B);
            } else {
                ID3D12CommandList* la[] = { A.cl.Get() };
                A.q->ExecuteCommandLists(1, la);
                A.q->Signal(A.fence.Get(), ++A.fv);
                waitSide(A);
                ID3D12CommandList* lb[] = { B.cl.Get() };
                B.q->ExecuteCommandLists(1, lb);
                B.q->Signal(B.fence.Get(), ++B.fv);
                waitSide(B);
            }
            out.push_back(NowMs() - t0);
        }
    };

    Log("\n=== 跨卡搬运：A(7900XT) -> 共享堆 -> B(9070XT) ===\n");
    Log("size    | mode | mean_ms | p99_ms | max_ms | eff_GB/s | 60fps预算占比\n");
    Log("--------+------+---------+--------+--------+----------+------------\n");
    const UINT64 sizes[] = { 8ull << 20, 16ull << 20, 33ull << 20, 44ull << 20, 64ull << 20 };
    for (UINT64 bytes : sizes) {
        for (int mode = 0; mode < 2; ++mode) {
            std::vector<double> t;
            run(bytes, mode == 1, 12, t);
            std::vector<double> st = t;
            std::sort(st.begin(), st.end());
            double sum = 0; for (double v : st) sum += v;
            double mean = sum / st.size();
            double p99 = st[(size_t)((st.size() - 1) * 0.99)];
            double mx = st.back();
            double gbs = (double)bytes / 1e6 / mean * 1000.0;
            Log("%5.1fMB | %s | %7.2f | %6.2f | %6.2f | %8.2f | %5.1f%%\n",
                (double)bytes / (1024.0 * 1024.0), mode ? "流水" : "串行",
                mean, p99, mx, gbs, mean / 16.667 * 100.0);
        }
    }

    Log("\n判读：\n");
    Log("  · 44MB 的 mean 若 < 4ms 且 p99 不飙（<8ms），跨卡搬运才可能藏在帧里\n");
    Log("  · 60fps 每帧预算 16.667ms，搬运吃掉多少百分比直接看最后一列\n");
    Log("  · 这里没有跨卡 fence，用 CPU 等待同步 —— 也就是『每帧最少要等的墙钟』\n");
    return 0;
}
