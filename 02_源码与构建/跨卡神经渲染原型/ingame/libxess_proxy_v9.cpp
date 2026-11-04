// libxess.dll 代理 v9 —— 方案 B：槽位队列 + 延迟呈现
//
// 目标：让整条链（NR + 上采样器）按 NR 的节奏跑，宁可变慢，也不丢帧、不错位。
//
// 每帧流程（K=4 槽位，延迟 3 帧）：
//   ① [CPU] 阻塞等待 slot[N-3] 的答案（无条件等待）
//   ② [CPU] Map 上一帧记录的降采样回读 → 喂 worker（对应 slot[N-1]）
//   ③ [GPU] 把本帧原始 pColor / MV / depth 拷进 slot[N]
//   ④ [GPU] 把 slot[N-1] 的 color 降采样到 1080p → 回读到缓冲
//   ⑤ [GPU] 把 slot[N-3] 的残差叠回它的原图 → g_pre → 写进 pColor
//   ⑥ 调真 XeSS，传入 slot[N-3] 的 MV/depth/jitter（关键：让上采样器看到自洽序列）
//
// 残差处理参考 AMDNR：限幅 0.2 / 边缘保护 0.6 / 输出平滑 0.8

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <tlhelp32.h>
#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include <wrl/client.h>
#include <dcomp.h>              // S1 镜像叠层：合成交换链 + DComp 视觉（不建独立窗口，不抢焦点）
#include <d3d11.h>              // 仅用于「D3D12 设备不能当 IDXGIDevice」时的降级路径
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3d11.lib")
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <share.h>
#include <sys/stat.h>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

extern "C" void* g_xessTab[72] = {};
static const char* kNames[72] = {
#include "xess_fwd_names.inc"
};

static HMODULE g_self = nullptr, g_real = nullptr;
static FILE* g_log = nullptr;
static volatile bool g_nrOn = false;   // ★ 默认【关】：进游戏先给原生画面，按 F10 才开 NR
                                       //   （网络在加载阶段已预建，所以第一次按 F10 立即出画面，不必等约 0.9 秒）
static bool g_disabled = false;
static ComPtr<ID3D12Resource> g_fedTex[8];   // ≥ K（K 上限 8）
// （SRV 槽位映射见 K 声明之后的 kSrv* 常量）
static ComPtr<ID3D12Resource> g_ansLocal;      // S5-4：本地中转 buffer（跨卡 buffer 不能建 SRV）
static const UINT64 ANS_BYTES = 1920ull * 1080ull * 4ull;
static volatile bool g_useGpuResid = true;   // S5-5：残差改用 GPU 计算（读本地中转 buffer）
static ComPtr<ID3D12Heap> g_xHeapIn, g_xHeapOut, g_xHeapInW, g_xHeapOutW;
static ComPtr<ID3D12Resource> g_xBufIn, g_xBufOut, g_xBufInW, g_xBufOutW;
static ComPtr<ID3D12Fence> g_xFence;
static ComPtr<ID3D12Fence> g_xFenceW;
static bool g_xOk = false;
static volatile bool g_xWrite = true;
static volatile bool g_xRead  = true;    // R4：worker 从共享 buffer 读输入    // R3：游戏侧是否写共享 buffer
static ComPtr<ID3D12Resource> g_xHdrUp;  // 头部上传缓冲
static const UINT64 XHDR = 512;

static volatile bool g_menuOpen = false;   // 菜单是否打开（提前声明，供 IAT hook 使用）
static volatile float g_uiScale = 1.0f;          // 菜单界面缩放（0.6~2.5）
static ImGuiStyle g_baseStyle;                    // 未缩放的基准样式
static float g_uiApplied = -1.0f;                 // 已应用的缩放值
static volatile float g_transfer = 1.0f;          // 官方细节强度 0..1（+64）
static volatile float g_colorStr = 1.0f;          // 官方色彩强度 0..1（+68）
static volatile bool  g_dejitter = false;      // NR 输入去抖动（用该帧 jitter 反向偏移降采样）
static volatile float g_dejitterSign = 1.0f;   // 符号：+1 或 -1，试错用
static volatile float g_curJitX = 0.0f, g_curJitY = 0.0f;   // 本帧 jitter（像素）
static volatile float g_jitterScale = 0.5f;   // 实测 0.5 为闪/锐度折中点（1.0 抖、0 太软）   // XeSS jitter 缩放（实验：0=不抖，稳定但无时域增益）
static volatile int   g_debugView = 0;            // 0=正常 1=代理 2=纯网络 3=差异x20 4=着色
static volatile bool  g_bypass = false;           // 直出运行时输出（配合 debug_view 使用）

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}
static std::string ModuleDir() {
    char p[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(g_self, p, MAX_PATH);
    if (!n) return std::string();
    std::string s(p); size_t q = s.find_last_of("\\/");
    return (q == std::string::npos) ? std::string() : s.substr(0, q + 1);
}
static void OpenLog() {
    if (g_log) return;
    std::string s = ModuleDir() + "xess_proxy.log";
    g_log = _fsopen(s.c_str(), "w", _SH_DENYNO);
}
static HMODULE RealXess() {
    if (g_real) return g_real;
    std::string s = ModuleDir() + "libxess_orig.dll";
    g_real = LoadLibraryA(s.c_str());
    if (!g_real) Log("!! 加载失败 err=%lu", GetLastError());
    return g_real;
}
static void InitForwarders() {
    HMODULE m = RealXess();
    for (int i = 0; i < 72; ++i) {
        g_xessTab[i] = m ? (void*)GetProcAddress(m, kNames[i]) : nullptr;
        if (!g_xessTab[i]) Log("!! 缺少导出: %s", kNames[i]);
    }
}

// ---------------- lmxxf ABI ----------------
struct ApiTable {
    uint32_t struct_size; uint32_t abi_version;
    int32_t (*QueryCapabilities)(void*);
    int32_t (*Create)(const void*, void**);
    int32_t (*Destroy)(void*);
    int32_t (*PrepareSession)(void*);
    int32_t (*PrepareFrame)(void*, const void*, void*);
    int32_t (*RecordInputs)(void*, void*, void*);
    int32_t (*EnqueueHip)(void*, void*, void*);
    int32_t (*RecordOutputs)(void*, void*, void*);
    int32_t (*ExecuteAfterProducer)(void*, void*, void*);
    int32_t (*CancelUnsubmitted)(void*, void*);
    int32_t (*Poll)(void*, void*, uint32_t*);
    int32_t (*Retire)(void*, void*);
    int32_t (*ResetHistory)(void*);
    int32_t (*Drain)(void*);
    int32_t (*GetStatus)(void*, char*, uint32_t);
    int32_t (*GetLastError)(char*, uint32_t);
    int32_t (*GetTimings)(void*, void*);
    void* s18; void* s19;
};
struct FrameInfo160 {
    uint32_t struct_size; uint32_t pad0;
    uint64_t session_id, frame_id, list_generation;
    void* command_list;
    uint32_t color_width, color_height;
    void* color;
    uint32_t color_state, flags;
    float transfer_strength, color_strength;
    uint32_t debug_view; float model_scale;
    void* exposure; uint32_t exposure_state;
    float pre_exposure, exposure_scale;
    void* depth; void* velocity;
    float ctlTone, ctlStructure, ctlSkin, ctlOther, ctlStyle;
    uint32_t ctlPad;
    void* historyMask; void* reserved152;
};
static_assert(sizeof(FrameInfo160) == 160, "");
struct Job { uint32_t struct_size; void* handle; void* private_output; };
struct CreateInfo { uint32_t struct_size; uint32_t pad0; void* device; void* queue; const wchar_t* assets_directory; uint32_t flags; uint32_t pad1; };
static_assert(sizeof(CreateInfo) == 40, "CreateInfo 必须 40 字节（对齐官方 LmxxfNrCreateInfo）");

// ---- 官方 include/LmxxfNrApi.h 的 ABI 常量（2026-10-26 核对，见知识库 §11.2）----
// 表：v1 = 136（2×u32 + 16 个函数指针）；当前 = 144（尾部追加 GetTimings）
// FrameInfo：v1 = 80（止于 model_scale）；当前 = 104（追加 exposure/exposure_state/pre_exposure/exposure_scale）
static const uint32_t kApiSizeCurrent   = 144u;
static const uint32_t kApiSizeV1        = 136u;
static const uint32_t kFrameSizeCurrent = 104u;
static const uint32_t kFrameSizeV1      = 80u;

struct Capabilities {   // LmxxfNrCapabilities
    uint32_t struct_size; uint32_t abi_version;
    uint32_t max_input_width; uint32_t max_input_height;
    uint32_t history_supported, overlap_supported, graph_supported, hip_ready, gfx1201_target;
};

static const UINT kNetW = 1920, kNetH = 1080;
static const int  K = 6;                       // 槽位数（6 ⇒ 余量 1~3；SRV 映射已按 K 计算，见 §56.10）
// 呈现延迟固定 3、喂帧滞后固定 2 —— 这两个不是"可调参数"，是结构事实：
//   · 降采样在帧 N 录制、结果要帧 N+1 才能从回读缓冲读出 ⇒ 能拿到的最新小图永远是第 N-2 帧的
//     （实测把滞后写成 1 只是标签改错：内容仍是 N-2、答案被写进 N-1 ⇒ 错位，而等待均值照样 0.8ms）
//   · 再由 kDelay ≥ 滞后+1 且 kDelay ≤ K-1（K=4）⇒ kDelay 只能是 3
// 要余量（kDelay − 滞后）> 1 必须先加槽位（主文档 §56）
// ---- SRV/UAV 槽位映射：全部按 K 算，禁止写死（K>4 时旧版会撞车）----
// 颜色 [0,K) / 残差 K / netS K+1 / pre K+2 / smooth K+3 / MV [K+4,2K+3) / fed [2K+4,3K+3) / 答案 3K+4
static const UINT kSrvRes     = (UINT)K;
static const UINT kSrvNetS    = (UINT)K + 1;
static const UINT kSrvPre     = (UINT)K + 2;
static const UINT kSrvSmooth  = (UINT)K + 3;
static const UINT kSrvMvBase  = (UINT)K + 4;
static const UINT kSrvFed     = 2u * (UINT)K + 4u;
static const UINT kSrvAns     = 3u * (UINT)K + 4u;
static const UINT kSrvCount   = 3u * (UINT)K + 5u;
// ---- 路线 B / B1：残差改在 4K 上叠加 ----
//   kSrvOutp    : outp 的 4K SRV（Add4K 的底图）
//   kSrvPreSrv  : g_pre 的 SRV（合成后的 pColor = 槽位原图 + e）
//   kSrvPre4K   : g_pre4k 的 4K UAV（叠加结果，之后拷回 outp）
static const UINT kSrvOutp    = kSrvCount;
static const UINT kSrvPreSrv  = kSrvCount + 1u;
static const UINT kSrvPre4K   = kSrvCount + 2u;
static const UINT kSrvMvCur   = kSrvCount + 3u;    // 当前帧原始 MV 的 SRV（打包用）
static const UINT kSrvMvPacked= kSrvCount + 4u;    // 打包后的 MV（1080p RG16F）UAV
static const UINT kSrvCount2  = kSrvCount + 5u;
static const UINT kSrvOutp8   = kSrvCount2;        // 4K 后置：8bit 转换目标（UAV）
static const UINT kSrvCount3  = kSrvCount2 + 1u;
static const UINT kSrvOutp8b  = kSrvCount3;        // 第二缓冲的 UAV（双缓冲必须各一个描述符）
static const UINT kSrvCount4  = kSrvCount3 + 1u;
static volatile int  g_kDelay = 3;                // 呈现延迟（帧），固定 3
static const int     kFeedLag = 2;                // 喂帧滞后（结构固定）
static volatile bool g_kDelayAuto = true;         // 保留字段：现在不再改 kDelay

// ---------------- worker ----------------
struct Worker {
    std::thread th; std::mutex m; std::condition_variable cv; std::condition_variable cvOut;
    std::vector<uint8_t> inBuf, outBuf;
    bool hasIn = false, hasOut = false, quit = false;
    int inIndex = -1, outIndex = -1;
    int inFrame = -1;                 // 这一份输入对应哪一帧（身份校验用）
    std::vector<std::vector<uint8_t>> ansBuf;   // [K] 每槽一份答案（身份化交付）
    std::vector<int> ansFrame;                  // [K] 该槽答案对应的帧号，-1 = 还没算过
    int processed = 0; double lastMs = 0;
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al; ComPtr<ID3D12CommandAllocator> al2;   // ★ 双分配器轮转（见 ai/aiFv）
    ComPtr<ID3D12GraphicsCommandList> cl;
    int      ai = 0;                     // 本轮用哪一个（0/1 轮转）
    UINT64   aiFv[2] = { 0, 0 };         // 该分配器上次提交后的围栏值（复用前要确认已完成）
    ID3D12CommandAllocator* curAl = nullptr;
    ComPtr<ID3D12Fence> fe; UINT64 fv = 0;
    ComPtr<ID3D12Fence> xferFe;   // 跨卡共享围栏（A 的 4K 拷贝完成 → B 解码前等它）
    ComPtr<ID3D12Resource> color, up, rb, rbPriv;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}, fpPriv{};
    UINT64 bytes = 0, bytesPriv = 0;
    void* upPtr = nullptr;
    HMODULE mod = nullptr; ApiTable api{}; void* ctx = nullptr; bool ok = false;
    bool extAbi = true;   // true = 144 字节表（当前 lmxxf）；false = 官方 v1 / 136（mochizuki 等）
    uint32_t frameSize = 160;   // FrameInfo 声明尺寸，跟随被接受的 ABI（160 / 104 / 80）
    std::wstring assetsDir;     // 建会话用的资源目录（重建会话时复用）
    bool isMochi = false;       // 后端身份：mochizuki / lmxxf 系
    int  modelApplied = -1;     // 当前会话对应哪个模型请求（1=Original, 0=Small）
    void* setControls = nullptr;   // MochizukiNrSetControls（不在 ApiTable 里，单独 GetProcAddress）
};
static Worker g_worker;
static char g_statusLine[256] = { 0 };   // 运行时自报状态（GetStatus），菜单里显示
static volatile int g_modelSel = 1;      // 模型请求：1 = Original 71-block，0 = 31-block Small
// ---- mochizuki 风格化控制块（S4）：布局来自交接包 §九.3（反汇编 parse 得到）----
// 0x00 size(0x20C) / 0x08 intensity / 0x0C style / 0x10 tone / 0x14 structure /
// 0x18 skin / 0x1C automask / 0x20 grade / 0x2C 总闸（必须非 0，否则整块无效）
static volatile float g_nrIntensity = 2.0f, g_nrTone = 1.0f, g_nrStruct = 1.0f;   // 2.0 = 本项目调好的默认（见 0.1.1 定版说明）
static volatile float g_nrSkin = -1.0f, g_nrGrade = 2.0f;   // grade 2.0 = 本项目实测观感（0.1.2 起作为出厂默认）
static volatile int   g_nrStyle = 0;                 // 0 Default / 1 Natural / 2 Cinematic
static volatile bool  g_nrAutoMask = false, g_nrCtrlOn = true;
static volatile bool  g_ctrlDirty = true;            // 让 worker 下一帧重新下发

static bool WaitW() {                       // 有界等待（纪律：任何阻塞等待必须有超时）
    Worker& W = g_worker;
    W.q->Signal(W.fe.Get(), ++W.fv);
    for (int i = 0; i < 1500; ++i) {        // 最多约 150 ms
        if (W.fe->GetCompletedValue() >= W.fv) return true;
        Sleep(0);
    }
    static int to = 0;
    if ((to++ % 20) == 0) Log("!! worker 等待 GPU 完成超时（累计 %d 次，fence %llu vs %llu）",
                              to, (unsigned long long)W.fe->GetCompletedValue(), (unsigned long long)W.fv);
    return false;
}
// ★ 真正等 GPU 落地（有界）。WaitW 的自旋 1500 次 Sleep(0) 在忙系统上不到 1ms 就返回 false
//   （日志里上万次"超时"多半是假超时）；回读答案前必须用围栏确认，否则 Map 会读到上一帧的旧数据，
//   答案与槽位不配对 → A 侧 NR 直接失效（这就是 10-28 那次"NR 又失效"的成因）。
static bool WaitWFence(DWORD ms) {
    Worker& W = g_worker;
    if (!W.fe || !W.q) return false;
    if (W.fe->GetCompletedValue() >= W.fv) return true;
    // ★ 先自旋查围栏：事件等待会撞 Windows 默认 15.6ms 定时器精度，
    //   GPU 明明 1ms 就完事，却要等下一个 tick 才唤醒（实测每帧白等 ~15ms，worker 2.4→18.6ms）
    ULONGLONG t0 = GetTickCount64();
    while ((GetTickCount64() - t0) < (ULONGLONG)ms) {
        if (W.fe->GetCompletedValue() >= W.fv) return true;
        SwitchToThread();
    }
    return false;
}
static ComPtr<ID3D12Resource> MakeTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
                                      D3D12_RESOURCE_STATES st, bool uav, bool shared = false) {
    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    dev->CreateCommittedResource(&dh, shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE,
                                &td, st, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
    return r;
}

static bool WorkerCreateSession();   // 前向声明（WorkerInit 里调用；定义在 WorkerInit 之后）

static unsigned int g_wantDevId = 0;   // 安装器指定的神经卡 DeviceId（0=未指定）
static bool WorkerInit() {
    Worker& W = g_worker;
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(fac.GetAddressOf())))) return false;
    ComPtr<IDXGIAdapter1> ad;
    {   // 运行时要求 D3D12 设备与 HIP 设备同卡
        // ★ 可选：安装器写入的神经卡选择。支持两种写法：
        //     nr=dev:7550                 按 PCI DeviceId（安装器用这种，简单可靠）
        //     nr=luid:00000000:0001A2B3   按 LUID（同名双卡时更精确）
        //   文件不存在 / 解析失败 / 不匹配 → 完全回落到原有自动选择（0x7550 → 序数 1）
        LUID wantLuid{}; bool haveWant = false; bool haveWantDev = false;
        unsigned wantSub = 0; bool haveWantSub = false;   // nr=sub:deviD:subsys —— 同 DeviceId 双卡靠子型号区分
        {
            std::string gp = ModuleDir() + "xess_proxy.gpu.txt";
            FILE* gf = nullptr;
            if (fopen_s(&gf, gp.c_str(), "r") == 0 && gf) {
                // ★ 必须整文件读进来再用 sscanf 试格式：
                //   fscanf 在"字面量中途失配"时已经消费掉了已匹配的字符（例如先把 "nr=" 吃掉），
                //   导致顺序尝试第二种格式时永远从错误位置开始 ⇒ 以前 nr=luid: 与 nr=sub: 都读不到。
                char gbuf[256] = { 0 };
                size_t gotBytes = fread(gbuf, 1, sizeof(gbuf) - 1, gf);
                gbuf[gotBytes] = 0;
                fclose(gf);
                unsigned long long hi = 0, lo = 0;
                unsigned int wantDev = 0, wantSubTmp = 0;
                if (sscanf_s(gbuf, "nr=sub:%x:%x", &wantDev, &wantSubTmp) == 2 && wantDev) {
                    // DeviceId + 子型号（SubsystemId）双键：同 DeviceId 的两张卡（如 9070 XT / 9070 GRE 都是 0x7550）也能区分
                    g_wantDevId = wantDev; wantSub = wantSubTmp; haveWantDev = true; haveWantSub = true;
                }
                else if (sscanf_s(gbuf, "nr=dev:%x", &wantDev) == 1 && wantDev) {
                    g_wantDevId = wantDev; haveWantDev = true;
                }
                else if (sscanf_s(gbuf, "nr=luid:%llx:%llx", &hi, &lo) == 2) {
                    wantLuid.HighPart = (LONG)hi; wantLuid.LowPart = (DWORD)lo; haveWant = true;
                }
                if (haveWantDev) Log("worker: 读到安装器的神经卡选择 DeviceId=0x%04X", g_wantDevId);
                if (haveWantSub) Log("worker: 读到安装器的神经卡选择 SubsystemId=0x%04X（按子型号精确选卡）", wantSub);
                else if (haveWant) Log("worker: 读到安装器的神经卡选择 LUID=%08X:%08X", (unsigned)wantLuid.HighPart, (unsigned)wantLuid.LowPart);
            }
        }
        ComPtr<IDXGIAdapter1> best, fallback;
        for (UINT i = 0; i < 8; ++i) {
            ComPtr<IDXGIAdapter1> a1;
            if (fac->EnumAdapters1(i, a1.GetAddressOf()) != S_OK) break;
            DXGI_ADAPTER_DESC dd{};
            a1->GetDesc(&dd);
            Log("  worker 候选[%u] = %ls  DeviceId=0x%04X  SubSysId=0x%08X", i, dd.Description, dd.DeviceId, (unsigned)dd.SubSysId);
            if (haveWantSub) {
                // 子型号也必须一致，避免"同 DeviceId 的另一张卡"被选中
                if (dd.DeviceId == g_wantDevId && (unsigned)((dd.SubSysId >> 16) & 0xFFFF) == wantSub && !best) best = a1;
            }
            else if (haveWantDev) { if (dd.DeviceId == g_wantDevId && !best) best = a1; }
            else if (haveWant && (dd.AdapterLuid.HighPart == wantLuid.HighPart) && (dd.AdapterLuid.LowPart == wantLuid.LowPart)) best = a1;
            else if (!haveWant && dd.DeviceId == 0x7550 && !best) best = a1;   // 未指定时按 DeviceId（0x7550 = RX 9070 XT）
        // 不再按名字兜底：显卡可被改名，只有 DeviceId 可靠（0x7550 = Navi48 / RX 9070 XT）
            if (i == 1 && !fallback) fallback = a1;
        }
        if (!best && haveWantDev) {
            Log("worker: 注意：配置指定的神经卡（DeviceId=0x%04X%s）在枚举里没找到，回落到序数 1 的适配器",
                g_wantDevId, haveWantSub ? " + 子型号" : "");
        }
        ad = best ? best : fallback;
    }
    if (!ad) { Log("worker: 找不到适配器"); return false; }
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(W.dev.GetAddressOf())))) {
        Log("worker: 设备创建失败"); return false;
    }
    {
        DXGI_ADAPTER_DESC dc{};
        ad->GetDesc(&dc);
        Log("worker: 使用 %ls", dc.Description);
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    W.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(W.q.GetAddressOf()));
    W.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(W.al.GetAddressOf()));
    W.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(W.al2.GetAddressOf()));  // ★ 第二组（轮转用）
    W.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, W.al.Get(), nullptr, IID_PPV_ARGS(W.cl.GetAddressOf()));
    W.cl->Close();
    // mochizuki 的 Windows 路线要把 D3D12 fence 导入 Vulkan timeline semaphore ⇒ fence 必须 SHARED。
    // lmxxf(HIP) 不关心这个标志，加了对它无影响。
    W.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(W.fe.GetAddressOf()));

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    // mochizuki 是 Vulkan 后端：它要把我们给的 D3D12 纹理导入 Vulkan（CreateSharedHandle），
    // 所以这张输入纹理必须带 D3D12_HEAP_FLAG_SHARED；否则导入失败、首帧就卡住（本机实测现象）。
    W.color = MakeTex(W.dev.Get(), kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON, false, true);
    if (!W.color) { Log("worker: 纹理失败"); return false; }
    D3D12_RESOURCE_DESC cd = W.color->GetDesc();
    D3D12_RESOURCE_DESC plain = cd; plain.Flags = D3D12_RESOURCE_FLAG_NONE;
    UINT rows = 0; UINT64 rs = 0, total = 0;
    W.dev->GetCopyableFootprints(&plain, 0, 1, 0, &W.fp, &rows, &rs, &total);
    W.bytes = total;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = W.bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    W.dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(W.up.GetAddressOf()));
    {
        D3D12_RANGE zr{ 0, 0 };
        if (SUCCEEDED(W.up->Map(0, &zr, &W.upPtr)) && W.upPtr) Log("worker: 上传缓冲持久映射 ok");
    }
    W.inBuf.assign((size_t)W.bytes, 0);
    W.outBuf.assign((size_t)W.bytes, 0);

    std::string dir = ModuleDir();
    std::wstring pak(dir.begin(), dir.end()); pak += L"LmxxfNrRuntime.pak";
    std::wstring dllp = pak;
    size_t sl = dllp.find_last_of(L'\\');
    if (sl != std::wstring::npos) dllp = dllp.substr(0, sl + 1);
    dllp += L"LmxxfNrRuntime.dll";
    // 模型请求：先读 xess_proxy.model.txt（"1"/"2"/"3" = Original 单/2/3 遍，"0.5" = Small），
    // 缺省 Original。环境变量在 WorkerCreateSession() 里、Create 之前才写，
    // 因为真正的开关是"用哪个模型建会话"（= AMDNR 的做法：换模型就重建会话）。
    g_modelSel = 1;
    {
        std::string mp = ModuleDir() + "xess_proxy.model.txt";
        FILE* mf = nullptr;
        if (fopen_s(&mf, mp.c_str(), "r") == 0 && mf) {
            char ln[32] = { 0 };
            if (fgets(ln, sizeof(ln), mf)) {
                std::string v(ln);
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
                size_t s = v.find_first_not_of(" \t");
                if (s != std::string::npos) v = v.substr(s);
                if (v == "0.5") g_modelSel = 0;
                else if (v == "1" || v == "2" || v == "3") g_modelSel = 1;
            }
            fclose(mf);
            Log("worker: xess_proxy.model.txt = %s → 请求 %s", ln, g_modelSel ? "Original 71-block" : "Small 31-block");
        }
    }
    W.mod = LoadLibraryW(dllp.c_str());
    if (!W.mod) { Log("worker: lmxxf 加载失败（%ls, err=%lu）", dllp.c_str(), GetLastError()); return false; }
    using GetApiFn = int32_t(*)(uint32_t, void*);
    auto getApi = (GetApiFn)GetProcAddress(W.mod, "LmxxfNrGetApi");
    W.setControls = (void*)GetProcAddress(W.mod, "MochizukiNrSetControls");   // 不在 ApiTable 里
    Log("worker: MochizukiNrSetControls = %p（风格控制块用）", W.setControls);
    if (!getApi) { Log("worker: 导出 LmxxfNrGetApi 缺失"); return false; }

    // ABI 协商：三个已知尺寸，按"最兼容 → 最保守"依次试
    //   160 = 本机 AMDNR 版运行时的扩展表（原基线一直在用，实测被接受）
    //   144 = 上游 lmxxf 当前头文件 sizeof(LmxxfNrApi)（含 GetTimings）
    //   136 = 官方 ABI v1（LMXXF_NR_API_V1_SIZE；mochizuki 等只认这个）
    static const uint32_t kApiSizes[] = { 160u, 144u, 136u };
    int32_t abiRc = -1;
    uint32_t abiTried = 0;
    for (uint32_t sz : kApiSizes) {
        memset(&W.api, 0, sizeof(W.api));
        W.api.struct_size = sz;
        abiRc = getApi(1, &W.api);
        abiTried = sz;
        if (abiRc == 0) break;
    }
    if (abiRc != 0) { Log("worker: GetApi 失败（160/144/136 均被拒 rc=%d）", (int)abiRc); return false; }
    W.extAbi = (abiTried != kApiSizeV1);
    // FrameInfo 尺寸与表尺寸配对：160(AAMDNR 扩展，含 ctl*/historyMask) / 104(上游当前) / 80(v1)
    W.frameSize = (abiTried == 160u) ? 160u : ((abiTried == kApiSizeCurrent) ? kFrameSizeCurrent : kFrameSizeV1);
    Log("worker: ABI 就绪 abi_version=%u 请求=%u 声明 struct_size=%u FrameInfo=%u 扩展=%d",
        W.api.abi_version, abiTried, W.api.struct_size, W.frameSize, (int)W.extAbi);

    // 运行时身份：mochizuki 才走 dlssnr-amd 资源目录（不能只看 ABI 尺寸——136 也可能被 lmxxf 系接受）
    bool isMochi = (GetProcAddress(W.mod, "MochizukiNrGetInfo") != nullptr) ||
                   (GetProcAddress(W.mod, "MochizukiNrSetControls") != nullptr);
    Log("worker: 运行时 = %s", isMochi ? "mochizuki" : "lmxxf/其它");

    // 能力查询：只在扩展 ABI 上调用，v1 路径与稳定基线保持一致
    if (W.extAbi && W.api.QueryCapabilities) {
        Capabilities cap{}; cap.struct_size = (uint32_t)sizeof(Capabilities);
        if (W.api.QueryCapabilities(&cap) == 0)
            Log("worker: caps 输入<=%ux%u history=%u overlap=%u graph=%u hip=%u gfx1201=%u",
                cap.max_input_width, cap.max_input_height, cap.history_supported,
                cap.overlap_supported, cap.graph_supported, cap.hip_ready, cap.gfx1201_target);
        else Log("worker: QueryCapabilities 失败");
    }

    // 资源目录：
    //   · lmxxf 系运行时要求 assets_directory = 【modules 目录】（内含 SHA256SUMS + *.hsaco）
    //     —— 实测：传 pak 文件路径 → "assets_directory is not a directory"；
    //              传游戏目录   → "modules directory needs SHA256SUMS + hsaco"；
    //              传 lmxxf-modules → Create=0 / PrepareSession=0 / weights=1
    //   · mochizuki → dlssnr-amd
    std::wstring assets(dir.begin(), dir.end());
    if (isMochi) {
        std::wstring alt = assets; alt += L"dlssnr-amd";
        if (GetFileAttributesW(alt.c_str()) != INVALID_FILE_ATTRIBUTES) assets = alt;
        else Log("worker: 未找到 dlssnr-amd，mochizuki 将自行解析");
    } else {
        std::wstring mod = assets; mod += L"lmxxf-modules";
        if (GetFileAttributesW(mod.c_str()) != INVALID_FILE_ATTRIBUTES) assets = mod;
        else Log("worker: 未找到 lmxxf-modules，退回模块目录");
    }
    Log("worker: assets 目录 = %ls", assets.c_str());
    W.assetsDir = assets;
    W.isMochi = isMochi;
    return WorkerCreateSession();
}

// 下发 mochizuki 风格控制块（S4）。实测：必须在 PrepareSession 之后再调一次，
// 运行时才把 uniform 用上；0x2C（总闸）为 0 时整块无效（这是以前"改了没反应"的根因）。
static void ApplyMochiControls(bool force = false) {
    Worker& W = g_worker;
    if (!W.isMochi || !W.setControls || !W.ctx) return;
    if (!force && !g_ctrlDirty) return;
    // ★ 块尺寸必须与 struct_size 一致：运行时读 min(size,0x20C) 字节。
    //   以前声明 0x20C、缓冲只有 256 ⇒ 运行时会读过界 268 字节（读到栈垃圾）。
    //   parse 实际只读 ≤0xEC，所以行为不变，但过界读本身是 UB，这里按声明尺寸给足。
    uint8_t blk[0x20C] = { 0 };
    auto putF = [&](int off, float v) { memcpy(blk + off, &v, 4); };
    auto putU = [&](int off, uint32_t v) { memcpy(blk + off, &v, 4); };
    putU(0x00, 0x20Cu);                              // struct_size（运行时 min(size,0x20C)，且要求 >=4）
    putF(0x08, g_nrIntensity);
    putU(0x0C, (uint32_t)g_nrStyle);
    putF(0x10, g_nrTone);
    putF(0x14, g_nrStruct);
    putF(0x18, g_nrSkin);
    putU(0x1C, g_nrAutoMask ? 1u : 0u);
    putF(0x20, g_nrGrade);
    putU(0x2C, g_nrCtrlOn ? 1u : 0u);                // ★ 总闸
    using Fn = int32_t(*)(void*, void*);
    int32_t r = ((Fn)W.setControls)(W.ctx, blk);
    g_ctrlDirty = false;
    Log("worker: SetControls r=%d | style=%d intensity=%.2f tone=%.2f struct=%.2f skin=%.2f mask=%d grade=%.2f enable=%d",
        (int)r, (int)g_nrStyle, g_nrIntensity, g_nrTone, g_nrStruct, g_nrSkin,
        (int)g_nrAutoMask, g_nrGrade, (int)g_nrCtrlOn);
}

// 建会话（建哪张网络由 g_modelSel 决定）。换模型 = 重建会话，这是 AMDNR 的做法：
// 请求值在 Create 之前写环境变量，销毁旧会话后重新 Create/PrepareSession。
// mochizuki 的模型选择器 = 哪个权重文件在位（实测唯一有效的通道）：
//   在 = CC_SMALL_FP8（新），不在 = 回退 CC_SILVER_AARDWOLD（旧）
// 切换 = 改文件名 + 重建会话（照 AMDNR 的编排：旧网络继续出帧 → 新网络建好 → 换过去）
static bool MochiSetWeights(int sel) {   // sel: 0=Small, 1=Original
    std::string base = ModuleDir() + "dlssnr-amd\\dlssnr-small.bin";
    std::string off1 = base + ".off";
    std::string off2 = base + ".off-1026";
    bool on = (GetFileAttributesA(base.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (sel == 1 && on) {
        if (MoveFileA(base.c_str(), off1.c_str())) { Log("worker: 模型选择 → Original（已隐藏 small 权重）"); }
        else { Log("worker: 隐藏 small 权重失败 err=%lu", GetLastError()); return false; }
    } else if (sel == 0 && !on) {
        const char* src = (GetFileAttributesA(off1.c_str()) != INVALID_FILE_ATTRIBUTES) ? off1.c_str() : off2.c_str();
        if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
            Log("worker: 找不到 small 权重备份，无法切到 Small"); return false;
        }
        if (MoveFileA(src, base.c_str())) { Log("worker: 模型选择 → Small（已恢复 small 权重）"); }
        else { Log("worker: 恢复 small 权重失败 err=%lu", GetLastError()); return false; }
    }
    Log("worker: 当前 small 权重在位=%d（1=新模型 0=旧模型）", (int)(GetFileAttributesA(base.c_str()) != INVALID_FILE_ATTRIBUTES));
    return true;
}

static bool WorkerCreateSession() {
    Worker& W = g_worker;
    if (W.isMochi) MochiSetWeights(g_modelSel);      // mochizuki：靠权重在位
    const wchar_t* mp = g_modelSel ? L"1" : L"0.5";
    SetEnvironmentVariableW(L"DLSS5_MULTI_PASS", mp);   // lmxxf 系：靠环境变量（mochizuki 忽略）
    {
        wchar_t rb[16] = { 0 };
        GetEnvironmentVariableW(L"DLSS5_MULTI_PASS", rb, 16);
        Log("worker: 建会话 → 请求 DLSS5_MULTI_PASS=%ls（回读 %ls，%s）",
            mp, rb, g_modelSel ? "Original 71-block" : "Small 31-block");
    }
    CreateInfo ci{};
    ci.struct_size = (uint32_t)sizeof(CreateInfo);
    ci.device = W.dev.Get(); ci.queue = W.q.Get(); ci.assets_directory = W.assetsDir.c_str();
    if (W.api.Create(&ci, &W.ctx) != 0) {
        char eb[256] = { 0 }; if (W.api.GetLastError) W.api.GetLastError(eb, 256);
        Log("worker: Create 失败 err=%s assets=%ls", eb, W.assetsDir.c_str()); W.ok = false; return false;
    }
    if (W.api.PrepareSession(W.ctx) != 0) {
        char eb[256] = { 0 }; if (W.api.GetLastError) W.api.GetLastError(eb, 256);
        Log("worker: PrepareSession 失败 err=%s", eb); W.ok = false; return false;
    }
    W.modelApplied = g_modelSel;
    W.ok = true;
    Log("worker: 会话就绪 (%ux%u, %llu 字节) 模型=%s", kNetW, kNetH,
        (unsigned long long)W.bytes, g_modelSel ? "Original" : "Small");
    ApplyMochiControls(true);   // 会话建好后下发风格控制块（实测必须在这一步之后再调一次）
    return true;
}

// 换模型：销毁旧会话 → 用新请求重建。调用方是 worker 线程，游戏线程不受阻塞。
static bool WorkerRebuildForModel(int sel) {
    Worker& W = g_worker;
    if (!W.ctx || !W.api.Destroy) { Log("worker: 没有会话，无法重建"); return false; }
    auto t0 = Clock::now();
    W.ok = false;
    W.api.Destroy(W.ctx); W.ctx = nullptr;
    g_modelSel = sel;
    bool ok = WorkerCreateSession();
    // 实测结论（2026-10-26）：kDelay=2 视觉上不可用（帧配对错位），3 是本链路的硬下限 ——
    // 因为喂帧滞后固定 2 帧（inIndex=(N-2)%K），present 必须再退 1 帧才能配到"同一帧的内容与残差"。
    // 所以换模型不改 kDelay，保持 3。
    (void)g_kDelayAuto;
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    Log("worker: 模型切换 → %s（重建会话 %s，%.0f ms；时域状态按新会话重来）",
        sel ? "Original 71-block" : "Small 31-block", ok ? "成功" : "失败", ms);
    return ok;
}


// ---- B 形态：这几个定义在文件前半部分（WorkerMain 要用），其余定义在后面 ----
static bool g_bformOn = false;
static bool g_bformReady = false;
static int  g_upSlot = 0;
static ComPtr<ID3D12Resource> g_upBufW;
static void BformBDecode(ID3D12GraphicsCommandList* cl, int slot);
static bool BformBEnsureDS(ID3D12Device* dev);
static bool BformBDownsample(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst1080);
static bool g_bCompReady = false;
static bool g_bUpReady = false;
static int  g_bformStage = 0;   // 1..5：只开到第 N 步
static bool BformBEnsureComp(ID3D12Device* dev);
static bool BformBCompose(ID3D12GraphicsCommandList* cl, ID3D12Resource* ansTex);
static bool BformBEnsureUp(ID3D12Device* dev);
static bool BformPPEnsure(ID3D12Device* dev);
static void BformPPStage1(ID3D12GraphicsCommandList* cl, int i, ID3D12Resource* nrColorDst, ID3D12Resource* a, ID3D12Resource* b);
static bool BformPPStage2(int j);
static bool g_ppReady = false;
static int  g_ppCur = 0;
static bool g_ppHavePrev = false;
static volatile ULONGLONG g_workerBeat = 0;
static UINT g_diagCnt = 1;
static ComPtr<ID3D12Resource> g_bUp4k, g_bTexC;   // 诊断要用，提前定义
static void BformDiagEnqueue(Worker& W, ID3D12Resource* ans);   // 五源同块对照（挂 worker 自己的 list）
static void BformDiagReadback(Worker& W);   // 回读 + 打一行数字
static ComPtr<ID3D12Resource> g_ppAns[2];   // WorkerMain 里要用，提前声明
static bool BformBUpscale(ID3D12GraphicsCommandList* cl);
static HWND g_gameHwnd = nullptr;
// ★ 取句柄不能依赖菜单：B 形态出画（DComp 贴到游戏窗口）必须要 HWND，
//   而以前唯一的取句柄入口在 Del 菜单里 → 不开一次菜单就永远 g_gameHwnd == nullptr
//   → B 形态永远不初始化出画（现象：进游戏后 NR 画面不出现，开一次菜单才出现）
static HWND ResolveGameHwnd();
static volatile int g_upLastSlot = 0;   // A 最近写完的槽号（B 直接读它，省掉 kFeedLag 的 2 帧延迟）
static volatile bool g_bStarved = false; // 防饿死门：本轮 B 落后太多 → 跳过合成（保持上一帧，不要撕裂帧）
// 槽位"面单"：记录每个槽位里装的是【第几帧】+ 该帧的 jitter。
//   A 写、B 读，都在同一个进程里 → 不需要过显卡、也不需要回读，直接 CPU 侧共享。
//   用途：① 让 B 知道自己解得的是第几帧（以前完全不知道）② 给 TAA 做抖动补偿
struct BformSlotTag { volatile int frame; volatile float jx, jy; };
static BformSlotTag g_slotTag[K];
static bool g_bpReady = false;
static bool g_bpFailed = false;
// 答案交付（按形态分路）：A 形态在 worker 内等落地并当场交付；B 形态整段跳过
static int    g_pendIdx = -1;      // 保留字段（历史），未使用
static int    g_pendFrame = -1;
static UINT64 g_pendFv = 0;
static bool   g_pendValid = false;
static bool BformPresentInit();
static void BformPresentFrame();
// 覆盖层可见性：切回 A（stage<6）时把 DComp 视觉卸掉，否则会留一张冻结的 B 画面盖住 A
static void BformPresentShow(bool show);

// ---- B 形态 TAA-lite（时域上采样：治噪点/锯齿）----
//   bform=8 = 阶段 6 + 一个 TAA pass（jitter 由共享 buffer 头部传；本版先不做 jitter 补偿）
//   出画源从 g_bUp4k 换成 TAA 输出；bform=6 保持原样，方便 A/B 对照
static bool g_taaOn = false;
// ---- 4K 后置：10bit→8bit 转换（pOutput 是 R10G10B10A2，我们整条链是 8bit）----
static const char* kBformFmt8 = R"(
Texture2D<float4>   gS : register(t0);
RWTexture2D<float4> gD : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; gD.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    gD[id.xy] = float4(gS.Load(int3(id.xy, 0)).rgb, 1.0);
}
)";
static ComPtr<ID3D12Resource> g_outp8[2];   // 4K / B8G8R8A8 转换结果（双缓冲：一帧写、一帧拷）
static ComPtr<ID3D12PipelineState> g_psoFmt8;
static bool g_outp8Ready[2] = { false, false };
static ComPtr<ID3D12CommandQueue>        g_xqQ;
static ComPtr<ID3D12CommandAllocator>    g_xqAl[3];   // 3 组轮转（单组会在上一份拷贝没跑完时 Reset 失败 → 半帧不更新 → 画面交替）
static ComPtr<ID3D12GraphicsCommandList> g_xqCl[3];
static int      g_xqAi = 0;                        // 3 组轮转指针
static UINT64   g_xqFvUsed[3] = { 0, 0, 0 };        // 每组上次的完成围栏值
static ComPtr<ID3D12Fence>               g_xqFe;    // 游戏提交后的记号（A 内部排序）
static ComPtr<ID3D12Fence>               g_xqFe2;   // 拷贝完成（共享给 B）
static UINT64        g_xqFv = 0, g_xqFv2 = 0;
static HANDLE        g_xqShare = nullptr;
static volatile UINT64 g_slotReadyFv[K] = { 0,0,0,0,0,0 };   // 每槽：B 解码前要等的围栏值
static volatile bool g_xferOwn = false;                       // true = 颜色由我们的队列拷
static volatile LONG g_xferKick = 0, g_xferSkip = 0;
static volatile UINT64 g_lastSigV = 0;   // 最近一次"提交记号"的值（拷贝等它）
// ---- 抓帧导出（全局声明 + 前向声明；实现在 BformCopy 附近）----
static std::string g_dumpDir;
static int   g_dumpMask, g_dumpLeft;
static UINT  g_dumpW, g_dumpH, g_dumpPitch;
static ComPtr<ID3D12Resource> g_dumpRb[3];   // 0=A 1=N 2=C
static bool g_dumpPend[3] = {false,false,false};
static int  g_dumpPendFrame[3] = {0,0,0};
static void DumpPollFile();
static void DumpRecord(ID3D12GraphicsCommandList* cl, ID3D12Device* dev, int st,
                       ID3D12Resource* src, D3D12_RESOURCE_STATES cur);
static void DumpFlush(int st);
static volatile UINT64 g_xferPendSig = 0;   // 登记 pending 时的记号值
static volatile int g_xferPendFrameTag = 0;   // 待拷那一帧的帧号（拷贝成功后用它发布面单）
// ★ 待拷队列（4 坑 FIFO）：以前只有 1 个坑，某帧没及时触发就被下一帧覆盖 → 那个槽位永远没颜色
struct XferPend { volatile bool valid; int slot; int parity; unsigned long long sig; int frameTag; };
static XferPend g_pend[4];
static volatile LONG g_pendW = 0;
static volatile bool g_xferPendValid = false;
static volatile int  g_xferPendParity = -1;                   // 钩子里要拷的缓冲奇偶
static volatile UINT g_xferPendSlot = 0;
static ID3D12GraphicsCommandList* g_ourList = nullptr;        // 本帧我们录过转换的那条 list
static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,  // 前沿声明（worker 里的 4K 直通要用）
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
static volatile bool g_bform4k = false;   // ★ 必须 volatile：worker 线程要读它              // bform=9：取 pOutput（XeSS 之后, 4K）作底图，B 端直通出画
static ComPtr<ID3D12Resource> g_outpRes;    // 本帧的 pOutput（每帧在 hook 里赋值，供 4K 模式取帧）
static bool g_taaReady = false;
static bool g_taaValid = false;          // 历史是否有效（切换/首帧时无效）
static ComPtr<ID3D12Resource> g_taaOut;  // TAA 输出（诊断要提前引用，定义在此，后面不再重复定义）
// TAA 两个参数（可由 xess_proxy.taa.txt 热调，格式："alpha clampK"）
static volatile float g_taaAlpha  = 0.15f;
static volatile float g_taaClampK = 3.0f;
static bool BformTaaEnsure(ID3D12Device* dev);
static bool BformTaaRun(ID3D12GraphicsCommandList* cl);
static ID3D12Resource* BformPresentSrc();

static void WorkerMain() {
    Worker& W = g_worker;
    W.ansBuf.assign(K, std::vector<uint8_t>((size_t)kNetW * kNetH * 4, 0));
    W.ansFrame.assign(K, -1);
    while (true) {
        std::vector<uint8_t> local;
        int idx = -1, frameId = -1;
        {
            std::unique_lock<std::mutex> lk(W.m);
            W.cv.wait(lk, [&] { return W.quit || W.hasIn; });
            if (W.quit) break;
            local = W.inBuf; idx = W.inIndex; frameId = W.inFrame; W.hasIn = false;
        }
        auto t0 = Clock::now();
        bool delivered = false;   // 本帧是否把答案交付给了 A（B 形态下恒为 false）

        // 模型切换：菜单/文件请求了不同模型 → 在本帧之前重建会话（旧会话出的帧已经交付，不打断 present）
        if (W.modelApplied != g_modelSel) {
            int want = g_modelSel;
            Log("worker: 收到换模型请求（%s），开始重建会话", want ? "Original 71-block" : "Small 31-block");
            WorkerRebuildForModel(want);
        }
        // S4：菜单改了风格参数 → 重新下发控制块（很便宜，不需要重建会话）
        if (g_ctrlDirty) ApplyMochiControls();

        if (!W.upPtr && !(g_xRead && g_xOk && g_xBufInW)) continue;
        if (!(g_xRead && g_xOk && g_xBufInW))               // S6-2：共享路径下不需要 CPU 拷贝
            memcpy(W.upPtr, local.data(), local.size());

        if (W.api.PrepareFrame) {
            // ★ 双分配器轮转：本轮换用另一个分配器，并确认它上次那批命令已经执行完（有界等待）。
            //   以前只有一个分配器、Reset 也不检查 → D3D12 非法用法，偶发丢帧/长帧的嫌疑就在这。
            W.ai ^= 1;
            W.curAl = W.ai ? W.al2.Get() : W.al.Get();
            if (W.aiFv[W.ai]) {
                ULONGLONG tw0 = GetTickCount64();
                while (W.fe->GetCompletedValue() < W.aiFv[W.ai] && (GetTickCount64() - tw0) < 25)
                    SwitchToThread();
            }
            W.curAl->Reset(); W.cl->Reset(W.curAl, nullptr);
            {
                D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = W.color.Get();
                d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
                static bool loggedR4 = false;
                if (g_xRead && g_xOk && g_xBufInW) {
                    D3D12_TEXTURE_COPY_LOCATION s{};
                    s.pResource = g_xBufInW.Get();
                    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    s.PlacedFootprint.Offset = XHDR;
                    s.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    s.PlacedFootprint.Footprint.Width = kNetW;
                    s.PlacedFootprint.Footprint.Height = kNetH;
                    s.PlacedFootprint.Footprint.Depth = 1;
                    s.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
                    W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                    if (!loggedR4) { loggedR4 = true; Log("R4: worker 已改为从跨卡共享 buffer 读输入"); }
                } else {
                    D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = W.up.Get();
                    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = W.fp;
                    W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                }
                // 【B 形态 S2/S3】把 A 送来的原始 pColor/MV/深度 解成本地纹理，
                // 然后**在 B 上自己降采样**喂给 NR —— 不再依赖 A 传过来的 1080p
                if (g_bformOn && g_bformReady && g_upBufW && g_bformStage >= 2) {
                    static bool dsFail = false, compFail = false, upFail = false, ppFail = false;
                    if (g_bformStage >= 7) {
                        // 【流水线 Stage1】解码 + 降采样 → 本组缓冲；NR 输入也在这一步准备好
                        if (!ppFail) {
                            if (!BformPPEnsure(W.dev.Get()) || !BformBEnsureDS(W.dev.Get()) ||
                                !BformBEnsureComp(W.dev.Get()) || !BformBEnsureUp(W.dev.Get())) ppFail = true;
                            else BformPPStage1(W.cl.Get(), g_ppCur, W.color.Get(), nullptr, nullptr);
                        }
                    } else {
                        // 读【上一帧写完的槽】：比最新槽安全（A 的跨卡拷贝肯定已完成）
                        const int decSlot = (g_upLastSlot + K - 1) % K;
                        // ★ 防饿死门：如果 A 已经跑到我们前面 K-2 帧以外，这一格可能正被改写/已被绕圈覆盖
                        //   ⇒ 解码会读到"半新半旧"的底图（撕裂帧）。这时本轮跳过【合成】，画面保持上一帧，
                        //     宁可少更新一次，也不出一帧撕裂。（解码/喂网络照旧，不影响恢复）
                        {
                            static int lastDecF = 0;
                            int aNewF = g_slotTag[g_upLastSlot % K].frame;
                            g_bStarved = (lastDecF > 0) && ((aNewF - lastDecF) >= (K - 2));
                            if (g_bStarved) {
                                static int starve = 0;
                                if ((starve++ % 60) == 0)
                                    Log("!! 防饿死：B 落后 %d 帧（>=K-2）→ 本轮跳过合成，累计 %d 次",
                                        aNewF - lastDecF, starve);
                            }
                            lastDecF = g_slotTag[decSlot].frame;   // 记下本解码到的帧号
                        }
                        // ★ 等 A 那边"4K 颜色拷贝"完成才解码（跨卡共享围栏；GPU 侧等待，不占 CPU）
                        {
                            static bool xfTried = false;
                            if (!W.xferFe && g_xqShare && !xfTried) {
                                xfTried = true;
                                if (FAILED(W.dev->OpenSharedHandle(g_xqShare, IID_PPV_ARGS(W.xferFe.GetAddressOf()))))
                                    Log("XFER: B 端打开共享围栏失败（退回靠滞后）");
                                else Log("XFER: B 端已接上跨卡就绪围栏");
                            }
                            UINT64 need = g_slotReadyFv[decSlot];
                            if (need && W.xferFe) W.q->Wait(W.xferFe.Get(), need);
                        }
                        DumpFlush(1);
                        BformBDecode(W.cl.Get(), decSlot);
                        DumpRecord(W.cl.Get(), W.dev.Get(), 1, g_bTexC.Get(), D3D12_RESOURCE_STATE_COMMON);
                        // 【面单校验】把"A 最新写的是第几帧 / B 这轮解码的是第几帧 / 落后几帧"打出来。
                        //   以前这一层完全不可见：配错了只会表现为"残差怪"，不会报错。
                        {
                            static int pairCnt = 0;
                            if ((pairCnt++ % 150) == 0) {
                                int aNew = g_slotTag[g_upLastSlot % K].frame;
                                int bDec = g_slotTag[decSlot].frame;
                                Log("BFORM-PAIR: A 最新写第 %d 帧（槽%d）| B 解码第 %d 帧（槽%d）| 落后 %d 帧 | jitter=%.2f,%.2f",
                                    aNew, (int)(g_upLastSlot % K), bDec, decSlot,
                                    aNew - bDec, (float)g_slotTag[decSlot].jx, (float)g_slotTag[decSlot].jy);
                            }
                        }
                        // ★ 每个 Ensure 只尝试一次：失败就永久跳过，否则会每帧重编译 shader（FPS 曾被打到 18）
                        if (g_bformStage >= 3 && !dsFail) {
                            if (!BformBEnsureDS(W.dev.Get())) dsFail = true;
                            else BformBDownsample(W.cl.Get(), W.color.Get());
                        }
                        if (g_bformStage >= 4) {
                            if (!compFail && !BformBEnsureComp(W.dev.Get())) compFail = true;
                            if (!upFail && !BformBEnsureUp(W.dev.Get())) upFail = true;
                        }
                    }
                }
            }
            Job job{};
            job.struct_size = (uint32_t)sizeof(Job);
            FrameInfo160 fi{};
            fi.struct_size = W.frameSize;   // 跟随 ABI：160 / 104 / 80
            // 注：下面的 exposure/dLSS 自造字段（depth/velocity/ctl*）写在 104 之后，声明尺寸更小时运行时不读，无副作用
            fi.session_id = 1; fi.frame_id = (uint64_t)W.processed + 1; fi.list_generation = 1;
            fi.command_list = W.cl.Get();
            fi.color_width = kNetW; fi.color_height = kNetH;
            fi.color = W.color.Get();
            fi.color_state = (uint32_t)D3D12_RESOURCE_STATE_COMMON;
            // flags：bit0=强度生效 bit1=调试视图生效（否则这两个字段会被运行时忽略）
            {
                uint32_t fl = 0;
                if (g_transfer < 0.999f || g_colorStr < 0.999f) fl |= 1u;   // STRENGTH
                if (g_debugView != 0) fl |= 2u;                                // DEBUG_VIEW
                fi.flags = fl;
            }
            fi.transfer_strength = (float)g_transfer; fi.color_strength = (float)g_colorStr;
            fi.debug_view = (uint32_t)g_debugView;
            fi.model_scale = 1.0f; fi.pre_exposure = 1.0f; fi.exposure_scale = 1.0f;
            fi.depth = nullptr; fi.velocity = nullptr;
            fi.ctlTone = 1.0f; fi.ctlStructure = 1.0f; fi.ctlSkin = 3.0f; fi.ctlOther = 2.0f; fi.ctlStyle = 0.0f;

            // rc=4 = "building the network"（mochizuki 首次建网要几百毫秒~几十秒）。
            // 这时不能把它当成"这一帧失败"丢掉：丢掉会导致下游不再喂帧（第一帧永远没有答案）。
            // 正确做法是原地重试同一帧（官方 call-order 文档也是"PrepareFrame 很便宜"）。
            int32_t r;
            int waits = 0;
            for (;;) {
                r = W.api.PrepareFrame(W.ctx, &fi, &job);
                if (r != 4 || waits >= 1200) break;      // 最多等 120 秒
                if (waits == 0) Log("worker: 运行时还在建网络，原地重试同一帧（不丢帧）");
                Sleep(100);
                ++waits;
                job = Job{}; job.struct_size = (uint32_t)sizeof(Job);
            }
            if (waits > 0) Log("worker: 建网络等待结束（等了 %d×100ms，rc=%d）", waits, (int)r);
            if (W.processed < 3) Log("worker: [%d] PrepareFrame 返回 %d", W.processed, (int)r);
            if (r != 0) {
                // 关键：PrepareFrame 失败（例如 rc=4 "building the network"）也可能留下了一个 job 句柄，
                // mochizuki 在下一次 PrepareFrame 里会等上一个 job 被交还 —— 不归还就会永久阻塞。
                if (job.handle) {
                    int32_t cr = -1;
                    if (W.api.CancelUnsubmitted) cr = W.api.CancelUnsubmitted(W.ctx, job.handle);
                    else if (W.api.Retire)       cr = W.api.Retire(W.ctx, job.handle);
                    if (W.processed < 3) Log("worker: [%d] 释放失败 job handle（CancelUnsubmitted -> %d）",
                                             W.processed, (int)cr);
                }
                if (W.processed < 3 || (W.processed % 150) == 0) {
                    char eb[256] = { 0 }; W.api.GetLastError(eb, 256);
                    Log("worker: PrepareFrame -> %d err=%s（第 %d 帧）", (int)r, eb, W.processed);
                }
            } else {
                if (W.processed < 3) Log("worker: [%d] 进入 RecordInputs", W.processed);
                W.api.RecordInputs(W.ctx, job.handle, W.cl.Get());
                W.cl->Close();
                { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                // 【流水线 Stage2】趁着 NR 还在 GPU 上跑，把【上一帧】的 合成+上采样+出画 发到 g_bpQ
                //   读的是另一组缓冲，与 Stage1(N) 不冲突；这是本方案唯一能真正并行的地方
                if (g_bformOn && g_bformStage >= 7 && g_ppReady && g_bpReady && g_ppHavePrev)
                    BformPPStage2(1 - g_ppCur);
                if (!WaitW() && W.processed < 3) Log("worker: [%d] producer 提交后等待超时", W.processed);
                if (W.processed < 3) Log("worker: [%d] 进入 ExecuteAfterProducer", W.processed);
                if (W.api.ExecuteAfterProducer(W.ctx, job.handle, W.q.Get()) == 0) {
                    if (W.processed < 3) Log("worker: [%d] ExecuteAfterProducer ok，进入 RecordOutputs", W.processed);
                    W.curAl->Reset(); W.cl->Reset(W.curAl, nullptr);
                    W.api.RecordOutputs(W.ctx, job.handle, W.cl.Get());
                    W.cl->Close();
                    { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }
                    if (!WaitW() && W.processed < 3) Log("worker: [%d] consumer 提交后等待超时", W.processed);

                    ID3D12Resource* po = (ID3D12Resource*)job.private_output;
                    if (po) {
                        D3D12_RESOURCE_DESC pd = po->GetDesc();
                        D3D12_RESOURCE_DESC pl = pd; pl.Flags = D3D12_RESOURCE_FLAG_NONE;
                        UINT r2 = 0; UINT64 rs2 = 0, tot2 = 0;
                        W.dev->GetCopyableFootprints(&pl, 0, 1, 0, &W.fpPriv, &r2, &rs2, &tot2);
                        {   // 【诊断·一次性】把 NR 答案纹理的真实描述打出来：格式/尺寸/数组/行距
                            //   我们建 SRV 时写死 R8G8B8A8_UNORM（BformBCompose:1917），若真实格式不同，
                            //   shader 采到的就不是答案 —— 这是"B 屏无效果"的第一号嫌疑，先量掉它
                            static bool loggedPoDesc = false;
                            if (!loggedPoDesc) {
                                loggedPoDesc = true;
                                Log("DIAG-PO: 答案纹理 fmt=%d %ux%u array=%u mip=%u samples=%u 行距=%u",
                                    (int)pd.Format, (UINT)pd.Width, (UINT)pd.Height, pd.DepthOrArraySize,
                                    pd.MipLevels, pd.SampleDesc.Count, W.fpPriv.Footprint.RowPitch);
                            }
                        }
                        if (!W.rbPriv || W.bytesPriv != tot2) {
                            D3D12_RESOURCE_DESC bd2{};
                            bd2.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                            bd2.Width = tot2; bd2.Height = 1; bd2.DepthOrArraySize = 1; bd2.MipLevels = 1;
                            bd2.Format = DXGI_FORMAT_UNKNOWN; bd2.SampleDesc.Count = 1;
                            bd2.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                            D3D12_HEAP_PROPERTIES rhb{}; rhb.Type = D3D12_HEAP_TYPE_READBACK;
                            rhb.CreationNodeMask = 1; rhb.VisibleNodeMask = 1;
                            W.rbPriv.Reset();
                            W.dev->CreateCommittedResource(&rhb, D3D12_HEAP_FLAG_NONE, &bd2,
                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(W.rbPriv.GetAddressOf()));
                            W.bytesPriv = tot2;
                        }
                        W.curAl->Reset(); W.cl->Reset(W.curAl, nullptr);
                        // 【B 形态 S4b】答案刚出来 → 立刻在 B 上做残差合成（用本帧收到的 pColor/MV + 本地降采样）
                        if (g_bformOn && g_bformStage >= 4 && (g_bCompReady || g_bform4k) && !g_bStarved) {
                            if (g_bform4k) {
                                // 【bform=9】4K 后置：残差直接合成到 4K 底图上（g_bTexC 就是 XeSS 的 4K 输出）
                                //   BformBCompose 的输出尺寸取自 g_upW/g_upH = 4K，所以这里天然是 4K 合成；
                                //   合成后不走放大（底图已经是 4K），present 直接取 g_bComp。
                                BformBCompose(W.cl.Get(), po);
                            } else {
                            BformBCompose(W.cl.Get(), po);
                            if (g_bformStage >= 5 && g_bUpReady) BformBUpscale(W.cl.Get());
                            // 【TAA-lite】bform=8 时，在上采样之后再多走一个时域累积 pass（治噪点/锯齿）
                            if (g_taaOn && g_bUpReady) {
                                static bool taaFail = false;
                                if (!taaFail) {
                                    if (!BformTaaEnsure(W.dev.Get())) taaFail = true;
                                    else if (!BformTaaRun(W.cl.Get())) taaFail = true;
                                }
                            }
                            }
                        }
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = po;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        W.cl->ResourceBarrier(1, &b);
                        static bool loggedR5 = false;
                        // 【流水线】把 NR 的答案快照进本组缓冲（po 下一帧就被运行时复用，必须拷走）
                        if (g_bformOn && g_bformStage >= 7 && g_ppReady && g_ppAns[g_ppCur]) {
                            D3D12_RESOURCE_BARRIER pb{};
                            pb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                            pb.Transition.pResource = g_ppAns[g_ppCur].Get();
                            pb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                            pb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                            pb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                            W.cl->ResourceBarrier(1, &pb);
                            W.cl->CopyResource(g_ppAns[g_ppCur].Get(), po);
                            pb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                            pb.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                            W.cl->ResourceBarrier(1, &pb);
                        }
                        // ★ mode 9（4K 后置）下 A 不合成，答案不需要回传 → 跳过这次 8.3MB 的跨卡直写，
                        //   省 B 的队列时间 + 省 PCIe 带宽（和 A→B 的底图传输抢带宽）。
                        //   代价：热切回 A 形态后头几帧没现成答案（A 会等一次 ~20ms，之后正常）。
                        if (g_xOk && g_xBufOutW && !g_bform4k) {
                            D3D12_TEXTURE_COPY_LOCATION xd{};
                            xd.pResource = g_xBufOutW.Get();
                            xd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                            xd.PlacedFootprint.Offset = XHDR + (UINT64)idx * (UINT64)(kNetW * kNetH * 4);
                            xd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            xd.PlacedFootprint.Footprint.Width = kNetW;
                            xd.PlacedFootprint.Footprint.Height = kNetH;
                            xd.PlacedFootprint.Footprint.Depth = 1;
                            xd.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
                            D3D12_TEXTURE_COPY_LOCATION xs5{};
                            xs5.pResource = po;
                            xs5.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            xs5.SubresourceIndex = 0;
                            W.cl->CopyTextureRegion(&xd, 0, 0, 0, &xs5, nullptr);
                            if (!loggedR5) { loggedR5 = true; Log("R5: 答案已 GPU 直写进跨卡共享 buffer"); }
                        }
                        D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = W.rbPriv.Get();
                        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = W.fpPriv;
                        D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = po;
                        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
                        W.cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                        W.cl->ResourceBarrier(1, &b);
                        // 【诊断】五源同块对照的"拷贝"必须挂在这个 list 上（跨队列碰同一批资源 = S1 崩过）
                        BformDiagEnqueue(W, po);
                        W.cl->Close();
                        { ID3D12CommandList* l[] = { W.cl.Get() }; W.q->ExecuteCommandLists(1, l); }

                        // 【2026-10-28 14:40 回退：不等围栏】
                        //   查明：A 的合成用的是 B 侧 GPU 直写进跨卡 buffer 的那份答案（use_gpu_resid=1），
                        //   本来就没有错帧；CPU 回读这份只喂诊断和 CPU 残差路径（未启用）。
                        //   为它牺牲帧率不划算 → 回到"提交完立刻 Map"。
                        //   已知代价：DIAGA 诊断数字偏高（它比的是错帧），不要用它下画质结论。
                        WaitW();   // Signal 围栏（只记账，不等待）
                        W.aiFv[W.ai] = W.fv;   // ★ 记下本分配器的围栏值：下次轮到它复用前要先确认已完成

                        // 出画（无条件；围栏是刚 Signal 的，不能用来当条件）
                        if (g_bformOn && g_bformStage >= 6 && g_bUpReady) {
                            if (!g_bpReady && !g_bpFailed) BformPresentInit();
                            if (g_bpReady) BformPresentFrame();
                        }

                        // 回读本轮答案并交付给 A（不等围栏）
                        {
                            D3D12_RANGE rr{ 0, (SIZE_T)tot2 };
                            void* rm = nullptr;
                            if (SUCCEEDED(W.rbPriv->Map(0, &rr, &rm)) && rm) {
                                // ===== 一次性诊断（只在游戏目录存在 xess_proxy.podiag.txt 时生效）=====
                                //   目的：把运行时答案（private_output）的真实字节打出来，
                                //   与我们在 FFX 代理里的同口径观测对照，判断"答案到底是不是浮点残差"。
                                {
                                    static bool diagDone = false;
                                    static int diagGate = -1;
                                    if (diagGate < 0) {
                                        std::string gate = ModuleDir() + "xess_proxy.podiag.txt";
                                        diagGate = (GetFileAttributesA(gate.c_str()) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
                                    }
                                    static int diagShot = 0;
                                    const bool shoot = (diagGate == 1) &&
                                                       ((diagShot == 0 && W.processed >= 60) ||
                                                        (diagShot == 1 && W.processed >= 300) ||
                                                        (diagShot == 2 && W.processed >= 600));
                                    if (shoot) {
                                        diagShot++;
                                        const unsigned char* b0 = (const unsigned char*)rm;
                                        D3D12_RESOURCE_DESC pd2 = ((ID3D12Resource*)job.private_output)->GetDesc();
                                        Log("DIAG-POB: po fmt=%d %ux%u RowPitch=%u | 前16字节= %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                                            (int)pd2.Format, (unsigned)pd2.Width, (unsigned)pd2.Height, (unsigned)W.fpPriv.Footprint.RowPitch,
                                            b0[0],b0[1],b0[2],b0[3],b0[4],b0[5],b0[6],b0[7],
                                            b0[8],b0[9],b0[10],b0[11],b0[12],b0[13],b0[14],b0[15]);
                                        float f0 = 0; memcpy(&f0, b0, 4);
                                        Log("DIAG-POB2: 帧%d 按 R32_FLOAT 首像素 = %.6g ；outBuf[0..3]=%u,%u,%u,%u",
                                            (int)W.processed, f0, W.outBuf[0], W.outBuf[1], W.outBuf[2], W.outBuf[3]);
                                        {   // 前 4096 字节里有多少非零；前 1024 个 float 的 min/max/mean
                                            int nz = 0; for (int k = 0; k < 4096; ++k) if (b0[k]) ++nz;
                                            float mn = 1e30f, mx = -1e30f; double sum = 0; int n = 0;
                                            for (int k = 0; k < 1024; ++k) { float v; memcpy(&v, b0 + k * 4, 4);
                                                if (v < mn) mn = v; if (v > mx) mx = v; sum += v; ++n; }
                                            Log("DIAG-POB3: 帧%d 前4096字节非零=%d/4096 | float min=%.6g max=%.6g mean=%.6g",
                                                (int)W.processed, nz, mn, mx, n ? sum / n : 0.0);
                                        }
                                    }
                                }
                                UINT pitch = W.fpPriv.Footprint.RowPitch;
                                for (UINT y = 0; y < kNetH; ++y)
                                    memcpy(&W.outBuf[(size_t)y * kNetW * 4],
                                           (unsigned char*)rm + (size_t)y * pitch, (size_t)kNetW * 4);
                                D3D12_RANGE nw{ 0, 0 };
                                W.rbPriv->Unmap(0, &nw);
                                {
                                    std::lock_guard<std::mutex> lk(W.m);
                                    if (idx >= 0 && idx < K && (int)W.ansBuf.size() == K) {
                                        memcpy(W.ansBuf[idx].data(), W.outBuf.data(),
                                               (std::min)(W.ansBuf[idx].size(), W.outBuf.size()));
                                        W.ansFrame[idx] = frameId;
                                    }
                                }
                                W.cvOut.notify_all();
                                delivered = true;
                            }
                        }
                        W.hasOut = delivered;

                        // 【诊断】回读 + 打一行五源对照（每 150 帧一次）
                        BformDiagReadback(W);
                    }
                    W.api.Retire(W.ctx, job.handle);
                }
                // 【流水线】本轮结束：切到另一组缓冲，下一轮 Stage2 就处理这一帧的结果
                if (g_bformOn && g_bformStage >= 7 && g_ppReady) {
                    g_ppCur ^= 1;
                    g_ppHavePrev = true;
                }
                g_workerBeat = GetTickCount64();   // 看门狗心跳：每轮结束更新一次
            }
        }
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        {
            std::lock_guard<std::mutex> lk(W.m);
            W.outIndex = idx;
            W.processed++;
            W.lastMs = ms;
            if (W.processed <= 3 || W.processed % 150 == 0) {
                char sb[256] = { 0 };
                if (W.api.GetStatus) W.api.GetStatus(W.ctx, sb, 256);
                Log("worker: #%d(槽%d) %.1f ms %s | 状态: %s",
                    W.processed, idx, ms, delivered ? "ok" : "无输出", sb[0] ? sb : "(空)");
                strncpy_s(g_statusLine, sizeof(g_statusLine), sb[0] ? sb : "(空)", _TRUNCATE);
            }
            W.cvOut.notify_all();
        }
    }
}

// ---------------- 游戏侧 ----------------
struct Slot {
    ComPtr<ID3D12Resource> color;    // 本帧原始 pColor
    ComPtr<ID3D12Resource> mv;       // 本帧 MV
    ComPtr<ID3D12Resource> depth;    // 本帧深度
    float jx = 0, jy = 0;
    int   frame = -1;
    bool  captured = false;
    bool  hasAnswer = false;
    int   answerFrame = -1;          // 这份答案对应哪一帧（身份校验）
    std::vector<uint8_t> answer;     // 1080p 答案
    std::vector<uint8_t> feedInput;  // 1080p 喂进去的输入（算残差基准）
};
static Slot g_slots[K];

static ComPtr<ID3D12Device> g_dev;
static ComPtr<ID3D12RootSignature> g_rs;
static ComPtr<ID3D12PipelineState> g_psoDown, g_psoResid, g_psoAdd4K, g_psoMvPack;
static ComPtr<ID3D12DescriptorHeap> g_heap;
static UINT g_inc = 0;
static D3D12_CPU_DESCRIPTOR_HANDLE g_cpuResSmooth{};
static ComPtr<ID3D12Resource> g_netS;      // 1080p 降采样目标
static ComPtr<ID3D12Resource> g_pre;       // pColor 几何的合成目标
static ComPtr<ID3D12Resource> g_pre4k;     // B1：4K 叠加结果（outp 几何）
static bool g_resid4k = false;             // B1 开关：残差改在 4K 上叠加（默认关）
static ComPtr<ID3D12Resource> g_resTex;    // 1080p 残差（上传）
static ComPtr<ID3D12Resource> g_resSmooth; // 残差时域状态
static ComPtr<ID3D12Resource> g_resUp, g_rb;   // 上传/回读缓冲
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fpRes{}, g_fpRb{};
static UINT64 g_bytesRes = 0, g_bytesRb = 0;
static UINT g_cw = 0, g_ch = 0, g_outW = 0, g_outH = 0;
static DXGI_FORMAT g_cfmt = DXGI_FORMAT_UNKNOWN;
static int g_frame = 0;
static int g_fedIdx = 0;         // 已喂出的作业序号
static double g_sumWait = 0; static int g_nWait = 0;

static volatile float g_limit = 0.2f, g_edge = 0.6f, g_smooth = 0.0f, g_gain = 0.66f, g_sat = 0.9f;
static volatile float g_temporalStatic = 0.02f;  // 静止时的时域混合（1.0=关，0.20=最强；实测 0.20 完全消除闪烁）
static volatile float g_motionAlpha = 0.20f;  // 运动时的 alpha 上限（不再到 1.0；由 conf/gate 决定实际历史量）
static volatile float g_motionLo = 0.02f;   // 运动量下界（低于此=完全平滑）
static volatile float g_motionHi = 0.04f;   // 运动量上界（实测 0.04：静止不闪 + 运动无残影）
static volatile float g_alpha = 1.0f;             // 本帧实际用的 alpha（CPU 按运动量算）
static volatile float g_varGain = 0.0f;           // 方差门限强度（F6）
static int g_stateWarmup = 4;
static volatile float g_mvScale = -0.00045f;      // MV 单位校准（实测最佳：像素单位+反向）
static volatile float g_mvConf  = 1.0f;          // MV 置信度衰减（实测最佳）
static volatile int   g_mvScaleIdx = 5, g_mvConfIdx = 1;   // 当前档位（屏幕显示用）
static volatile float g_gateK = 8.0f;    // 实测 8 + motionHi 0.04：静止不闪且运动无拖影（40 会小幅闪、0 会拖影）
static volatile int   g_gateIdx = 4;
static ComPtr<ID3D12Resource> g_dumpRb1, g_dumpRb2;
static UINT64 g_dumpBytes = 0;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_dumpFp{};
static int g_dumpPhase = 0;                       // 0=未开始 1=已记录拷贝 2=已完成

// ---------------- S1 镜像叠层开关（ini 键 mirror / 菜单勾选 / F11） ----------------
// 纯观测功能：把游戏已出好的成品帧再叠一层到游戏窗口上，用来测「自己拥有 present」的代价。
// 默认关；关着的时候一行代码都不执行。
static bool g_mirOn = false;
static int  g_mirState = 0;     // 0=未建 3=已关(可再开) 1=就绪 2=失败(不再重试)

// ---------------- 参数快照 / 用户存档（菜单：保存 / 撤销 / 恢复 / 出厂） ----------------
// 三层语义，互不污染：
//   出厂默认 = 代码里的常量（FactoryParams）—— 任何按钮都不会改它
//   用户存档 = xess_proxy.saved.ini —— “保存设置”写入，独立文件，不覆盖出厂
//   启动状态 = 本次游戏启动、首次跑 NR 时的快照 —— “撤销改动”回到这里
struct Params {
    float gain, limit, edge, sat, temporal, motionAlpha, motionLo, motionHi;
    float mvScale, mvConf, gateK, transfer, colorStr, jitter, dejitterSign, uiScale, varGain;
    int   debugView;
    bool  dejitter, bypass, useGpuRes;
    // S4 风格化（发给 mochizuki 控制块）
    float nrIntensity, nrTone, nrStruct, nrSkin, nrGrade;
    int   nrStyle;
    bool  nrAutoMask, nrCtrlOn;
    bool  mirrorOn;          // S1 镜像叠层
};
static Params g_sessionStart{};
static bool   g_sessionStartValid = false;

static Params CaptureParams() {
    Params p{};
    p.gain = g_gain; p.limit = g_limit; p.edge = g_edge; p.sat = g_sat;
    p.temporal = g_temporalStatic; p.motionAlpha = g_motionAlpha;
    p.motionLo = g_motionLo; p.motionHi = g_motionHi;
    p.mvScale = g_mvScale; p.mvConf = g_mvConf; p.gateK = g_gateK;
    p.transfer = g_transfer; p.colorStr = g_colorStr; p.jitter = g_jitterScale;
    p.dejitterSign = g_dejitterSign; p.uiScale = g_uiScale; p.varGain = g_varGain;
    p.debugView = g_debugView; p.dejitter = g_dejitter; p.bypass = g_bypass; p.useGpuRes = g_useGpuResid;
    p.nrIntensity = g_nrIntensity; p.nrTone = g_nrTone; p.nrStruct = g_nrStruct; p.nrSkin = g_nrSkin;
    p.nrGrade = g_nrGrade; p.nrStyle = g_nrStyle; p.nrAutoMask = g_nrAutoMask; p.nrCtrlOn = g_nrCtrlOn;
    p.mirrorOn = g_mirOn;
    return p;
}
static void ApplyParams(const Params& p) {
    g_gain = p.gain; g_limit = p.limit; g_edge = p.edge; g_sat = p.sat;
    g_temporalStatic = p.temporal; g_stateWarmup = 4;
    g_motionAlpha = p.motionAlpha; g_motionLo = p.motionLo; g_motionHi = p.motionHi;
    g_mvScale = p.mvScale; g_mvConf = p.mvConf; g_gateK = p.gateK;
    g_transfer = p.transfer; g_colorStr = p.colorStr; g_jitterScale = p.jitter;
    g_dejitterSign = p.dejitterSign; g_uiScale = p.uiScale; g_varGain = p.varGain;
    g_debugView = p.debugView; g_dejitter = p.dejitter; g_bypass = p.bypass; g_useGpuResid = p.useGpuRes;
    g_nrIntensity = p.nrIntensity; g_nrTone = p.nrTone; g_nrStruct = p.nrStruct; g_nrSkin = p.nrSkin;
    g_nrGrade = p.nrGrade; g_nrStyle = p.nrStyle; g_nrAutoMask = p.nrAutoMask; g_nrCtrlOn = p.nrCtrlOn;
    if (g_mirOn != p.mirrorOn) Log("MIR: 镜像叠层 %s（来自参数应用）", p.mirrorOn ? "开" : "关");
    g_mirOn = p.mirrorOn;
    g_ctrlDirty = true;
}
static Params FactoryParams() {
    Params p{};
    p.gain = 0.66f; p.limit = 0.20f; p.edge = 0.6f; p.sat = 0.9f;
    p.temporal = 0.02f; p.motionAlpha = 0.20f; p.motionLo = 0.02f; p.motionHi = 0.04f;
    p.mvScale = -0.00045f; p.mvConf = 1.0f; p.gateK = 8.0f;
    p.transfer = 1.0f; p.colorStr = 1.0f; p.jitter = 0.5f;
    p.dejitterSign = 1.0f; p.uiScale = 1.0f; p.varGain = 0.0f;
    p.debugView = 0; p.dejitter = false; p.bypass = false; p.useGpuRes = true;
    // ★ 出厂默认 = 本项目实测调好的值（不是上游的裸默认）：
    //   intensity 2.0 / grade 2.0 —— 用户按「恢复默认设置」应当回到这套可用的观感。
    //   （mochizuki 上游缺省是 grade 1.0，效果很弱；1.x 版本曾按上游给 1.0，导致"恢复默认"看起来变淡。）
    p.nrIntensity = 2.0f; p.nrTone = 1.0f; p.nrStruct = 1.0f; p.nrSkin = -1.0f; p.nrGrade = 2.0f;
    p.nrStyle = 0; p.nrAutoMask = false; p.nrCtrlOn = true;
    p.mirrorOn = false;
    return p;
}
static bool SaveParamsToFile(const Params& p) {
    std::string path = ModuleDir() + "xess_proxy.saved.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f) { Log("菜单: 保存失败（打不开 %s）", path.c_str()); return false; }
    fprintf(f, "; 跨卡 NR 用户存档（菜单“保存设置”写入；出厂默认在代码里，不会被这里覆盖）\n");
    fprintf(f, "gain=%.6f\nlimit=%.6f\nedge=%.6f\nsat=%.6f\ntemporal=%.6f\n", p.gain, p.limit, p.edge, p.sat, p.temporal);
    fprintf(f, "motion_alpha=%.6f\nmotion_lo=%.6f\nmotion_hi=%.6f\n", p.motionAlpha, p.motionLo, p.motionHi);
    fprintf(f, "mv_scale=%.8f\nmv_conf=%.6f\ngate_k=%.6f\n", p.mvScale, p.mvConf, p.gateK);
    fprintf(f, "transfer=%.6f\ncolour=%.6f\njitter=%.6f\nde_jitter_sign=%.1f\n", p.transfer, p.colorStr, p.jitter, p.dejitterSign);
    fprintf(f, "ui_scale=%.6f\nvar_gain=%.6f\n", p.uiScale, p.varGain);
    fprintf(f, "nr_intensity=%.6f\nnr_style=%d\nnr_tone=%.6f\nnr_struct=%.6f\nnr_skin=%.6f\nnr_grade=%.6f\nnr_automask=%d\nnr_ctrl_on=%d\n",
            p.nrIntensity, p.nrStyle, p.nrTone, p.nrStruct, p.nrSkin, p.nrGrade, (int)p.nrAutoMask, (int)p.nrCtrlOn);
    fprintf(f, "debug_view=%d\nde_jitter=%d\nbypass=%d\nuse_gpu_resid=%d\n",
            p.debugView, (int)p.dejitter, (int)p.bypass, (int)p.useGpuRes);
    fprintf(f, "mirror=%d\n", (int)p.mirrorOn);
    fclose(f);
    Log("菜单: 已保存设置 → %s", path.c_str());
    return true;
}
static bool LoadParamsFromFile(Params& p) {   // 只覆盖文件里出现的键
    std::string path = ModuleDir() + "xess_proxy.saved.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f) { Log("菜单: 没有存档（%s）", path.c_str()); return false; }
    char line[256]; int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char* k = line; while (*k == ' ' || *k == '\t') ++k;
        const char* v = eq + 1;
        float fv = (float)atof(v); int iv = atoi(v);
        if (!strcmp(k, "gain")) p.gain = fv;
        else if (!strcmp(k, "limit")) p.limit = fv;
        else if (!strcmp(k, "edge")) p.edge = fv;
        else if (!strcmp(k, "sat")) p.sat = fv;
        else if (!strcmp(k, "temporal")) p.temporal = fv;
        else if (!strcmp(k, "motion_alpha")) p.motionAlpha = fv;
        else if (!strcmp(k, "motion_lo")) p.motionLo = fv;
        else if (!strcmp(k, "motion_hi")) p.motionHi = fv;
        else if (!strcmp(k, "mv_scale")) p.mvScale = fv;
        else if (!strcmp(k, "mv_conf")) p.mvConf = fv;
        else if (!strcmp(k, "gate_k")) p.gateK = fv;
        else if (!strcmp(k, "transfer")) p.transfer = fv;
        else if (!strcmp(k, "colour")) p.colorStr = fv;
        else if (!strcmp(k, "jitter")) p.jitter = fv;
        else if (!strcmp(k, "de_jitter_sign")) p.dejitterSign = fv;
        else if (!strcmp(k, "ui_scale")) p.uiScale = fv;
        else if (!strcmp(k, "var_gain")) p.varGain = fv;
        else if (!strcmp(k, "nr_intensity")) p.nrIntensity = fv;
        else if (!strcmp(k, "nr_style")) p.nrStyle = iv;
        else if (!strcmp(k, "nr_tone")) p.nrTone = fv;
        else if (!strcmp(k, "nr_struct")) p.nrStruct = fv;
        else if (!strcmp(k, "nr_skin")) p.nrSkin = fv;
        else if (!strcmp(k, "nr_grade")) p.nrGrade = fv;
        else if (!strcmp(k, "nr_automask")) p.nrAutoMask = (iv != 0);
        else if (!strcmp(k, "nr_ctrl_on")) p.nrCtrlOn = (iv != 0);
        else if (!strcmp(k, "debug_view")) p.debugView = iv;
        else if (!strcmp(k, "de_jitter")) p.dejitter = (iv != 0);
        else if (!strcmp(k, "bypass")) p.bypass = (iv != 0);
        else if (!strcmp(k, "use_gpu_resid")) p.useGpuRes = (iv != 0);
        else if (!strcmp(k, "mirror")) p.mirrorOn = (iv != 0);
        else continue;
        ++n;
    }
    fclose(f);
    Log("菜单: 已读取存档 %d 项（%s）", n, path.c_str());
    return n > 0;
}

static const char* kHLSL = R"(
cbuffer P : register(b0) { float gLimit; float gEdge; float gSmooth; float gGain;
                            float gSat; float gAlpha; float gVarGain; float gTemporal;
                            float gMvScale; float gMvConf; float gMvIdx; float gConfIdx;
                            float gGateK; float gGateIdx; float gBypass; float gUseGpuRes;
    float gJitX; float gJitY; float gDejitter; float gPadJ; };

Texture2D<float4>   gSrc  : register(t0);   // 槽位里的原始小图
Texture2D<float4>   gRes  : register(t1);   // 1080p 残差（0.5 中性）
Texture2D<float4>   gMV   : register(t2);
Texture2D<float4>   gFed  : register(t3);   // 该槽位被喂进去的 1080p 输入
Buffer<float4>      gAns  : register(t4);   // 答案（本地中转 buffer, RGBA8 有类型视图）
SamplerState        gSmp  : register(s0);
RWTexture2D<float4> gDst  : register(u0);
RWTexture2D<float4> gSt   : register(u1);   // 残差时域状态

// 小图 → 1080p（喂给网络）
[numthreads(8,8,1)]
void DownCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;
    float2 uv=(float2(id.xy)+0.5)/float2(w,h);
    // 去抖动：用该帧的 jitter 反向偏移采样，使 NR 看到"投影稳定"的图像
    if (abs(gDejitter) > 0.001) {
        uint sw, sh; gSrc.GetDimensions(sw, sh);
        uv += float2(gJitX, gJitY) / float2(max(sw,1), max(sh,1)) * gDejitter;
    }
    // 2x2 区域平均：源比目标大约 1.16 倍，单点采样会混叠
    float2 t = float2(0.25/1920.0, 0.25/1080.0) * 1.16;
    float4 c  = gSrc.SampleLevel(gSmp, uv + float2(-t.x,-t.y), 0);
    c += gSrc.SampleLevel(gSmp, uv + float2( t.x,-t.y), 0);
    c += gSrc.SampleLevel(gSmp, uv + float2(-t.x, t.y), 0);
    c += gSrc.SampleLevel(gSmp, uv + float2( t.x, t.y), 0);
    gDst[id.xy]=float4(c.rgb*0.25,1.0);
}

// 槽位原图 + 残差 → 合成后的 pColor
[numthreads(8,8,1)]
void ResidCS(uint3 id : SV_DispatchThreadID)
{
    uint w,h; gDst.GetDimensions(w,h);
    if (id.x>=w||id.y>=h) return;

    float4 c = gSrc.Load(int3(id.xy,0));
    float2 uv=(float2(id.xy)+0.5)/float2(w,h);

    // 直出模式：把运行时给的原样当作画面（配合 debug_view 看残差热图）
    if (gBypass > 0.5) { gDst[id.xy] = float4(gRes.SampleLevel(gSmp, uv, 0).rgb, 1.0); return; }

    // 输出平滑：残差在 1080p 上做 9 抽头
    float2 texel = float2(1.0/1920.0, 1.0/1080.0) * gSmooth;
    float3 e;
    if (gUseGpuRes > 0.5) {
        // S5-5：答案与输入都在 GPU 上，直接相减（本地 buffer 载荷从 0 开始）
        // 手工双线性：buffer 不能过滤采样，用 4 邻域插值，消除亚像素移动时的纹素跳变
        float2 fc = uv * float2(1920.0, 1080.0) - 0.5;
        float2 fl = floor(fc);
        int2 i0 = int2(fl);
        float2 wt = fc - fl;
        int2 c0 = clamp(i0,          int2(0, 0),       int2(1918, 1078));
        int2 c11 = clamp(i0 + int2(1, 1), int2(1, 1),  int2(1919, 1079));
        int2 c10 = int2(c11.x, c0.y);
        int2 c01 = int2(c0.x, c11.y);
        float w00 = (1.0 - wt.x) * (1.0 - wt.y);
        float w10 = wt.x * (1.0 - wt.y);
        float w01 = (1.0 - wt.x) * wt.y;
        float w11 = wt.x * wt.y;
        float4 a = gAns[c0.y*1920 + c0.x] * w00 + gAns[c0.y*1920 + c11.x] * w10
                 + gAns[c11.y*1920 + c0.x] * w01 + gAns[c11.y*1920 + c11.x] * w11;
        float4 f = gFed.Load(int3(c0, 0))  * w00 + gFed.Load(int3(c10, 0)) * w10
                 + gFed.Load(int3(c01, 0)) * w01 + gFed.Load(int3(c11, 0)) * w11;
        e = (a.rgb - f.rgb);
    } else if (gSmooth < 0.05) {
        e = gRes.SampleLevel(gSmp, uv, 0).rgb - 0.5;
    } else {
        e  = (gRes.SampleLevel(gSmp, uv + float2( 0.0, 1.0)*texel, 0).rgb - 0.5);
        e += (gRes.SampleLevel(gSmp, uv + float2( 0.0,-1.0)*texel, 0).rgb - 0.5);
        e += (gRes.SampleLevel(gSmp, uv + float2( 1.0, 0.0)*texel, 0).rgb - 0.5);
        e += (gRes.SampleLevel(gSmp, uv + float2(-1.0, 0.0)*texel, 0).rgb - 0.5);
        e += (gRes.SampleLevel(gSmp, uv, 0).rgb - 0.5);
        e /= 5.0;
    }
    // 去饱和（参考 AmdLmxxfEditSaturation=0.9）：残差只保留亮度分量为主，抑制彩色噪点
    float elum = dot(e, float3(0.2126, 0.7152, 0.0722));
    e = lerp(float3(elum, elum, elum), e, gSat);

    e *= gGain;

    // 限幅（参考 AmdResidualLimit）
    e = clamp(e, -gLimit, gLimit);

    // 局部方差门限（参考 AmdLmxxfEdgeGuard / 高光保护）：方差大的地方少改
    {
        float3 m1 = gSrc.Load(int3(min(id.x + 2, w - 1), (int)id.y, 0)).rgb;
        float3 m2 = gSrc.Load(int3(max((int)id.x - 2, 0), (int)id.y, 0)).rgb;
        float3 m3 = gSrc.Load(int3((int)id.x, min(id.y + 2, h - 1), 0)).rgb;
        float3 m4 = gSrc.Load(int3((int)id.x, max((int)id.y - 2, 0), 0)).rgb;
        float3 mean4 = (m1 + m2 + m3 + m4) * 0.25;
        float  var   = dot(abs(c.rgb - mean4), float3(0.3333, 0.3333, 0.3333));
        e *= 1.0 / (1.0 + gVarGain * var * 8.0);
    }

    // 时域稳定 + 运动矢量重投影（对齐 AMDNR 的 TemporalStability）
    {
        uint sw, sh; gSt.GetDimensions(sw, sh);
        int2 sxy = int2(saturate(uv) * float2(sw - 1, sh - 1) + 0.5);
        float2 mv = gMV.Load(int3(min(id.xy, int2(w - 1, h - 1)), 0)).rg * gMvScale;
        float2 prevUV = saturate(uv - mv);
        int2 pxy = clamp(int2(prevUV * float2(sw - 1, sh - 1) + 0.5), int2(0, 0), int2(sw - 1, sh - 1));
        float3 prev = gSt[pxy].rgb - 0.5;   // UAV 不能过滤采样，用最近邻
        float mlen = length(mv * 1920.0);
        float conf;
        if (gUseGpuRes > 0.5) {
            // 颜色一致性验证：重投影位置的原输入颜色是否与当前一致
            // 一致（物体内部，随物体一起移动）→ 可信 → 保留历史 → 亮度稳定
            // 不一致（遮挡/新露出）→ 不可信 → 丢历史 → 不拖影
            float3 pc = gRes.SampleLevel(gSmp, prevUV, 0).rgb;
            float  cd = dot(abs(c.rgb - pc), float3(0.3333, 0.3333, 0.3333));
            conf = saturate(1.0 - cd * gMvConf * 8.0);
        } else {
            conf = saturate(1.0 - mlen * gMvConf);
        }
        // 逐像素可信度：把重投影后的旧残差和当前残差比较，差太多就不用它（防拖影）
        float d = dot(abs(prev - e), float3(0.3333, 0.3333, 0.3333));
        float gate = saturate(1.0 - d * gGateK);
        float a = lerp(1.0, gAlpha, conf * gate);

        e = lerp(prev, e, a);
        gSt[sxy] = float4(e + 0.5, 1.0);
    }


    float3 outc = saturate(c.rgb + e);

gDst[id.xy] = float4(outc, c.a);
}

// ★ Add4K 已移出本文件 —— 现在从 xess_proxy.add4k.hlsl 读并在运行时编译（可热重编译）
)";

static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &br);
}

static void SetPass(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso,
                    UINT srv0, UINT srv1, UINT srv2, UINT srv3, UINT srv4,
                    UINT uav, UINT uav1, UINT w, UINT h) {
    cl->SetPipelineState(pso);
    cl->SetComputeRootSignature(g_rs.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE b = g_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE a1 = b; a1.ptr += (SIZE_T)srv0 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE a2 = b; a2.ptr += (SIZE_T)srv1 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE a3 = b; a3.ptr += (SIZE_T)srv2 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE a4 = b; a4.ptr += (SIZE_T)srv3 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE a5 = b; a5.ptr += (SIZE_T)srv4 * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE u  = b; u.ptr  += (SIZE_T)uav  * g_inc;
    D3D12_GPU_DESCRIPTOR_HANDLE u2 = b; u2.ptr += (SIZE_T)uav1 * g_inc;
    cl->SetComputeRootDescriptorTable(0, a1);
    cl->SetComputeRootDescriptorTable(1, a2);
    cl->SetComputeRootDescriptorTable(2, a3);
    cl->SetComputeRootDescriptorTable(3, a4);
    cl->SetComputeRootDescriptorTable(4, a5);
    cl->SetComputeRootDescriptorTable(5, u);
    cl->SetComputeRootDescriptorTable(6, u2);
    float dj = g_dejitter ? (float)g_dejitterSign : 0.0f;
    float c[20] = { g_limit, g_edge, g_smooth, g_gain, g_sat, g_alpha, g_varGain, g_temporalStatic,
                    g_mvScale, g_mvConf, (float)g_mvScaleIdx, (float)g_mvConfIdx,
                    g_gateK, (float)g_gateIdx, g_bypass ? 1.0f : 0.0f, g_useGpuResid ? 1.0f : 0.0f,
                    g_curJitX, g_curJitY, dj, 0.0f };
    cl->SetComputeRoot32BitConstants(7, 20, c, 0);
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

// 描述符槽位：0..3 = 各槽位 color SRV，4 = 残差 SRV，5 = g_netS UAV，6 = g_pre UAV
// =====================================================================================
// 带宽/拷贝引擎测试：每帧在【copy 队列】上搬 33MB（= 新形态要传的 4K RGBA8 原生色）
//   · 目的：验证"拷贝类工作能否被计算完全掩盖"（探针里 ALU ∥ 拷贝 = 0.88，这里用真负载）
//   · 开关：xess_proxy.bwtest.txt 写 1/0（每 300 帧读一次）
//   · 上一帧的拷贝还没完成就跳过本帧 —— 绝不阻塞
// =====================================================================================
static bool g_bwOn = false;
static ComPtr<ID3D12CommandQueue> g_bwQ;
static ComPtr<ID3D12CommandAllocator> g_bwAlloc;
static ComPtr<ID3D12GraphicsCommandList> g_bwList;
static ComPtr<ID3D12Fence> g_bwFence;
static UINT64 g_bwFv = 0;
static ComPtr<ID3D12Resource> g_bwA, g_bwB;
static std::string g_bwPath;

static void BwPollFile() {
    if (g_bwPath.empty()) g_bwPath = ModuleDir() + "xess_proxy.bwtest.txt";
    static int s_last = -1;
    FILE* f = nullptr;
    if (fopen_s(&f, g_bwPath.c_str(), "r") != 0 || !f) { return; }
    char b[8] = { 0 };
    size_t n = fread(b, 1, 7, f);
    fclose(f);
    if (!n) return;
    int v = (b[0] == '1') ? 1 : 0;
    if (v == s_last) return;
    s_last = v;
    g_bwOn = (v != 0);
    Log("带宽测试: %s（帧 %d）", g_bwOn ? "开（每帧 copy 队列搬 33MB）" : "关", g_frame);
}

static bool BwInit(ID3D12Device* dev) {
    g_bwPath = ModuleDir() + "xess_proxy.bwtest.txt";
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_bwQ)))) { Log("带宽测试: 建 copy 队列失败"); return false; }
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_bwAlloc)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_bwAlloc.Get(), nullptr, IID_PPV_ARGS(&g_bwList)))) return false;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_bwFence)))) return false;
    g_bwList->Close();

    const UINT64 bytes = 3840ull * 2160ull * 4ull;   // 33.18 MB
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (int i = 0; i < 2; ++i) {
        ComPtr<ID3D12Resource>* t = i ? &g_bwB : &g_bwA;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                __uuidof(ID3D12Resource), (void**)t->GetAddressOf()))) {
            Log("带宽测试: 建 buffer 失败");
            return false;
        }
    }
    Log("带宽测试: 就绪（copy 队列 + 2×33.2MB）");
    return true;
}

static void BwTick() {
    if (!g_bwOn || !g_bwQ || !g_bwList || !g_bwFence) return;
    if (g_bwFence->GetCompletedValue() + 1 < g_bwFv) return;   // 上一帧还没搬完 → 本帧跳过
    if (FAILED(g_bwAlloc->Reset())) return;
    if (FAILED(g_bwList->Reset(g_bwAlloc.Get(), nullptr))) return;
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.pResource = g_bwA.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_bwList->ResourceBarrier(1, &br);
    br.Transition.pResource = g_bwB.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_bwList->ResourceBarrier(1, &br);
    g_bwList->CopyResource(g_bwB.Get(), g_bwA.Get());
    br.Transition.pResource = g_bwA.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_bwList->ResourceBarrier(1, &br);
    br.Transition.pResource = g_bwB.Get();
    br.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    br.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_bwList->ResourceBarrier(1, &br);
    if (FAILED(g_bwList->Close())) return;
    ID3D12CommandList* ls[] = { g_bwList.Get() };
    g_bwQ->ExecuteCommandLists(1, ls);
    g_bwQ->Signal(g_bwFence.Get(), ++g_bwFv);
    static int cnt = 0;
    if ((cnt++ % 300) == 0) Log("带宽测试: 已提交 %d 次 33MB 拷贝（帧 %d）", cnt, g_frame);
}

// ---- MV 打包：1440p R16G16B16A16_SNORM(8B) → 1080p R16G16_FLOAT(4B) ----
// 传输量从 29.5MB 降到 8.3MB；只用 .rg，信息无损
static const char* kMvPackHLSL = R"(
Texture2D<float4>   gS : register(t0);
SamplerState        gM : register(s0);
RWTexture2D<float2> gD : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gD.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    gD[id.xy] = gS.SampleLevel(gM, uv, 0).rg;
}
)";
static ComPtr<ID3D12Resource> g_mvPacked;
static bool g_mvPackReady = false;

// =====================================================================================
// B 形态 步骤1：把「上采样所需的输入」跨卡传给 9070 XT
// -------------------------------------------------------------------------------------
// 传什么：合成后的 pColor（= 槽位原图 + 残差）+ 该槽位的 MV + 深度 + jitter
// 为什么传这些：B 要在自己的卡上跑上采样（真 XeSS / FSR4），它必须拿到这些输入。
// 放哪：一个跨适配器共享 buffer，2 槽轮转（每槽 512B 头 + 三张线性图）
// 开关：xess_proxy.bform.txt 写 1/0（每 300 帧读一次）；默认 0，不影响现有 A 形态
// 注意：RowPitch 必须 256 对齐（D3D12_TEXTURE_DATA_PITCH_ALIGNMENT）
// =====================================================================================
// 前置声明（MakeXBuf 定义在后面）
static bool MakeXBuf(UINT64 bytes, const char* tag,
                     ComPtr<ID3D12Heap>& hA, ComPtr<ID3D12Resource>& bA,
                     ComPtr<ID3D12Heap>& hB, ComPtr<ID3D12Resource>& bB);
static ComPtr<ID3D12Heap>     g_upHeap, g_upHeapW;
static ComPtr<ID3D12Resource> g_upBuf;
static UINT64 g_upSlotBytes = 0;      // 单槽字节数
static UINT64 g_upOffC = 0, g_upOffM = 0, g_upOffD = 0;   // 槽内三段偏移
static UINT   g_upPitchC = 0, g_upPitchM = 0, g_upPitchD = 0;
static UINT   g_upW = 0, g_upH = 0;
static UINT   g_upMw = 0, g_upMh = 0;
static UINT   g_upDw = 0, g_upDh = 0;
static DXGI_FORMAT g_upFmtC = DXGI_FORMAT_UNKNOWN;
static DXGI_FORMAT g_upFmtM = DXGI_FORMAT_UNKNOWN;
static DXGI_FORMAT g_upFmtD = DXGI_FORMAT_UNKNOWN;
static UINT   g_upFrameCnt = 0;
static std::string g_bformPath;

static UINT UpPitch(UINT w, UINT bpp) {
    UINT p = w * bpp;
    return (p + 255u) & ~255u;
}
// 返回每像素字节数；返回 0 表示【该格式不能用 PlacedFootprint 拷进 buffer】
//   典型不可拷：typeless（如 MV 的 R32G8X24_TYPELESS、深度的 typeless 变体）、
//               压缩格式、以及所有 depth/stencil。宁可跳过，也不要发非法调用（已因此崩过一次）。
static UINT FmtBytesCopyable(DXGI_FORMAT f) {
    switch (f) {
    // 4 字节
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:  case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
        return 4;
    // 8 字节
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
        return 8;
    // 2 字节
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
        return 2;
    // 1 字节
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT:
        return 1;
    // 其余（含所有 typeless / depth / stencil / BC 压缩）→ 不可拷
    default:
        return 0;
    }
}
static UINT FmtBytes(DXGI_FORMAT f) {
    UINT b = FmtBytesCopyable(f);
    return b ? b : 4u;    // 仅供不可拷格式算个"占位"尺寸，实际会被跳过
}

static void BformPollFile() {
    if (g_bformPath.empty()) g_bformPath = ModuleDir() + "xess_proxy.bform.txt";
    static int s_last = -1;
    FILE* f = nullptr;
    if (fopen_s(&f, g_bformPath.c_str(), "r") != 0 || !f) return;
    char b[8] = { 0 };
    size_t n = fread(b, 1, 7, f);
    fclose(f);
    if (!n) return;
    int v = (b[0] >= '0' && b[0] <= '9') ? (b[0] - '0') : 0;   // ★ 加档位必须同步改范围（曾写死 0~8）
    if (v == s_last) return;
    s_last = v;
    const bool taa = (v == 8);
    const bool b4k = (v == 9);
    g_bform4k = b4k;                     // 9 = 4K 后置：取 pOutput(XeSS 之后, 4K) 作底图，B 直通出画
    if (v == 6)
        Log("!! bform=6 已弃用：它是 Catmull-Rom 放大（画质明显差于 9）。保留仅作应急对照，推荐写 9 或 0");
    g_taaOn = taa;
    g_bformOn = (v != 0);
    g_bformStage = (taa || b4k) ? 6 : v;  // 8/9 = 阶段 6 的变体（复用同一条链路）
    g_taaValid = false;                  // 任何切换都让时域历史作废，避免把上一形态的画面拖进来
    Log("BFORM: 开关 -> %s 阶段=%d%s（帧 %d）%s", g_bformOn ? "开" : "关", g_bformStage,
        taa ? " +TAA-lite" : "", g_frame,
        g_bformOn && !g_bformReady ? "（但资源未就绪，本帧跳过）" : "");
}

// 【探针 5】运行时对照实验：改 xess_proxy.diag5.txt 即刻生效（不用改代码、不用重启）
//   0=正常  1=成品整屏洋红（验证"我们看到的到底是不是成品"）
//   2=强制把残差抬到 +0.2（验证残差能不能上屏）  3=成品直接放 NR 答案（看网络输出本身）
static volatile int g_diag5Mode = 0;
static std::string g_diag5Path;
static void Diag5PollFile() {
    if (g_diag5Path.empty()) g_diag5Path = ModuleDir() + "xess_proxy.diag5.txt";
    static int s_last = -1;
    FILE* f = nullptr;
    if (fopen_s(&f, g_diag5Path.c_str(), "r") != 0 || !f) return;
    char b[8] = { 0 };
    size_t n = fread(b, 1, 7, f);
    fclose(f);
    if (!n) return;
    int v = (b[0] >= '0' && b[0] <= '4') ? (b[0] - '0') : 0;   // ★ 范围必须覆盖所有档位（曾写死 0~3，把 4 静默吃掉）
    if (v == s_last) return;
    s_last = v;
    g_diag5Mode = v;
    Log("DIAG5: 探针模式 = %d（%s）", v,
        v == 0 ? "正常" : v == 1 ? "整屏洋红 → 验证覆盖层可见性" :
        v == 2 ? "强制残差 +0.2 → 验证残差能上屏" :
        v == 3 ? "成品直接放 NR 答案 → 看网络输出本身" :
                 "残差低频扣除 → 只留细节修正，整帧不变亮变暗");
}

// 【TAA 参数热调】xess_proxy.taa.txt 写两个数："alpha clampK"（例如 0.25 3.0），每 60 帧读一次。
//   为什么要热调：TAA 的观感/拖尾全靠这两个数，改一次重启一次太浪费。
static void TaaPollFile() {
    static std::string path;
    if (path.empty()) path = ModuleDir() + "xess_proxy.taa.txt";
    static float sA = -1.0f, sC = -1.0f;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return;
    float a = 0, c = 0;
    int n = fscanf_s(f, "%f %f", &a, &c);
    fclose(f);
    if (n < 2) return;
    if (a < 0.01f) a = 0.01f; if (a > 1.0f) a = 1.0f;
    if (c < 0.0f)  c = 0.0f;  if (c > 8.0f) c = 8.0f;
    if (a == sA && c == sC) return;
    sA = a; sC = c;
    g_taaAlpha = a; g_taaClampK = c;
    Log("TAA: 参数更新 alpha=%.2f clampK=%.2f", a, c);
}

static bool BformInit(ID3D12Device* dev, ID3D12Resource* color, ID3D12Resource* mv, ID3D12Resource* dep) {
    if (g_mvPackReady && g_mvPacked) mv = g_mvPacked.Get();   // 传的是打包后的 1080p MV
    if (!color || !mv || !dep) { Log("BFORM: 缺输入纹理，跳过初始化"); return false; }
    D3D12_RESOURCE_DESC cd = color->GetDesc();
    D3D12_RESOURCE_DESC md = mv->GetDesc();
    D3D12_RESOURCE_DESC dd = dep->GetDesc();

    g_upW = (UINT)cd.Width; g_upH = cd.Height; g_upFmtC = cd.Format;
    // ★ bform=9（4K 后置）：槽位与下游全部按【pOutput】的尺寸/格式 —— 底图是 XeSS 上采样之后的 4K
    if (g_bform4k && g_outpRes) {
        D3D12_RESOURCE_DESC od = g_outpRes->GetDesc();
        g_upW = (UINT)od.Width; g_upH = od.Height;
        g_upFmtC = DXGI_FORMAT_B8G8R8A8_UNORM;   // ★ 传 8bit 转换结果（与 A 侧底图同为 BGRA）；10bit 直传实验已回退
        Log("BFORM: 4K 后置模式 → 底图 %ux%u（8bit，由 pOutput 转换而来）", g_upW, g_upH);
    }
    g_upMw = (UINT)md.Width; g_upMh = md.Height; g_upFmtM = md.Format;
    g_upDw = (UINT)dd.Width; g_upDh = dd.Height; g_upFmtD = dd.Format;

    // 逐路检查"能不能用 PlacedFootprint 拷进 buffer"；不能就整路跳过（不发非法调用）
    UINT bppC = FmtBytesCopyable(g_upFmtC);
    UINT bppM = FmtBytesCopyable(g_upFmtM);
    UINT bppD = FmtBytesCopyable(g_upFmtD);
    if (!bppC) { Log("BFORM: 颜色格式 %d 不可拷，B 形态不可用", (int)g_upFmtC); return false; }
    if (!bppM) { Log("BFORM: MV 格式 %d 不可拷 → 跳过 MV 这一路", (int)g_upFmtM); g_upMw = g_upMh = 0; }
    if (!bppD) { Log("BFORM: 深度格式 %d 不可拷（typeless/深度）→ 跳过深度这一路", (int)g_upFmtD); g_upDw = g_upDh = 0; }

    g_upPitchC = UpPitch(g_upW, bppC);
    g_upPitchM = g_upMw ? UpPitch(g_upMw, bppM) : 0;
    g_upPitchD = g_upDw ? UpPitch(g_upDw, bppD) : 0;

    const UINT64 HDR = 512;
    g_upOffC = HDR;
    g_upOffM = g_upOffC + (UINT64)g_upPitchC * g_upH;
    g_upOffD = g_upOffM + (UINT64)g_upPitchM * g_upMh;
    g_upSlotBytes = g_upOffD + (UINT64)g_upPitchD * g_upDh;
    // 每槽起点按 64K 对齐（共享堆/放置要求）
    g_upSlotBytes = (g_upSlotBytes + 65535ull) & ~65535ull;

    UINT64 total = g_upSlotBytes * (UINT64)K;
    if (!MakeXBuf(total, "B形态上采样输入(K槽)", g_upHeap, g_upBuf, g_upHeapW, g_upBufW)) {
        Log("BFORM: 建跨卡 buffer 失败");
        return false;
    }
    g_upSlot = 0;
    g_upFrameCnt = 0;
    g_bformReady = true;
    Log("BFORM: 就绪 | 色 %ux%u fmt=%d pitch=%u | MV %ux%u fmt=%d pitch=%u | 深度 %ux%u fmt=%d pitch=%u | 每槽 %.1f MB × %d",
        g_upW, g_upH, (int)g_upFmtC, g_upPitchC,
        g_upMw, g_upMh, (int)g_upFmtM, g_upPitchM,
        g_upDw, g_upDh, (int)g_upFmtD, g_upPitchD,
        (double)g_upSlotBytes / (1024.0 * 1024.0), (int)K);
    return true;
}

// ===== 抓帧导出（2026-10-28）：把链路各环节的画面存成 .raw（BGRA8, 4K），供离线转 PNG =====
// 开关：xess_proxy.dumpshot.txt 内容 "<stage> <frames>"
//   stage = A（我方转换后的 4K，即准备传过去的） / N（B 解码到的底图） / C（B 合成结果）
// 文件落盘：<游戏目录>\dump\<stage>_f<帧号>.raw（4800x2700 之类请用同目录 .txt 里的宽高）
// (重复声明已移除) static std::string g_dumpDir;
// (重复声明已移除) static int   g_dumpStage = 0, g_dumpLeft = 0;
// (重复) static ComPtr<ID3D12Resource> g_dumpRbA, g_dumpRbB;
// (重复声明已移除) static bool  g_dumpPendA = false, g_dumpPendB = false;
// (重复) static ComPtr<ID3D12Resource> g_dumpRbA_, g_dumpRbB_;
// (重复) static int   g_dumpPendFrameA = 0, g_dumpPendFrameB = 0;


static const char* kDumpName[3] = { "A", "N", "C" };

static void DumpPollFile() {
    static int lastMask = -2, lastN = -1;
    if (g_dumpDir.empty()) { g_dumpDir = ModuleDir() + "dump"; CreateDirectoryA(g_dumpDir.c_str(), nullptr); }
    std::string p = ModuleDir() + "xess_proxy.dumpshot.txt";
    FILE* f = nullptr; if (fopen_s(&f, p.c_str(), "r") != 0 || !f) return;
    char st[16] = {0}; int n = 0;
    int got = fscanf_s(f, "%15s %d", st, (unsigned)sizeof(st), &n);
    fclose(f);
    if (got < 2) return;
    if (n <= 0) { g_dumpMask = 0; g_dumpLeft = 0; return; }   // ★ 写 0 就是关闭
    int mask = 0;
    for (int i = 0; st[i]; ++i) {
        char ch = (char)toupper((unsigned char)st[i]);
        if (ch == (char)65) mask |= 1;   // A
        else if (ch == (char)78) mask |= 2;  // N
        else if (ch == (char)67) mask |= 4;  // C
    }
    if (!mask) return;
    if (mask != lastMask || n != lastN) { lastMask = mask; lastN = n; g_dumpMask = mask; g_dumpLeft = n; Log("DUMP: 开始抓帧 mask=0x%X（A/N/C）共 %d 帧", mask, n); }
}

static void DumpRecord(ID3D12GraphicsCommandList* cl, ID3D12Device* dev, int st,
                       ID3D12Resource* src, D3D12_RESOURCE_STATES cur) {
    if (!cl || !dev || !src || g_dumpLeft <= 0) return;   // ★ <=0 必须停（之前写成 !g_dumpLeft 导致负数时继续抓）
    if (!(g_dumpMask & (1 << st))) return;
    D3D12_RESOURCE_DESC sd = src->GetDesc();
    UINT w = (UINT)sd.Width, h = sd.Height, pitch = (w * 4u + 255u) & ~255u;
    if (!g_dumpRb[st]) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (UINT64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&g_dumpRb[st])))) { Log("DUMP: 建回读缓冲失败(st=%d)", st); return; }
    }
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = src;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = g_dumpRb[st].Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = sd.Format;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter  = cur;
    cl->ResourceBarrier(1, &b);
    g_dumpW = w; g_dumpH = h; g_dumpPitch = pitch;
    g_dumpPend[st] = true; g_dumpPendFrame[st] = g_frame;
}

static void DumpFlush(int st) {
    if (!g_dumpPend[st] || !g_dumpRb[st] || g_dumpLeft <= 0) return;
    g_dumpPend[st] = false;
    void* m = nullptr; D3D12_RANGE rr{ 0, (SIZE_T)g_dumpPitch * g_dumpH };
    if (FAILED(g_dumpRb[st]->Map(0, &rr, &m)) || !m) return;
    char name[256];
    sprintf_s(name, "%s\\%s_f%d.raw", g_dumpDir.c_str(), kDumpName[st], g_dumpPendFrame[st]);
    FILE* f = nullptr;
    if (fopen_s(&f, name, "wb") == 0 && f) {
        fwrite(m, 1, (size_t)g_dumpPitch * g_dumpH, f); fclose(f);
        sprintf_s(name, "%s\\%s_f%d.txt", g_dumpDir.c_str(), kDumpName[st], g_dumpPendFrame[st]);
        if (fopen_s(&f, name, "w") == 0 && f) { fprintf(f, "w=%u h=%u pitch=%u fmt=BGRA8\n", g_dumpW, g_dumpH, g_dumpPitch); fclose(f); }
        --g_dumpLeft;
        Log("DUMP: 已存 %s_f%d（%ux%u）剩余 %d", kDumpName[st], g_dumpPendFrame[st], g_dumpW, g_dumpH, g_dumpLeft);
    }
    D3D12_RANGE nw{ 0, 0 }; g_dumpRb[st]->Unmap(0, &nw);
}
// 记在游戏的命令列表上：三张图 → 当前槽
static void BformCopy(ID3D12GraphicsCommandList* cl, ID3D12Resource* color,
                      ID3D12Resource* mv, ID3D12Resource* dep, float jx, float jy) {
    if (!g_bformOn || !g_bformReady || !cl || !color || !mv || !dep) return;
    // 槽位按【帧号】索引（K 槽）：B 处理第 N 帧时去读 N%K，不会被后来的帧覆盖
    const UINT64 base = (UINT64)(g_frame % K) * g_upSlotBytes;

    Barrier(cl, g_upBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);

    auto one = [&](ID3D12Resource* src, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch, UINT64 off,
                   D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES back) {
        Barrier(cl, src, from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = g_upBuf.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = base + off;
        dst.PlacedFootprint.Footprint.Format = fmt;
        dst.PlacedFootprint.Footprint.Width = w;
        dst.PlacedFootprint.Footprint.Height = h;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = pitch;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = src;
        s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
        Barrier(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, back);
    };
    // 状态与 ③ 步捕获槽位时完全一致（照抄，避免又多一种状态假设）
    if (!g_xferOwn)   // 颜色由我们自己的 COPY 队列拷（见 XferKickColor），这里只拷 MV/深度
        one(color, g_upFmtC, g_upW, g_upH, g_upPitchC, g_upOffC,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON);
    if (g_upMw && g_upMh)
        one(mv, g_upFmtM, g_upMw, g_upMh, g_upPitchM, g_upOffM,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (g_upDw && g_upDh)
        one(dep, g_upFmtD, g_upDw, g_upDh, g_upPitchD, g_upOffD,
            D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_READ);

    Barrier(cl, g_upBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);

    if (!g_xferOwn) {   // ★ 4K 模式下 tag/lastSlot 由 XferKickColor 在【颜色拷贝成功后】发布
        g_upLastSlot = g_frame % K;
        const int sl = g_frame % K;
        g_slotTag[sl].jx = jx;
        g_slotTag[sl].jy = jy;
        g_slotTag[sl].frame = g_frame;
    }
    if ((g_upFrameCnt++ % 300) == 0)
        Log("BFORM: 已拷贝第 %u 次（帧 %d 槽 %d，每槽 %.1f MB）jitter=%.3f,%.3f",
            g_upFrameCnt, g_frame, g_frame % K, (double)g_upSlotBytes / (1024.0 * 1024.0), jx, jy);
}

// =====================================================================================
// B 形态 步骤2：B 侧把共享 buffer 解成纹理（供之后的降采样 / NR / 上采样用）
//   全部懒创建 + 全在 bform 开关下；bform=0 时一行都不执行
// =====================================================================================
static ComPtr<ID3D12Resource> g_bTexM, g_bTexD;
static bool g_bTexReady = false;
static UINT g_bDecCnt = 0;

static bool BformBEnsure(ID3D12Device* dev) {
    if (g_bTexReady) return true;
    if (!dev || !g_bformReady || !g_upW || !g_upH) return false;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto mk = [&](UINT w, UINT h, DXGI_FORMAT f, ComPtr<ID3D12Resource>& r, const char* nm) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = f; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        if (FAILED(hr)) { Log("BFORM-B: 建 %s 纹理失败 0x%08X", nm, (unsigned)hr); return false; }
        return true;
    };
    if (!mk(g_upW, g_upH, g_upFmtC, g_bTexC, "色")) return false;
    if (!mk(g_upMw, g_upMh, g_upFmtM, g_bTexM, "MV")) return false;
    if (g_upDw && g_upDh) { if (!mk(g_upDw, g_upDh, g_upFmtD, g_bTexD, "深度")) return false; }
    g_bTexReady = true;
    Log("BFORM-B: B 侧纹理就绪 | 色 %ux%u fmt=%d | MV %ux%u fmt=%d | 深度 %ux%u fmt=%d",
        g_upW, g_upH, (int)g_upFmtC, g_upMw, g_upMh, (int)g_upFmtM, g_upDw, g_upDh, (int)g_upFmtD);
    return true;
}

// 记在 B 的命令列表上：共享 buffer 当前槽 → 三张本地纹理
static void BformBDecode(ID3D12GraphicsCommandList* cl, int slot) {
    if (!g_bformOn || !g_bformReady || !cl || !g_upBufW) return;
    if (!BformBEnsure(g_worker.dev.Get())) return;
    const UINT64 base = (UINT64)slot * g_upSlotBytes;

    auto one = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch, UINT64 off) {
        Barrier(cl, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = tex;
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = g_upBufW.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Offset = base + off;
        s.PlacedFootprint.Footprint.Format = fmt;
        s.PlacedFootprint.Footprint.Width = w;
        s.PlacedFootprint.Footprint.Height = h;
        s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = pitch;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        Barrier(cl, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    };
    // ★ B 侧读共享 buffer 前必须把它转成 COPY_SOURCE —— 跨适配器资源的"状态"是各设备各记一份的，
    //    A 那边转成 COMMON 不代表 B 这边也在 COMMON。少了这句会读到非法状态（表现是极慢/异常）。
    Barrier(cl, g_upBufW.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    one(g_bTexC.Get(), g_upFmtC, g_upW, g_upH, g_upPitchC, g_upOffC);
    if (g_upMw && g_upMh)
        one(g_bTexM.Get(), g_upFmtM, g_upMw, g_upMh, g_upPitchM, g_upOffM);
    if (g_upDw && g_upDh && g_bTexD)
        one(g_bTexD.Get(), g_upFmtD, g_upDw, g_upDh, g_upPitchD, g_upOffD);
    Barrier(cl, g_upBufW.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    if ((g_bDecCnt++ % 300) == 0)
        Log("BFORM-B: 已解出第 %u 次（槽 %d，%ux%u）", g_bDecCnt, slot, g_upW, g_upH);
}

// ---- B 形态 步骤3：B 侧自己把收到的 pColor 降采样到 1080p，直接喂给 NR ----
//   这样 A 就不用再传 1080p（省 8.3MB/帧 + 一条通道）
static const char* kBformDS = R"(
Texture2D<float4>   gS : register(t0);
SamplerState        gM : register(s0);
RWTexture2D<float4> gD : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gD.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    // ★ 与 A 侧 DownCS 对齐：2×2 区域平均（源比目标大约 1.16 倍，单点采样会混叠）
    //   原来是单点双线性，导致 B 喂给网络的小图和 A 不是同一张 →
    //   同一场景 A 的 |答案−喂入| ≈7~9、B 只有 2~6（差 3~4 倍）
    float2 t = float2(0.25 / 1920.0, 0.25 / 1080.0) * 1.16;
    float4 c  = gS.SampleLevel(gM, uv + float2(-t.x, -t.y), 0);
    c += gS.SampleLevel(gM, uv + float2( t.x, -t.y), 0);
    c += gS.SampleLevel(gM, uv + float2(-t.x,  t.y), 0);
    c += gS.SampleLevel(gM, uv + float2( t.x,  t.y), 0);
    gD[id.xy] = float4(c.rgb * 0.25, 1.0);
}
)";
static ComPtr<ID3D12RootSignature> g_bDsRs;
static ComPtr<ID3D12PipelineState> g_bDsPso;
static ComPtr<ID3D12Resource> g_bDs1080;
static bool g_bDsReady = false;

// ★ 共用的小工具堆/根签名：DS 与 UP 都用"描述符表"绑纹理
//   （根描述符 SRV/UAV 只能描述 buffer，绑 Texture2D 会让 PSO 创建失败 —— 已踩）
static ComPtr<ID3D12DescriptorHeap> g_bAuxHeap;   // 0=DS源 1=DS目标 2=UP源 3=UP目标
static ComPtr<ID3D12RootSignature>  g_bAuxRs;
static UINT g_bAuxInc = 0;
static bool g_bAuxReady = false;

static bool BformBAuxEnsure(ID3D12Device* dev) {
    if (g_bAuxReady) return true;
    if (!dev) return false;
    D3D12_DESCRIPTOR_RANGE rgS{};
    rgS.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rgS.NumDescriptors = 1;
    rgS.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE rgU{};
    rgU.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rgU.NumDescriptors = 1;
    rgU.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &rgS;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &rgU;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2; rd.pParameters = rp;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Log("BFORM-B: AUX 根签名失败: %s", err ? (char*)err->GetBufferPointer() : "?"); return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_bAuxRs)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_bAuxHeap)))) return false;
    g_bAuxInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_bAuxReady = true;
    return true;
}

static bool BformBEnsureDS(ID3D12Device* dev) {
    if (g_bDsReady) return true;
    if (!dev) return false;
    if (!BformBAuxEnsure(dev)) return false;
    g_bDsRs = g_bAuxRs;
    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kBformDS, strlen(kBformDS), "bds.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("BFORM-B: DS 编译失败: %s", e2 ? (char*)e2->GetBufferPointer() : "?"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_bDsRs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    {   HRESULT hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_bDsPso));
        if (FAILED(hr)) { Log("BFORM-B: DS PSO 失败 0x%08X", (unsigned)hr); return false; }
    }

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kNetW; td.Height = kNetH;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_bDs1080)))) {
        Log("BFORM-B: DS 纹理失败"); return false;
    }
    g_bDsReady = true;
    Log("BFORM-B: 本地降采样就绪（%dx%d）", kNetW, kNetH);
    return true;
}

// B 侧：收到的 pColor → 1080p → 写进 NR 的输入纹理
static bool BformBDownsample(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst1080) {
    if (!g_bDsReady || !g_bTexC || !dst1080) return false;
    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetComputeRootSignature(g_bDsRs.Get());
    cl->SetPipelineState(g_bDsPso.Get());
    {   // 走描述符表（0=源 SRV，1=目标 UAV）
        auto mkS = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * g_bAuxInc;
            g_worker.dev->CreateShaderResourceView(r, &sd, h);
        };
        auto mkU = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * g_bAuxInc;
            g_worker.dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
        };
        mkS(g_bTexC.Get(), g_upFmtC, 0);
        mkU(g_bDs1080.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        ID3D12DescriptorHeap* hs[] = { g_bAuxHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        D3D12_GPU_DESCRIPTOR_HANDLE g = g_bAuxHeap->GetGPUDescriptorHandleForHeapStart();
        cl->SetComputeRootDescriptorTable(0, g);
        D3D12_GPU_DESCRIPTOR_HANDLE g1 = g; g1.ptr += g_bAuxInc;
        cl->SetComputeRootDescriptorTable(1, g1);
    }
    cl->Dispatch((kNetW + 7) / 8, (kNetH + 7) / 8, 1);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    // 注意：dst1080（NR 输入纹理）照抄现有做法，不额外加状态转换（跨 API 导入，沿用原约定）
    cl->CopyResource(dst1080, g_bDs1080.Get());
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    static int c = 0;
    if ((c++ % 300) == 0) Log("BFORM-B: 已本地降采样 %d 次", c);
    return true;
}

// ---- B 形态 步骤4：B 侧残差合成（把 A 的 ResidCS 搬到 B，答案改从纹理取）----
//   输入：pColor(收到) + MV(收到) + 喂进 NR 的 1080p + NR 答案(纹理)
//   输出：合成后的 pColor（供 S5 上采样用）；时域状态在 B 本地
static const char* kBformResid = R"(
Texture2D<float4>   gSrc : register(t0);
Texture2D<float4>   gRes : register(t1);
Texture2D<float4>   gMV  : register(t2);
Texture2D<float4>   gFed : register(t3);
Texture2D<float4>   gAns : register(t4);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);
RWTexture2D<float4> gSt  : register(u1);
cbuffer P : register(b0)
{
    // ★ 必须与 A 侧 SetPass 上传的 20 个常量【逐位对齐】（A 的 kHLSL 顺序）。
    //   曾经这里只声明 8 个、且删掉了 edge/smooth/temporal，导致后面的
    //   gain/sat/alpha/varGain/mvScale/mvConf/gateK 全部塌陷错位：
    //   MV 缩放被喂成 alpha、方差门限被喂成 sat ⇒ 残差被重投影到错误位置后又抵消掉。
    float gLimit; float gEdge; float gSmooth; float gGain;
    float gSat; float gAlpha; float gVarGain; float gTemporal;
    float gMvScale; float gMvConf; float gMvIdx; float gConfIdx;
    float gGateK; float gGateIdx; float gMode; float gForce;
    float gLowCut; float gLowR; float gPad2; float gPad3;
};
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 c = gSrc.Load(int3((int)id.x, (int)id.y, 0));
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);

    float3 a = gAns.SampleLevel(gSmp, uv, 0).rgb;
    float3 f = gFed.SampleLevel(gSmp, uv, 0).rgb;
    float3 e = a - f;

    // 【低频扣除】把残差的"局部均值"减掉，只保留高频细节修正。
    //   为什么：网络输出偶尔整体偏亮/偏暗（同帧实测差过 20+ 灰阶），
    //   全额叠上去会让整帧变亮变暗 → 表现为全图亮暗闪。
    //   做法：8 个方向 + 自己共 9 点，在大半径 gLowR 上估均值（很便宜）。
    if (gLowCut > 0.5) {
        float2 tx = float2(1.0 / 1920.0, 1.0 / 1080.0) * gLowR;
        float3 se = e;
        [unroll] for (int kk = 0; kk < 8; ++kk) {
            float ang = (float)kk * 0.7853982;
            float2 off = float2(cos(ang), sin(ang)) * tx;
            float3 a2 = gAns.SampleLevel(gSmp, saturate(uv + off), 0).rgb;
            float3 f2 = gFed.SampleLevel(gSmp, saturate(uv + off), 0).rgb;
            se += (a2 - f2);
        }
        e -= se * (1.0 / 9.0);
    }

    float elum = dot(e, float3(0.2126, 0.7152, 0.0722));
    e = lerp(float3(elum, elum, elum), e, gSat);
    e *= gGain;
    e = clamp(e, -gLimit, gLimit);

    {
        float3 m1 = gSrc.Load(int3(min((int)id.x + 2, (int)w - 1), (int)id.y, 0)).rgb;
        float3 m2 = gSrc.Load(int3(max((int)id.x - 2, 0), (int)id.y, 0)).rgb;
        float3 m3 = gSrc.Load(int3((int)id.x, min((int)id.y + 2, (int)h - 1), 0)).rgb;
        float3 m4 = gSrc.Load(int3((int)id.x, max((int)id.y - 2, 0), 0)).rgb;
        float3 mean4 = (m1 + m2 + m3 + m4) * 0.25;
        float  var   = dot(abs(c.rgb - mean4), float3(0.3333, 0.3333, 0.3333));
        e *= 1.0 / (1.0 + gVarGain * var * 8.0);
    }
    {
        uint sw, sh; gSt.GetDimensions(sw, sh);
        int2 sxy = int2(saturate(uv) * float2(sw - 1, sh - 1) + 0.5);
        float2 mv = gMV.SampleLevel(gSmp, uv, 0).rg * gMvScale;
        float2 prevUV = saturate(uv - mv);
        int2 pxy = clamp(int2(prevUV * float2(sw - 1, sh - 1) + 0.5), int2(0, 0), int2(sw - 1, sh - 1));
        float3 prev = gSt.Load(int3(pxy, 0)).rgb - 0.5;
        float3 pc = gFed.SampleLevel(gSmp, prevUV, 0).rgb;
        float cd = dot(abs(c.rgb - pc), float3(0.3333, 0.3333, 0.3333));
        float conf = saturate(1.0 - cd * gMvConf * 8.0);
        float d = dot(abs(prev - e), float3(0.3333, 0.3333, 0.3333));
        float gate = saturate(1.0 - d * gGateK);
        float al = lerp(1.0, gAlpha, conf * gate);
        e = lerp(prev, e, al);
        gSt[sxy] = float4(e + 0.5, 1.0);
    }
        // ---- 探针 5（运行时切换；只作用于 B 侧合成）----
    if (gMode > 0.5 && gMode < 1.5) { gDst[id.xy] = float4(1.0, 0.0, 1.0, 1.0); return; }  // 1=纯洋红
    if (gMode > 2.5 && gMode < 3.5) { gDst[id.xy] = float4(a, 1.0); return; }               // 3=直接放答案
    if (gMode > 1.5) { gDst[id.xy] = float4(saturate(c.rgb + e + gForce), c.a); return; }   // 2=强制叠加
gDst[id.xy] = float4(saturate(c.rgb + e), c.a);
}
)";
static ComPtr<ID3D12RootSignature> g_bRs;
static ComPtr<ID3D12PipelineState> g_bPso;
static ComPtr<ID3D12DescriptorHeap> g_bHeap;
static UINT g_bInc = 0;
static ComPtr<ID3D12Resource> g_bComp, g_bState;
static bool g_bStateCleared = false;
static UINT g_bCompCnt = 0;

// 槽位（B 自己的小堆）：0=pColor 1=占位 2=MV 3=fed(1080p) 4=答案 5=占位 6=合成UAV 7=状态UAV
static bool BformBEnsureComp(ID3D12Device* dev) {
    if (g_bCompReady) return true;
    if (!dev || !g_bformReady || !g_upW) return false;

    // ★ 每张表必须用【不同的寄存器】t0..t4 / u0 / u1；
    //   全部用 t0 会让根签名序列化直接失败（0x80070057，已踩）
    D3D12_DESCRIPTOR_RANGE rgS[5]{};
    D3D12_DESCRIPTOR_RANGE rgU[2]{};
    for (int i = 0; i < 5; ++i) {
        rgS[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rgS[i].NumDescriptors = 1;
        rgS[i].BaseShaderRegister = (UINT)i;
    }
    for (int i = 0; i < 2; ++i) {
        rgU[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rgU[i].NumDescriptors = 1;
        rgU[i].BaseShaderRegister = (UINT)i;
    }
    D3D12_ROOT_PARAMETER rp[8]{};
    for (int i = 0; i < 5; ++i) {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i].DescriptorTable.NumDescriptorRanges = 1;
        rp[i].DescriptorTable.pDescriptorRanges = &rgS[i];
        rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[5].DescriptorTable.NumDescriptorRanges = 1;
    rp[5].DescriptorTable.pDescriptorRanges = &rgU[0];
    rp[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[6].DescriptorTable.NumDescriptorRanges = 1;
    rp[6].DescriptorTable.pDescriptorRanges = &rgU[1];
    rp[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[7].Constants.ShaderRegister = 0;
    rp[7].Constants.Num32BitValues = 20;
    rp[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 8; rd.pParameters = rp;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Log("BFORM-B: 合成根签名失败"); return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_bRs)))) return false;

    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kBformResid, strlen(kBformResid), "bres.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("BFORM-B: 合成 shader 编译失败: %s", e2 ? (char*)e2->GetBufferPointer() : "?"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_bRs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_bPso)))) { Log("BFORM-B: 合成 PSO 失败"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_bHeap)))) { Log("BFORM-B: 合成堆失败"); return false; }
    g_bInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto mkuav = [&](UINT w, UINT h, DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& r, const char* nm) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = fmt; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)))) {
            Log("BFORM-B: 合成 %s 纹理失败", nm); return false;
        }
        return true;
    };
    // 合成输出固定 8bit：底图可能是 10bit（pOutput 原生），但合成/出画这条链保持 8bit
    //   （交换链也因此保持 8bit，避开"10bit swapchain + DComp + 8bit 桌面"的闪烁问题）
    if (!mkuav(g_upW, g_upH, DXGI_FORMAT_B8G8R8A8_UNORM, g_bComp, "输出")) return false;
    if (!mkuav(kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, g_bState, "状态")) return false;

    D3D12_CPU_DESCRIPTOR_HANDLE c = g_bHeap->GetCPUDescriptorHandleForHeapStart();
    auto at = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)i * g_bInc; return h; };
    // 6=合成 UAV, 7=状态 UAV
    { D3D12_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = g_upFmtC; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      dev->CreateUnorderedAccessView(g_bComp.Get(), nullptr, &ud, at(6)); }
    { D3D12_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      dev->CreateUnorderedAccessView(g_bState.Get(), nullptr, &ud, at(7)); }

    g_bCompReady = true;
    Log("BFORM-B: 残差合成就绪 | 输出 %ux%u | 状态 %ux%u", g_upW, g_upH, kNetW, kNetH);
    return true;
}

// 在 B 的列表上：合成 pColor + 残差 → g_bComp
static bool BformBCompose(ID3D12GraphicsCommandList* cl, ID3D12Resource* ansTex) {
    if (!g_bCompReady || !ansTex || !g_bTexC || !g_bTexM || !g_bDs1080) return false;
    {   // 【诊断·一次性】答案纹理描述 vs 我们建 SRV 时写死的格式（这里两个都在作用域里）
        static bool l1 = false;
        if (!l1) {
            l1 = true;
            D3D12_RESOURCE_DESC ad = ansTex->GetDesc();
            Log("DIAG-PO2: 答案 fmt=%d %ux%u array=%u mip=%u | 我们的SRV=R8G8B8A8_UNORM | 合成底色 fmt=%d %ux%u",
                (int)ad.Format, (UINT)ad.Width, (UINT)ad.Height, ad.DepthOrArraySize, ad.MipLevels,
                (int)g_upFmtC, g_upW, g_upH);
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_bHeap->GetCPUDescriptorHandleForHeapStart();
    auto cpu = [&](int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)i * g_bInc; return h; };
    auto mksrv = [&](ID3D12Resource* r, DXGI_FORMAT f, int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g_worker.dev->CreateShaderResourceView(r, &sd, cpu(i));
    };
    mksrv(g_bTexC.Get(), g_upFmtC, 0);
    mksrv(g_bTexC.Get(), g_upFmtC, 1);          // 占位
    mksrv(g_bTexM.Get(), g_upFmtM, 2);
    mksrv(g_bDs1080.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 3);
    mksrv(ansTex, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
    mksrv(g_bTexC.Get(), g_upFmtC, 5);          // 占位

    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, ansTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bComp.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cl, g_bState.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* hs[] = { g_bHeap.Get() };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_bRs.Get());
    cl->SetPipelineState(g_bPso.Get());
    auto gpu = [&](int i) { D3D12_GPU_DESCRIPTOR_HANDLE h = g_bHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * g_bInc; return h; };
    cl->SetComputeRootDescriptorTable(0, gpu(0));
    cl->SetComputeRootDescriptorTable(1, gpu(1));
    cl->SetComputeRootDescriptorTable(2, gpu(2));
    cl->SetComputeRootDescriptorTable(3, gpu(3));
    cl->SetComputeRootDescriptorTable(4, gpu(4));
    cl->SetComputeRootDescriptorTable(5, gpu(6));
    cl->SetComputeRootDescriptorTable(6, gpu(7));
    // 前 8 个与 A 侧 SetPass 的前 8 个含义一致（limit/edge/smooth/gain/sat/alpha/varGain/temporalStatic）
    // 时域状态纹理还没显式清成 0.5 中性 → 头几帧强制 alpha=1（只用当前残差，不读历史）
    float alphaUse = (g_bCompCnt < 8) ? 1.0f : g_alpha;
    float k[20] = { g_limit, g_edge, g_smooth, g_gain, g_sat, alphaUse, g_varGain, g_temporalStatic,
                    g_mvScale, g_mvConf, 0, 0, g_gateK, 0,
                    (float)g_diag5Mode, (g_diag5Mode == 2) ? 0.2f : 0.0f,
                    (g_diag5Mode == 4) ? 1.0f : 0.0f, (g_diag5Mode == 4) ? 12.0f : 0.0f, 0, 0 };
    cl->SetComputeRoot32BitConstants(7, 20, k, 0);
    cl->Dispatch((g_upW + 7) / 8, (g_upH + 7) / 8, 1);

    Barrier(cl, g_bComp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bState.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, ansTex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexC.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    DumpFlush(2);   // 先把上一帧的合成结果落盘
    if (g_dumpMask & 4) DumpRecord(cl, g_worker.dev.Get(), 2, g_bComp.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if ((g_bCompCnt++ % 300) == 0) Log("BFORM-B: 已合成第 %u 次（%ux%u）", g_bCompCnt, g_upW, g_upH);
    return true;
}

// ---- B 形态 步骤5a：B 侧上采样（先用 Catmull-Rom 空间放大，打通链路再说画质）----
//   1440p 合成结果 → 4K。等链路通了再换真 XeSS on B / FSR4（那有"一进程两实例"的不确定性）
static const char* kBformUp = R"(
Texture2D<float4>   gS : register(t0);
SamplerState        gM : register(s0);
RWTexture2D<float4> gD : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gD.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    uint sw, sh; gS.GetDimensions(sw, sh);
    float2 tc = uv * float2(sw, sh) - 0.5;
    float2 f  = frac(tc);
    int2   i  = int2(floor(tc));
    // Catmull-Rom 的 12 抽头（4x4 邻域里的 3x3 有效块）
    float4 acc = float4(0,0,0,0);
    float  wsum = 0.0;
    [unroll] for (int yy = -1; yy <= 2; ++yy) {
        [unroll] for (int xx = -1; xx <= 2; ++xx) {
            int2 q = clamp(i + int2(xx, yy), int2(0,0), int2((int)sw - 1, (int)sh - 1));
            // ★ 抽头与权重必须一一对应：w(-1)↔xx=-1, w(0)↔xx=0, w(1)↔xx=1, w(2)↔xx=2
            //   曾经整体右移一格（xx=0 用 w(-1)），等价于把成品整体偏移 +1 源像素并糊掉
            float wx = (xx == -1) ? (-0.5f*f.x*f.x*f.x + f.x*f.x - 0.5f*f.x)
                     : (xx == 0) ? ( 1.5f*f.x*f.x*f.x - 2.5f*f.x*f.x + 1.0f)
                     : (xx == 1) ? (-1.5f*f.x*f.x*f.x + 2.0f*f.x*f.x + 0.5f*f.x)
                                 : ( 0.5f*f.x*f.x*f.x - 0.5f*f.x*f.x);
            float wy = (yy == -1) ? (-0.5f*f.y*f.y*f.y + f.y*f.y - 0.5f*f.y)
                     : (yy == 0) ? ( 1.5f*f.y*f.y*f.y - 2.5f*f.y*f.y + 1.0f)
                     : (yy == 1) ? (-1.5f*f.y*f.y*f.y + 2.0f*f.y*f.y + 0.5f*f.y)
                                 : ( 0.5f*f.y*f.y*f.y - 0.5f*f.y*f.y);
            float wgt = wx * wy;
            acc += gS.Load(int3(q, 0)) * wgt;
            wsum += wgt;
        }
    }
    gD[id.xy] = acc / max(wsum, 1e-5);
}
)";
static ComPtr<ID3D12RootSignature> g_bUpRs;
static ComPtr<ID3D12PipelineState> g_bUpPso;
static UINT g_bUpCnt = 0;

static bool BformBEnsureUp(ID3D12Device* dev) {
    if (g_bUpReady) return true;
    if (!dev || !g_outW || !g_outH) return false;
    if (!BformBAuxEnsure(dev)) return false;
    g_bUpRs = g_bAuxRs;                 // 与 DS 共用"描述符表 + 静态采样器"的根签名
    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kBformUp, strlen(kBformUp), "bup.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("BFORM-B: UP 编译失败: %s", e2 ? (char*)e2->GetBufferPointer() : "?"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_bUpRs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_bUpPso)))) { Log("BFORM-B: UP PSO 失败"); return false; }

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = g_outW; td.Height = g_outH;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = g_upFmtC; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_bUp4k)))) {
        Log("BFORM-B: UP 纹理失败"); return false;
    }
    g_bUpReady = true;
    Log("BFORM-B: 上采样就绪 | %ux%u -> %ux%u", g_upW, g_upH, g_outW, g_outH);
    return true;
}

// ===== 诊断：五源同块对照（原生 pColor / B 合成 / 喂入 1080p / 答案 1080p / B 成品 4K）=====
//   目的：一次重启就能回答四个问题 ——
//     ① 答案和喂入差多少（网络到底改了多少）
//     ② B 的合成结果和原生差多少（残差有没有叠上）
//     ③ B 的成品和原生差多少（最终画面上还差多少）
//     ④ 三者的均值是否量级一致（排除格式/通道错位）
static const UINT kDiagBlk = 16;                 // 每块 16×16 像素
static const UINT kDiagPitch = 256;              // 行距必须 256 对齐
static const UINT kDiagBlkBytes = 16 * 256;      // 一块占的字节
static ComPtr<ID3D12Resource> g_diagRb;
static ComPtr<ID3D12CommandQueue> g_diagQ;
static ComPtr<ID3D12CommandAllocator> g_diagAl;
static ComPtr<ID3D12GraphicsCommandList> g_diagCl;
static ComPtr<ID3D12Fence> g_diagFe;
static UINT64 g_diagFv = 0;
static bool g_diagReady = false;

static bool BformDiagEnsure(ID3D12Device* dev) {
    if (g_diagReady) return true;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 6 * kDiagBlkBytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_diagRb)))) return false;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_diagQ)))) return false;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_diagAl)))) return false;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_diagAl.Get(), nullptr, IID_PPV_ARGS(&g_diagCl)))) return false;
    g_diagCl->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_diagFe)))) return false;
    g_diagReady = true;
    Log("DIAG-B: 就绪");
    return true;
}

static double BlkMean(const unsigned char* p, UINT pitch);   // 前沿声明（BlkStd 要用）
static double BlkStd(const unsigned char* p, UINT pitch) {
    double m = BlkMean(p, pitch), s = 0.0;
    for (UINT y = 0; y < kDiagBlk; ++y)
        for (UINT x = 0; x < kDiagBlk; ++x) {
            const unsigned char* q = p + (size_t)y * pitch + (size_t)x * 4;
            double v = (q[0] + q[1] + q[2]) / 3.0;
            s += (v - m) * (v - m);
        }
    return sqrt(s / (double)(kDiagBlk * kDiagBlk));
}
static double BlkMean(const unsigned char* p, UINT pitch) {
    double s = 0.0;
    for (UINT y = 0; y < kDiagBlk; ++y)
        for (UINT x = 0; x < kDiagBlk; ++x) {
            const unsigned char* q = p + (size_t)y * pitch + (size_t)x * 4;
            s += (q[0] + q[1] + q[2]) / 3.0;
        }
    return s / (double)(kDiagBlk * kDiagBlk);
}

// 两块同尺寸区域的"平均绝对差"（逐通道平均后取均值，单位 = 8bit 灰度级）
static double BlkDiff(const unsigned char* a, UINT pa, const unsigned char* b, UINT pb) {
    double s = 0.0;
    for (UINT y = 0; y < kDiagBlk; ++y)
        for (UINT x = 0; x < kDiagBlk; ++x) {
            const unsigned char* qa = a + (size_t)y * pa + (size_t)x * 4;
            const unsigned char* qb = b + (size_t)y * pb + (size_t)x * 4;
            s += (fabs((double)qa[0] - qb[0]) + fabs((double)qa[1] - qb[1]) + fabs((double)qa[2] - qb[2])) / 3.0;
        }
    return s / (double)(kDiagBlk * kDiagBlk);
}

// 把 4 块 16×16 拷进回读缓冲（挂在 worker 自己的 list 上，WaitW 之后才读）
static bool g_diagPending = false;
static void BformDiagEnqueue(Worker& W, ID3D12Resource* ans) {
    static int tick = 0;
    if (!g_bformOn) return;
    const int m = tick++ % 150;
    if (m != 0 && m != 1) return;   // 每 150 帧【连续采两帧】：用于"答案延迟"测量（第 2 帧比第 1 帧的喂入）
    if (!g_bCompReady || !g_bUpReady || !g_bTexC || !g_bComp || !g_bUp4k || !g_bDs1080) {
        Log("DIAG4: 资源未就绪，跳过（comp=%d up=%d）", (int)g_bCompReady, (int)g_bUpReady);
        return;
    }
    if (!BformDiagEnsure(W.dev.Get()) || !g_diagRb) { Log("DIAG4: 回读缓冲创建失败"); return; }
    ID3D12GraphicsCommandList* cl = W.cl.Get();
    auto blk = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT sx, UINT sy, UINT64 off, D3D12_RESOURCE_STATES st) {
        if (!tex) return;
        Barrier(cl, tex, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = g_diagRb.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        d.PlacedFootprint.Offset = off;
        d.PlacedFootprint.Footprint.Format = fmt;
        d.PlacedFootprint.Footprint.Width = kDiagBlk;
        d.PlacedFootprint.Footprint.Height = kDiagBlk;
        d.PlacedFootprint.Footprint.Depth = 1;
        d.PlacedFootprint.Footprint.RowPitch = kDiagPitch;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = tex; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
        D3D12_BOX box{ sx, sy, 0, sx + kDiagBlk, sy + kDiagBlk, 1 };
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
        Barrier(cl, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
    };
    const UINT nxs = (g_upW  > kDiagBlk + 2) ? (g_upW  / 2 - kDiagBlk / 2) : 0;
    const UINT nys = (g_upH  > kDiagBlk + 2) ? (g_upH  / 2 - kDiagBlk / 2) : 0;
    const UINT sxs = (kNetW  > kDiagBlk + 2) ? (kNetW  / 2 - kDiagBlk / 2) : 0;
    const UINT sys = (kNetH  > kDiagBlk + 2) ? (kNetH  / 2 - kDiagBlk / 2) : 0;
    const UINT oxs = (g_outW > kDiagBlk + 2) ? (g_outW / 2 - kDiagBlk / 2) : 0;
    const UINT oys = (g_outH > kDiagBlk + 2) ? (g_outH / 2 - kDiagBlk / 2) : 0;
    blk(g_bTexC.Get(),   g_upFmtC, nxs, nys, 0 * kDiagBlkBytes, D3D12_RESOURCE_STATE_COMMON);
    blk(g_bComp.Get(),   g_upFmtC, nxs, nys, 1 * kDiagBlkBytes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    blk(g_bDs1080.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, sxs, sys, 2 * kDiagBlkBytes, D3D12_RESOURCE_STATE_COMMON);
    blk(g_bUp4k.Get(),   g_upFmtC, oxs, oys, 3 * kDiagBlkBytes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // ★ 第 5 块直接取【本帧的答案纹理 po】：不再用 CPU 回读的那份（那份现在晚一轮）
    if (ans) blk(ans, DXGI_FORMAT_R8G8B8A8_UNORM, sxs, sys, 4 * kDiagBlkBytes, D3D12_RESOURCE_STATE_COMMON);
    // ★ 第 6 块：TAA 的输出。用来判断 TAA 有没有真的在混历史（输出≈输入 = 空转）
    if (g_taaOn && g_taaValid && g_taaOut)
        blk(g_taaOut.Get(), g_upFmtC, oxs, oys, 5 * kDiagBlkBytes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_diagPending = true;
}

static void BformDiagReadback(Worker& W) {
    if (!g_diagPending || !g_diagRb) return;
    g_diagPending = false;
    D3D12_RANGE rr{ 0, 6 * kDiagBlkBytes };
    void* m = nullptr;
    if (FAILED(g_diagRb->Map(0, &rr, &m)) || !m) { Log("DIAG4: 回读 Map 失败"); return; }
    const unsigned char* pN = (const unsigned char*)m + 0 * kDiagBlkBytes;
    const unsigned char* pC = (const unsigned char*)m + 1 * kDiagBlkBytes;
    const unsigned char* pF = (const unsigned char*)m + 2 * kDiagBlkBytes;
    const unsigned char* pU = (const unsigned char*)m + 3 * kDiagBlkBytes;
    const unsigned char* pA = (const unsigned char*)m + 4 * kDiagBlkBytes;   // 本帧答案（GPU 侧直接取）
    const unsigned char* pT = (const unsigned char*)m + 5 * kDiagBlkBytes;   // TAA 输出（仅 bform=8 有效）

    double mN = BlkMean(pN, kDiagPitch);
    double mC = BlkMean(pC, kDiagPitch);
    double mF = BlkMean(pF, kDiagPitch);
    double mU = BlkMean(pU, kDiagPitch);
    double mA = BlkMean(pA, kDiagPitch);
    double dAF = BlkDiff(pA, kDiagPitch, pF, kDiagPitch);     // 网络改了多少（同帧，无错配）
    double dCN = BlkDiff(pC, kDiagPitch, pN, kDiagPitch);     // B 合成 vs 原生
    double dUN = BlkDiff(pU, kDiagPitch, pN, kDiagPitch);     // B 成品 vs 原生
    // TAA 到底动了多少：输出 vs 它的输入（Catmull-Rom 结果）
    //   ≈0  → TAA 在空转（历史被门限/钳制全丢了，或初始化标志一直是"无历史"）
    //   有量 → TAA 真在混合历史，那么观感没改善就是"混合没帮上忙"（该考虑换 FSR4）
    double dTU = (g_taaOn && g_taaValid) ? BlkDiff(pT, kDiagPitch, pU, kDiagPitch) : -1.0;
    // 【降噪指标】块内标准差 σ：TAA 若真在累积，输出 σ 应明显低于输入 σ（噪点被平均掉）
    double sU = (g_taaOn && g_taaValid) ? BlkStd(pU, kDiagPitch) : -1.0;   // 输入（Catmull-Rom 结果）
    double sT = (g_taaOn && g_taaValid) ? BlkStd(pT, kDiagPitch) : -1.0;   // TAA 输出
    double sC = BlkStd(pC, kDiagPitch);                                    // B 合成结果（参考）
    double mT  = (g_taaOn && g_taaValid) ? BlkMean(pT, kDiagPitch) : -1.0;

    // 【答案延迟测量】DIAG4 每 150 帧连续采两次，第二次用第一次的"喂入块"跟本帧答案比：
    //   若 |答案 − 上轮喂入| 明显小于 |答案 − 本轮喂入| ⇒ 答案属于上一帧（滞后一帧）
    //   若两者接近            ⇒ 答案就是本帧的（没有滞后）
    //   这块采样点在画面中心，两次采样是相邻两帧，所以"上轮喂入"就是前一帧同一位置。
    static unsigned char prevFed[kDiagBlk * kDiagPitch];
    static bool  havePrevFed = false;
    static bool  diagSecond = false;
    double dAFprev = -1.0;
    if (diagSecond && havePrevFed) dAFprev = BlkDiff(pA, kDiagPitch, prevFed, kDiagPitch);
    memcpy(prevFed, pF, sizeof(prevFed));
    havePrevFed = true;
    diagSecond = !diagSecond;

    D3D12_RANGE nw{ 0, 0 };
    g_diagRb->Unmap(0, &nw);
    g_diagCnt++;
    Log("DIAG4[%d] gframe=%d stage=%d: 均值 原生=%.1f 喂入=%.1f 答案=%.1f 合成=%.1f 成品=%.1f TAA=%.1f | |答案-喂入|=%.2f |答案-上轮喂入|=%.2f |合成-原生|=%.2f |成品-原生|=%.2f |TAA变化|=%.2f |σ: 合成=%.2f 上采样=%.2f TAA=%.2f",
        W.processed, g_frame, g_bformStage, mN, mF, mA, mC, mU, mT, dAF, dAFprev, dCN, dUN, dTU, sC, sU, sT);
}
static bool BformBUpscale(ID3D12GraphicsCommandList* cl) {
    if (!g_bUpReady || !g_bComp) return false;
    Barrier(cl, g_bComp.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bUp4k.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetComputeRootSignature(g_bUpRs.Get());
    cl->SetPipelineState(g_bUpPso.Get());
    {   // 描述符表：2=源 SRV，3=目标 UAV（与 DS 共用同一个堆）
        auto mkS = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * g_bAuxInc;
            g_worker.dev->CreateShaderResourceView(r, &sd, h);
        };
        auto mkU = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)i * g_bAuxInc;
            g_worker.dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
        };
        mkS(g_bComp.Get(), g_upFmtC, 2);
        mkU(g_bUp4k.Get(), g_upFmtC, 3);
        ID3D12DescriptorHeap* hs[] = { g_bAuxHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        D3D12_GPU_DESCRIPTOR_HANDLE g = g_bAuxHeap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += (SIZE_T)2 * g_bAuxInc;
        cl->SetComputeRootDescriptorTable(0, g);
        D3D12_GPU_DESCRIPTOR_HANDLE g1 = g; g1.ptr += g_bAuxInc;
        cl->SetComputeRootDescriptorTable(1, g1);
    }
    cl->Dispatch((g_outW + 7) / 8, (g_outH + 7) / 8, 1);
    Barrier(cl, g_bUp4k.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if ((g_bUpCnt++ % 300) == 0) Log("BFORM-B: 已上采样第 %u 次（%ux%u）", g_bUpCnt, g_outW, g_outH);
    return true;
}

// ---- B 形态 步骤5b：TAA-lite 时域上采样（治噪点 / 锯齿）----
//   输入：上采样后的 4K 当前帧 + 上一帧的 4K 输出（历史）+ 运动矢量（1080p）
//   做法：MV 重投影 → 邻域钳制（防拖影）→ 与历史混合（alpha 越小越稳）
//   输出：4K → 出画；同时写进另一张历史，下一帧两张对调
//   注：本版【不做 jitter 补偿】（先看时域累积本身的效果），jitter 走共享 buffer 头部是下一步
static const char* kBformTAA = R"(
Texture2D<float4>   gCur  : register(t0);
Texture2D<float4>   gHist : register(t1);
Texture2D<float4>   gMV   : register(t2);
SamplerState        gSmp  : register(s0);
RWTexture2D<float4> gDst  : register(u0);
RWTexture2D<float4> gHO   : register(u1);
cbuffer P : register(b0)
{
    float gAlpha;     // 混合系数：越小越稳（越容易拖影）
    float gClampK;    // 邻域钳制放宽系数：越小越防拖影
    float gMvScale;   // MV 像素→uv 缩放（沿用残差那套）
    float gInit;      // 1 = 历史无效，直接输出当前帧
};
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float3 c = gCur.Load(int3((int)id.x, (int)id.y, 0)).rgb;
    float3 outc = c;

    if (gInit < 0.5) {
        // 邻域钳制：当前帧 3×3 的 min/max，历史必须落在里面（抑制拖影）
        float3 mn = c, mx = c;
        for (int j = -2; j <= 2; ++j) {      // 5×5 邻域（3×3 时钳制窗口太窄，TAA 等于空转）
            for (int i = -2; i <= 2; ++i) {
                int2 q = clamp(int2(id.xy) + int2(i, j), int2(0, 0), int2((int)w - 1, (int)h - 1));
                float3 sm = gCur.Load(int3(q, 0)).rgb;
                mn = min(mn, sm); mx = max(mx, sm);
            }
        }
        float3 ext = (mx - mn) * 0.5 * gClampK;
        mn -= ext; mx += ext;
        // MV 重投影：把历史对齐到当前像素
        float2 mv = gMV.SampleLevel(gSmp, uv, 0).rg * gMvScale;
        float2 puv = saturate(uv - mv);
        float3 hcol = gHist.SampleLevel(gSmp, puv, 0).rgb;
        hcol = clamp(hcol, mn, mx);
        outc = lerp(c, hcol, gAlpha);
    }
    gDst[id.xy] = float4(outc, 1.0);
    gHO[id.xy]  = float4(outc, 1.0);
}
)";
static ComPtr<ID3D12Resource> g_taaHist[2];
static ComPtr<ID3D12RootSignature> g_taaRs;
static ComPtr<ID3D12PipelineState> g_taaPso;
static ComPtr<ID3D12DescriptorHeap> g_taaHeap;
static UINT g_taaInc = 0;
static int  g_taaIdx = 0;

static bool BformTaaEnsure(ID3D12Device* dev) {
    if (g_taaReady) return true;
    if (!dev || !g_outW || !g_outH) return false;
    const DXGI_FORMAT fmt = g_upFmtC;

    g_taaHist[0] = MakeTex(dev, g_outW, g_outH, fmt, D3D12_RESOURCE_STATE_COMMON, true);
    g_taaHist[1] = MakeTex(dev, g_outW, g_outH, fmt, D3D12_RESOURCE_STATE_COMMON, true);
    g_taaOut     = MakeTex(dev, g_outW, g_outH, fmt, D3D12_RESOURCE_STATE_COMMON, true);
    if (!g_taaHist[0] || !g_taaHist[1] || !g_taaOut) { Log("BFORM-TAA: 建纹理失败"); return false; }

    // 根签名：t0/t1/t2 三张 SRV 表 + u0/u1 两张 UAV 表 + 4 个 32 位常量（b0）
    D3D12_DESCRIPTOR_RANGE rgS[3]{}, rgU[2]{};
    for (int i = 0; i < 3; ++i) {
        rgS[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rgS[i].NumDescriptors = 1; rgS[i].BaseShaderRegister = (UINT)i;
    }
    for (int i = 0; i < 2; ++i) {
        rgU[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        rgU[i].NumDescriptors = 1; rgU[i].BaseShaderRegister = (UINT)i;
    }
    D3D12_ROOT_PARAMETER rp[6]{};
    for (int i = 0; i < 3; ++i) {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[i].DescriptorTable.NumDescriptorRanges = 1;
        rp[i].DescriptorTable.pDescriptorRanges = &rgS[i];
    }
    for (int i = 0; i < 2; ++i) {
        rp[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[3 + i].DescriptorTable.NumDescriptorRanges = 1;
        rp[3 + i].DescriptorTable.pDescriptorRanges = &rgU[i];
    }
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[5].Constants.Num32BitValues = 4;
    rp[5].Constants.ShaderRegister = 0;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 6; rd.pParameters = rp;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        Log("BFORM-TAA: 根签名失败: %s", err ? (char*)err->GetBufferPointer() : "?"); return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                        IID_PPV_ARGS(&g_taaRs)))) { Log("BFORM-TAA: CreateRootSignature 失败"); return false; }

    ComPtr<ID3DBlob> cs, e2;
    if (FAILED(D3DCompile(kBformTAA, strlen(kBformTAA), "btaa.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &e2))) {
        Log("BFORM-TAA: 编译失败: %s", e2 ? (char*)e2->GetBufferPointer() : "?"); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_taaRs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_taaPso)))) { Log("BFORM-TAA: PSO 失败"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 5;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_taaHeap)))) return false;
    g_taaInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    g_taaReady = true;
    Log("BFORM-TAA: TAA-lite 就绪 | %ux%u fmt=%d | alpha=%.2f clampK=%.2f（本版未做 jitter 补偿）",
        g_outW, g_outH, (int)fmt, (float)g_taaAlpha, (float)g_taaClampK);
    return true;
}

// 出画源：TAA 生效时用 TAA 输出，否则用原始上采样结果
static ID3D12Resource* BformPresentSrc() {
    if (g_bform4k && g_bCompReady && g_bComp) return g_bComp.Get();   // 4K 后置：合成结果直接就是 4K，不走放大
    if (g_taaOn && g_taaReady && g_taaValid) return g_taaOut.Get();
    return g_bUp4k.Get();
}

static bool BformTaaRun(ID3D12GraphicsCommandList* cl) {
    if (!g_taaReady || !cl || !g_bUp4k || !g_bTexM) return false;
    ID3D12Device* dev = g_worker.dev.Get();
    if (!dev) return false;
    const int prev = g_taaIdx, next = 1 - g_taaIdx;

    auto cpu = [&](int i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_taaHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * g_taaInc; return h;
    };
    auto mksrv = [&](ID3D12Resource* r, DXGI_FORMAT f, int i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(r, &sd, cpu(i));
    };
    auto mkuav = [&](ID3D12Resource* r, DXGI_FORMAT f, int i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(r, nullptr, &ud, cpu(i));
    };
    mksrv(g_bUp4k.Get(), g_upFmtC, 0);             // 当前帧
    mksrv(g_taaHist[prev].Get(), g_upFmtC, 1);     // 历史
    mksrv(g_bTexM.Get(), g_upFmtM, 2);             // 运动矢量
    mkuav(g_taaOut.Get(), g_upFmtC, 3);            // 输出
    mkuav(g_taaHist[next].Get(), g_upFmtC, 4);     // 下一帧历史

    Barrier(cl, g_taaHist[prev].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_taaOut.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cl, g_taaHist[next].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* hs[] = { g_taaHeap.Get() };
    cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(g_taaRs.Get());
    cl->SetPipelineState(g_taaPso.Get());
    auto gpu = [&](int i) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = g_taaHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * g_taaInc; return h;
    };
    for (int i = 0; i < 5; ++i) cl->SetComputeRootDescriptorTable(i, gpu(i));
    float k[4] = { (float)g_taaAlpha, (float)g_taaClampK, g_mvScale, g_taaValid ? 0.0f : 1.0f };
    cl->SetComputeRoot32BitConstants(5, 4, k, 0);
    cl->Dispatch((g_outW + 7) / 8, (g_outH + 7) / 8, 1);

    Barrier(cl, g_taaOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cl, g_taaHist[next].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_taaHist[prev].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(cl, g_bTexM.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    g_taaIdx = next;
    g_taaValid = true;
    static UINT cnt = 0;
    if ((cnt++ % 300) == 0) Log("BFORM-TAA: 已时域累积 %u 次（%ux%u）", cnt, g_outW, g_outH);
    return true;
}

// ---- B 形态 步骤6：B 侧出画（合成交换链 + DComp 视觉贴到游戏窗口上）----
//   数据源 = g_bUp4k（B 自己上采样出来的 4K）
//   关键：只在 worker 的 GPU 工作【完全落地之后】才拷贝+Present，
//         避免"跨队列读写同一资源"（S1 第一次闪退就是栽在这）。
static bool g_bpOn = false;
static ComPtr<IDCompositionDevice> g_bpDcomp;
static ComPtr<IDCompositionTarget> g_bpTarget;
static ComPtr<IDCompositionVisual> g_bpVisual;
static ComPtr<IDXGISwapChain3>     g_bpSc;
static ComPtr<ID3D12CommandQueue>  g_bpQ;
static ComPtr<ID3D12CommandAllocator> g_bpAl, g_bpAl2;
static int g_bpAi = 0;   // 出画分配器轮转（单分配器复用且不确认完成 = D3D12 非法用法）
static ComPtr<ID3D12GraphicsCommandList> g_bpCl;
static ComPtr<ID3D12Fence>         g_bpFe;
static UINT64 g_bpFv = 0;
static UINT   g_bpCnt = 0;
static DXGI_FORMAT g_bpFmt = DXGI_FORMAT_UNKNOWN;
static ComPtr<IDCompositionScaleTransform> g_bpScale;   // 覆盖层缩放（跟随窗口客户区）
static UINT g_bpCw = 0, g_bpCh = 0;                     // 建立缩放时用的客户区尺寸
// 出画节奏统计（判断"卡卡的"是不是因为出画间隔不匀）
static double g_bpT0 = 0;
static double g_bpSumMs = 0, g_bpMinMs = 1e9, g_bpMaxMs = 0;
static int    g_bpShort = 0, g_bpLong = 0;   // <8ms（一刷新内出两帧）/ >25ms（漏帧）

// 覆盖层可见性跟随 stage：stage>=6 显示 B 的输出；切回 A（stage<6）就把视觉内容清空。
// 不这么做的话，切回 A 之后 DComp 会留一张冻结的 B 画面盖在游戏窗口上，看不到 A。
static void BformPresentShow(bool show) {
    static int cur = -1;   // -1 = 还没设置过
    if (!g_bpReady || !g_bpVisual || !g_bpDcomp) return;
    const int want = show ? 1 : 0;
    if (cur == want) return;
    cur = want;
    HRESULT hr = g_bpVisual->SetContent(show ? (IUnknown*)g_bpSc.Get() : nullptr);
    if (SUCCEEDED(hr)) g_bpDcomp->Commit();
    Log("BFORM-P: 覆盖层 %s（stage=%d）", show ? "显示" : "卸载", g_bformStage);
}

static bool BformPresentInit() {
    if (g_bpReady) return true;
    if (g_bpFailed) return false;
    if (!g_worker.dev || !g_bUp4k) { return false; }
    // 自己去找窗口（以前这里是"直接放弃 → 只能靠用户按 Del 开菜单顺带取到句柄"）
    if (!ResolveGameHwnd()) { static int n = 0; if ((n++ % 300) == 0) Log("BFORM-P: 还没定位到游戏主窗口，稍后重试（无需开菜单）"); return false; }

    // 用 B 的适配器
    LUID luid = g_worker.dev->GetAdapterLuid();
    ComPtr<IDXGIFactory4> fac4;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac4)))) { Log("BFORM-P: 工厂失败"); g_bpFailed = true; return false; }
    ComPtr<IDXGIAdapter1> ad;
    if (FAILED(fac4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad))) || !ad) {
        Log("BFORM-P: 按 LUID 找不到 B 适配器"); g_bpFailed = true; return false;
    }

    // 自己的 DIRECT 队列（Present 必须在建链的那条队列上）
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_worker.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_bpQ)))) { Log("BFORM-P: 建队列失败"); g_bpFailed = true; return false; }
    if (FAILED(g_worker.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_bpAl)))) { g_bpFailed = true; return false; }
    if (FAILED(g_worker.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_bpAl2)))) { g_bpFailed = true; return false; }
    if (FAILED(g_worker.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bpAl.Get(), nullptr, IID_PPV_ARGS(&g_bpCl)))) { g_bpFailed = true; return false; }
    g_bpCl->Close();
    if (FAILED(g_worker.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_bpFe)))) { g_bpFailed = true; return false; }

    // 合成交换链：格式与 g_bUp4k 一致，才能直接 CopyResource
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_outW; sd.Height = g_outH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // 固定 8bit（与合成输出一致；底图 10bit 也不影响这里）
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = fac4->CreateSwapChainForComposition(g_bpQ.Get(), &sd, nullptr, &sc1);
    if (FAILED(hr) || !sc1) { Log("BFORM-P: 合成交换链失败 0x%08X（B 可能没有输出）", (unsigned)hr); g_bpFailed = true; return false; }
    if (FAILED(sc1.As(&g_bpSc)) || !g_bpSc) { Log("BFORM-P: QI 失败"); g_bpFailed = true; return false; }
    g_bpSc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    g_bpFmt = g_upFmtC;

    // DComp 设备（D3D12 设备不能当 IDXGIDevice，失败则用同卡 D3D11 设备）
    hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&g_bpDcomp));
    if (FAILED(hr) || !g_bpDcomp) {
        ComPtr<ID3D11Device> d11; D3D_FEATURE_LEVEL fl{};
        HRESULT h2 = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                       nullptr, 0, D3D11_SDK_VERSION, &d11, &fl, nullptr);
        ComPtr<IDXGIDevice> dx;
        if (FAILED(h2) || !d11 || FAILED(d11.As(&dx)) || !dx) { Log("BFORM-P: D3D11 设备失败"); g_bpFailed = true; return false; }
        hr = DCompositionCreateDevice(dx.Get(), IID_PPV_ARGS(&g_bpDcomp));
        if (FAILED(hr) || !g_bpDcomp) { Log("BFORM-P: DComp 失败 0x%08X", (unsigned)hr); g_bpFailed = true; return false; }
    }
    hr = g_bpDcomp->CreateTargetForHwnd(g_gameHwnd, TRUE, &g_bpTarget);
    if (FAILED(hr) || !g_bpTarget) { Log("BFORM-P: CreateTargetForHwnd 失败 0x%08X", (unsigned)hr); g_bpFailed = true; return false; }
    hr = g_bpDcomp->CreateVisual(&g_bpVisual);
    if (FAILED(hr) || !g_bpVisual) { Log("BFORM-P: CreateVisual 失败 0x%08X", (unsigned)hr); g_bpFailed = true; return false; }
    g_bpVisual->SetContent(g_bpSc.Get());
    {
        // 覆盖层缩放 = 客户区 / B 出画尺寸。
        // ★ 现在可能在"进游戏之前"就初始化（不再等菜单），窗口之后还可能改分辨率/窗口模式
        //   → 缩放要能跟着改，见 BformPresentRetarget()
        RECT cr{}; GetClientRect(g_gameHwnd, &cr);
        UINT cw = (UINT)(cr.right - cr.left), chh = (UINT)(cr.bottom - cr.top);
        g_bpCw = cw; g_bpCh = chh;
        if (SUCCEEDED(g_bpDcomp->CreateScaleTransform(&g_bpScale)) && g_bpScale) {
            g_bpScale->SetScaleX(cw ? (float)cw / (float)g_outW : 1.0f);
            g_bpScale->SetScaleY(chh ? (float)chh / (float)g_outH : 1.0f);
            g_bpVisual->SetTransform(g_bpScale.Get());
        }
    }
    hr = g_bpTarget->SetRoot(g_bpVisual.Get());
    if (FAILED(hr)) { Log("BFORM-P: SetRoot 失败 0x%08X", (unsigned)hr); g_bpFailed = true; return false; }
    g_bpDcomp->Commit();

    // 先呈一帧黑，避免视觉挂上去时是未初始化的内容
    {
        UINT bi = g_bpSc->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> bb;
        if (SUCCEEDED(g_bpSc->GetBuffer(bi, IID_PPV_ARGS(&bb))) && bb) {
            g_bpAl->Reset(); g_bpCl->Reset(g_bpAl.Get(), nullptr);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = bb.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            g_bpCl->ResourceBarrier(1, &b);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            g_bpCl->ResourceBarrier(1, &b);
            if (SUCCEEDED(g_bpCl->Close())) {
                ID3D12CommandList* l[] = { g_bpCl.Get() };
                g_bpQ->ExecuteCommandLists(1, l);
            }
            g_bpSc->Present(0, 0);
    { static int pc=0; if((pc++ % 300)==0) Log("BFORM-PDBG[帧%d] present #%d 后缓冲=%u", (int)g_frame, pc, bi); }
        }
    }

    g_bpReady = true;
    Log("BFORM-P: 出画就绪 | %ux%u fmt=%d（B 用自己的交换链贴到游戏窗口）", g_outW, g_outH, (int)g_bpFmt);
    return true;
}

// 窗口客户区变了（改分辨率 / 切窗口模式 / 早点初始化时窗口还没定尺寸）→ 跟上缩放
static void BformPresentRetarget() {
    if (!g_bpReady || !g_bpScale || !g_gameHwnd || !g_bpDcomp) return;
    RECT cr{}; if (!GetClientRect(g_gameHwnd, &cr)) return;
    UINT cw = (UINT)(cr.right - cr.left), chh = (UINT)(cr.bottom - cr.top);
    if (!cw || !chh || (cw == g_bpCw && chh == g_bpCh)) return;
    g_bpCw = cw; g_bpCh = chh;
    const float sx = (float)cw / (float)g_outW, sy = (float)chh / (float)g_outH;
    g_bpScale->SetScaleX(sx);
    g_bpScale->SetScaleY(sy);
    g_bpDcomp->Commit();
    Log("BFORM-P: 窗口客户区变为 %ux%u → 覆盖层缩放 %.3fx%.3f", cw, chh, sx, sy);
}

// 只在 worker 的 GPU 工作完全落地之后调用
static void BformPresentFrame() {
    if (!g_bpReady) return;
    BformPresentRetarget();
    ID3D12Resource* src = BformPresentSrc();   // TAA 生效时取 TAA 输出，否则取原始上采样结果
    if (!src) return;
    UINT bi = g_bpSc->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    if (FAILED(g_bpSc->GetBuffer(bi, IID_PPV_ARGS(&bb))) || !bb) return;
    // ★ 出画分配器轮转：每帧换一个，避免"上一帧的拷贝还没执行完就 Reset"（D3D12 非法用法）
    g_bpAi ^= 1;
    ID3D12CommandAllocator* presAl = g_bpAi ? g_bpAl2.Get() : g_bpAl.Get();
    {
        bool ok = false;
        for (int tryI = 0; tryI < 2 && !ok; ++tryI) {
            ID3D12CommandAllocator* cand = (g_bpAi ^ tryI) ? g_bpAl2.Get() : g_bpAl.Get();
            if (SUCCEEDED(cand->Reset()) && SUCCEEDED(g_bpCl->Reset(cand, nullptr))) { ok = true; g_bpAi ^= (tryI ? 1 : 0); }
        }
        if (!ok) {
            // 有界等待上一份出画命令完成，再试一次（避免整帧跳过）
            ULONGLONG t0 = GetTickCount64();
            while (g_bpFv && g_bpFe && g_bpFe->GetCompletedValue() < g_bpFv && (GetTickCount64() - t0) < 20) SwitchToThread();
            ID3D12CommandAllocator* cand = g_bpAi ? g_bpAl2.Get() : g_bpAl.Get();
            if (FAILED(cand->Reset()) || FAILED(g_bpCl->Reset(cand, nullptr))) {
                static int pskip = 0; if ((pskip++ % 60) == 0) Log("!! BFORM-P: 出画整帧被跳过（分配器复用失败）累计 %d", pskip);
                return;
            }
        }
    }
    // (旧的多余花括号已移除)
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.pResource = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_bpCl->ResourceBarrier(1, &b);
    b.Transition.pResource = bb.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_bpCl->ResourceBarrier(1, &b);
    g_bpCl->CopyResource(bb.Get(), src);
    b.Transition.pResource = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    g_bpCl->ResourceBarrier(1, &b);
    b.Transition.pResource = bb.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_bpCl->ResourceBarrier(1, &b);
    if (FAILED(g_bpCl->Close())) return;
    // ★ GPU 侧同步（不占 CPU）：出画队列必须等主队列把上采样算完，否则可能拷到写了一半或上一帧的画面。
    //   以前这里完全没有同步，靠"能跑"。
    if (g_worker.fe && g_worker.q) g_bpQ->Wait(g_worker.fe.Get(), g_worker.fv);
    ID3D12CommandList* l[] = { g_bpCl.Get() };
    g_bpQ->ExecuteCommandLists(1, l);
    g_bpQ->Signal(g_bpFe.Get(), ++g_bpFv);
    g_bpSc->Present(0, 0);

    // 出画节奏统计：间隔不匀 = 肉眼"卡卡的"
    {
        static LARGE_INTEGER fq{};
        if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double t = (double)now.QuadPart * 1000.0 / (double)fq.QuadPart;
        if (g_bpT0 > 0) {
            double dt = t - g_bpT0;
            g_bpSumMs += dt;
            if (dt < g_bpMinMs) g_bpMinMs = dt;
            if (dt > g_bpMaxMs) g_bpMaxMs = dt;
            if (dt < 8.0)  ++g_bpShort;
            if (dt > 25.0) ++g_bpLong;
        }
        g_bpT0 = t;
    }
    if ((g_bpCnt++ % 300) == 0) {
        int n = 300;
        if (g_bpSumMs > 0)
            Log("BFORM-P: 产出 %u 帧 | 间隔 均值%.2fms 最小%.2f 最大%.2f | 过短(<8ms) %d 次 过长(>25ms) %d 次（每300帧）",
                g_bpCnt, g_bpSumMs / n, g_bpMinMs, g_bpMaxMs, g_bpShort, g_bpLong);
        g_bpSumMs = 0; g_bpMinMs = 1e9; g_bpMaxMs = 0; g_bpShort = 0; g_bpLong = 0;
    }
}

// =====================================================================================
// 【B 形态 S7：两级流水】把"解码+降采样"与"合成+上采样+出画"拆到两条队列、两个缓冲
//   Stage1（worker 的 compute 队列）：解码 + 降采样 + NR        （处理帧 N）
//   Stage2（g_bpQ，DIRECT 队列）    ：合成 + 上采样 + 出画      （处理帧 N-1）
//   两段跑在不同缓冲组上，所以 Stage2(N-1) 不会被 Stage1(N) 覆盖。
//   目标：每帧吞吐从 19.3ms（串行）压到 max(NR+解码, 合成+上采样) ≈ 15ms
// =====================================================================================
static const int PP = 2;
static ComPtr<ID3D12Resource> g_ppTexC[PP], g_ppTexM[PP], g_ppDs[PP];
static ComPtr<ID3D12Resource> g_ppComp[PP], g_ppUp4k[PP];
static UINT g_ppCnt = 0;

static bool BformPPEnsure(ID3D12Device* dev) {
    if (g_ppReady) return true;
    if (!dev || !g_upW || !g_outW) return false;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto mk = [&](UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl,
                  ComPtr<ID3D12Resource>& r, const char* nm) -> bool {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = f; td.SampleDesc.Count = 1; td.Flags = fl;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        if (FAILED(hr)) { Log("BFORM-PP: 建 %s 失败 0x%08X", nm, (unsigned)hr); return false; }
        return true;
    };
    for (int i = 0; i < PP; ++i) {
        if (!mk(g_upW, g_upH, g_upFmtC, D3D12_RESOURCE_FLAG_NONE, g_ppTexC[i], "color")) return false;
        if (g_upMw && !mk(g_upMw, g_upMh, g_upFmtM, D3D12_RESOURCE_FLAG_NONE, g_ppTexM[i], "mv")) return false;
        if (!mk(kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, g_ppAns[i], "ans")) return false;
        if (!mk(kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, g_ppDs[i], "ds")) return false;
        if (!mk(g_upW, g_upH, g_upFmtC, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, g_ppComp[i], "comp")) return false;
        if (!mk(g_outW, g_outH, g_upFmtC, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, g_ppUp4k[i], "up4k")) return false;
    }
    g_ppReady = true;
    Log("BFORM-PP: 双缓冲就绪（%d 组）| 色 %ux%u | 合成 %ux%u | 成品 %ux%u",
        PP, g_upW, g_upH, g_upW, g_upH, g_outW, g_outH);
    return true;
}

// Stage1 记在 worker 的 compute 列表上：解码 + 降采样（目标本组缓冲）
static void BformPPStage1(ID3D12GraphicsCommandList* cl, int i, ID3D12Resource* nrColorDst,
                          ID3D12Resource* ansSnapDst, ID3D12Resource* po) {
    if (!g_ppReady) return;
    const UINT64 base = (UINT64)(g_frame % K) * g_upSlotBytes;
    Barrier(cl, g_upBufW.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto dec = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch, UINT64 off) {
        if (!tex || !w || !h) return;
        Barrier(cl, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{};
        d.pResource = tex; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{};
        s.pResource = g_upBufW.Get(); s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint.Offset = base + off;
        s.PlacedFootprint.Footprint.Format = fmt;
        s.PlacedFootprint.Footprint.Width = w;
        s.PlacedFootprint.Footprint.Height = h;
        s.PlacedFootprint.Footprint.Depth = 1;
        s.PlacedFootprint.Footprint.RowPitch = pitch;
        cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    };
    dec(g_ppTexC[i].Get(), g_upFmtC, g_upW, g_upH, g_upPitchC, g_upOffC);
    dec(g_ppTexM[i].Get(), g_upFmtM, g_upMw, g_upMh, g_upPitchM, g_upOffM);
    Barrier(cl, g_upBufW.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    // 降采样 → NR 输入
    if (g_bDsReady && nrColorDst) {
        Barrier(cl, g_ppTexC[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto mkS = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT k) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bAuxInc;
            g_worker.dev->CreateShaderResourceView(r, &sd, h);
        };
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        { D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
          h.ptr += (SIZE_T)1 * g_bAuxInc;
          g_worker.dev->CreateUnorderedAccessView(g_ppDs[i].Get(), nullptr, &ud, h); }
        mkS(g_ppTexC[i].Get(), g_upFmtC, 0);
        ID3D12DescriptorHeap* hs[] = { g_bAuxHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        cl->SetComputeRootSignature(g_bAuxRs.Get());
        cl->SetPipelineState(g_bDsPso.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE g = g_bAuxHeap->GetGPUDescriptorHandleForHeapStart();
        cl->SetComputeRootDescriptorTable(0, g);
        D3D12_GPU_DESCRIPTOR_HANDLE g1 = g; g1.ptr += g_bAuxInc;
        cl->SetComputeRootDescriptorTable(1, g1);
        cl->Dispatch((kNetW + 7) / 8, (kNetH + 7) / 8, 1);
        Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyResource(nrColorDst, g_bDs1080.Get());
        Barrier(cl, g_bDs1080.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
}

// Stage2：记在 g_bpQ（DIRECT 队列）上 —— 合成 + 上采样 + 出画，全部读【上一组】缓冲
//   与 Stage1(N) 并行运行；读的资源 Stage1 不会碰（不同组）
static bool BformPPStage2(int j) {
    if (!g_ppReady || !g_bpReady || !g_ppComp[j] || !g_ppUp4k[j]) return false;
    if (FAILED(g_bpAl->Reset())) return false;
    if (FAILED(g_bpCl->Reset(g_bpAl.Get(), nullptr))) return false;
    ID3D12GraphicsCommandList* cl = g_bpCl.Get();
    auto dev = g_worker.dev.Get();

    // ---- 合成：ppTexC[j] + ppTexM[j] + ppDs[j] + ppAns[j] → ppComp[j] ----
    if (g_bCompReady) {
        auto cpuSrv = [&](ID3D12Resource* r, DXGI_FORMAT f, int k) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bInc;
            dev->CreateShaderResourceView(r, &sd, h);
        };
        auto cpuUav = [&](ID3D12Resource* r, DXGI_FORMAT f, int k) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bInc;
            dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
        };
        cpuSrv(g_ppTexC[j].Get(), g_upFmtC, 0);
        cpuSrv(g_ppTexC[j].Get(), g_upFmtC, 1);
        if (g_ppTexM[j]) cpuSrv(g_ppTexM[j].Get(), g_upFmtM, 2);
        if (g_ppDs[j])   cpuSrv(g_ppDs[j].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 3);
        if (g_ppAns[j])  cpuSrv(g_ppAns[j].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 4);
        cpuSrv(g_ppTexC[j].Get(), g_upFmtC, 5);
        cpuUav(g_ppComp[j].Get(), g_upFmtC, 6);
        cpuUav(g_bState.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 7);

        Barrier(cl, g_ppTexC[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g_ppTexM[j]) Barrier(cl, g_ppTexM[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g_ppDs[j])   Barrier(cl, g_ppDs[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (g_ppAns[j])  Barrier(cl, g_ppAns[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_ppComp[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cl, g_bState.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12DescriptorHeap* hs[] = { g_bHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        cl->SetComputeRootSignature(g_bRs.Get());
        cl->SetPipelineState(g_bPso.Get());
        auto gpu = [&](int k) { D3D12_GPU_DESCRIPTOR_HANDLE h = g_bHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)k * g_bInc; return h; };
        for (int k = 0; k < 5; ++k) cl->SetComputeRootDescriptorTable(k, gpu(k));
        cl->SetComputeRootDescriptorTable(5, gpu(6));
        cl->SetComputeRootDescriptorTable(6, gpu(7));
        float alphaUse = (g_ppCnt < 8) ? 1.0f : g_alpha;
        float kk[20] = { g_limit, g_edge, g_smooth, g_gain, g_sat, alphaUse, g_varGain, g_temporalStatic,
                         g_mvScale, g_mvConf, 0, 0, g_gateK, 0, 0, 0, 0, 0, 0, 0 };
        cl->SetComputeRoot32BitConstants(7, 20, kk, 0);
        cl->Dispatch((g_upW + 7) / 8, (g_upH + 7) / 8, 1);

        Barrier(cl, g_ppComp[j].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_bState.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, g_ppTexC[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (g_ppTexM[j]) Barrier(cl, g_ppTexM[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (g_ppDs[j])   Barrier(cl, g_ppDs[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (g_ppAns[j])  Barrier(cl, g_ppAns[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    // ---- 上采样：ppComp[j] → ppUp4k[j] ----
    if (g_bUpReady) {
        auto mkS = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT k) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bAuxInc;
            dev->CreateShaderResourceView(r, &sd, h);
        };
        auto mkU = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT k) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_bAuxHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (SIZE_T)k * g_bAuxInc;
            dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
        };
        mkS(g_ppComp[j].Get(), g_upFmtC, 2);
        mkU(g_ppUp4k[j].Get(), g_upFmtC, 3);
        Barrier(cl, g_ppComp[j].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_ppUp4k[j].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* hs[] = { g_bAuxHeap.Get() };
        cl->SetDescriptorHeaps(1, hs);
        cl->SetComputeRootSignature(g_bAuxRs.Get());
        cl->SetPipelineState(g_bUpPso.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE g = g_bAuxHeap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += (SIZE_T)2 * g_bAuxInc;
        cl->SetComputeRootDescriptorTable(0, g);
        D3D12_GPU_DESCRIPTOR_HANDLE g1 = g; g1.ptr += g_bAuxInc;
        cl->SetComputeRootDescriptorTable(1, g1);
        cl->Dispatch((g_outW + 7) / 8, (g_outH + 7) / 8, 1);
        Barrier(cl, g_ppUp4k[j].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    // ---- 出画：ppUp4k[j] → 后缓冲 → Present ----
    {
        UINT bi = g_bpSc->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> bb;
        if (FAILED(g_bpSc->GetBuffer(bi, IID_PPV_ARGS(&bb))) || !bb) { g_bpCl->Close(); return false; }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.pResource = bb.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cl->ResourceBarrier(1, &b);
        cl->CopyResource(bb.Get(), g_ppUp4k[j].Get());
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cl->ResourceBarrier(1, &b);
    }
    if (FAILED(g_bpCl->Close())) return false;
    ID3D12CommandList* l[] = { g_bpCl.Get() };
    g_bpQ->ExecuteCommandLists(1, l);
    g_bpQ->Signal(g_bpFe.Get(), ++g_bpFv);
    g_bpSc->Present(0, 0);

    // 节奏统计
    {
        static LARGE_INTEGER fq{};
        if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double t = (double)now.QuadPart * 1000.0 / (double)fq.QuadPart;
        static double t0 = 0, sum = 0, mn = 1e9, mx = 0;
        static int sh = 0, lg = 0;
        if (t0 > 0) {
            double dt = t - t0;
            sum += dt; if (dt < mn) mn = dt; if (dt > mx) mx = dt;
            if (dt < 8.0) ++sh; if (dt > 25.0) ++lg;
        }
        t0 = t;
        if ((g_ppCnt++ % 300) == 0) {
            if (sum > 0) Log("BFORM-PP: 产出 %u 帧 | 间隔 均值%.2fms 最小%.2f 最大%.2f | 过短 %d 过长 %d",
                            g_ppCnt, sum / 300.0, mn, mx, sh, lg);
            sum = 0; mn = 1e9; mx = 0; sh = 0; lg = 0;
        }
    }
    return true;
}

// =====================================================================================
// Add4K 热重编译：源码放在 xess_proxy.add4k.hlsl，改完保存 → 约 1 秒内自动生效
// -------------------------------------------------------------------------------------
// · 首次运行若文件不存在，会把内置模板写出来（方便直接改）
// · 编译失败 → 保留旧版本，把编译器报错写进日志（不会崩、不会黑屏）
// · 换 PSO 时旧的那个延迟一代再释放，避免正在飞的命令还引用它
// =====================================================================================
static const char* kAdd4KTemplate = R"HLSL(
// Add4K —— 残差抬到 4K 后叠加（本文件可热重编译：保存后约 1 秒生效）
// 绑定固定，不能改：
//   t0 = 4K 底图(outp)   t1 = 合成后的 pColor(= 槽位原图 + e)   t2 = 槽位原图
//   u0 = 输出(4K)        s0 = 采样器
// 注意：t1 / t2 是 pColor 几何（可能 2560x1440），尺寸必须用 GetDimensions 问，不要硬编码。

Texture2D<float4>   gSrc : register(t0);
Texture2D<float4>   gRes : register(t1);
Texture2D<float4>   gMV  : register(t2);
SamplerState        gSmp : register(s0);
RWTexture2D<float4> gDst : register(u0);

// 当前使用：双线性抬升（最便宜，实测 +0.45ms）
[numthreads(8,8,1)]
void Add4K(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv  = (float2(id.xy) + 0.5) / float2(w, h);
    float4 o   = gSrc.Load(int3((int)id.x, (int)id.y, 0));
    float3 pre = gRes.SampleLevel(gSmp, uv, 0).rgb;
    float3 col = gMV.SampleLevel(gSmp, uv, 0).rgb;
    gDst[id.xy] = float4(saturate(o.rgb + (pre - col)), o.a);
}

// 备选：引导式 JBU（边缘更稳，但 8 次散读，实测 +4ms / -12fps）
// 想用就把下面的函数改名成 Add4K，并删掉上面的 Add4K。
[numthreads(8,8,1)]
void Add4K_JBU(uint3 id : SV_DispatchThreadID)
{
    uint w, h; gDst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uint lw, lh; gRes.GetDimensions(lw, lh);
    float4 o  = gSrc.Load(int3((int)id.x, (int)id.y, 0));
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float2 lc = uv * float2(lw, lh) - 0.5;
    float2 fl = floor(lc);
    float2 wt = lc - fl;
    int2   i0 = int2(fl);
    int2   imax = int2((int)lw - 1, (int)lh - 1);
    float3 esum = float3(0,0,0);
    float  wsum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j) {
        [unroll] for (int i = 0; i < 2; ++i) {
            int2 q = clamp(i0 + int2(i, j), int2(0,0), imax);
            float4 pr = gRes.Load(int3(q, 0));
            float4 co = gMV.Load(int3(q, 0));
            float3 e  = pr.rgb - co.rgb;
            float  ws = ((i == 0) ? (1.0 - wt.x) : wt.x) * ((j == 0) ? (1.0 - wt.y) : wt.y);
            float  cd = dot(abs(o.rgb - co.rgb), float3(0.3333, 0.3333, 0.3333));
            float  wc = saturate(1.0 - cd * 8.0);
            wc = wc * wc;
            esum += e * (ws * wc);
            wsum += ws * wc;
        }
    }
    float3 e4 = (wsum > 1e-4) ? (esum / wsum) : float3(0,0,0);
    gDst[id.xy] = float4(saturate(o.rgb + e4), o.a);
}
)HLSL";

static std::string g_add4kPath;
static std::string g_add4kSrc;
static INT64       g_add4kStamp = 0;
static ComPtr<ID3D12PipelineState> g_psoAdd4KPrev;

static bool ReadTextFile(const std::string& p, std::string& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 20)) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    out.resize(got);
    return got > 0;
}
static bool WriteTextFile(const std::string& p, const std::string& s) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "wb") != 0 || !f) return false;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}
static INT64 FileStamp(const std::string& p) {
    struct _stat64 st{};
    if (_stat64(p.c_str(), &st) != 0) return 0;
    return (INT64)st.st_mtime * 1000000 + (INT64)st.st_size;
}
static bool CompileAdd4K(ID3D12Device* dev, const std::string& src,
                         ID3D12PipelineState** out, std::string& errOut) {
    ComPtr<ID3DBlob> cs, er;
    if (FAILED(D3DCompile(src.c_str(), src.size(), "add4k.hlsl", nullptr, nullptr, "Add4K",
                          "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &er))) {
        errOut = er ? (char*)er->GetBufferPointer() : "(无编译日志)";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(out)))) {
        errOut = "CreateComputePipelineState 失败";
        return false;
    }
    return true;
}
static bool InitAdd4K(ID3D12Device* dev) {
    g_add4kPath = ModuleDir() + "xess_proxy.add4k.hlsl";
    INT64 st = FileStamp(g_add4kPath);
    if (!st) {
        if (WriteTextFile(g_add4kPath, kAdd4KTemplate)) Log("热编译: 已生成模板 %s", g_add4kPath.c_str());
        st = FileStamp(g_add4kPath);
    }
    if (!ReadTextFile(g_add4kPath, g_add4kSrc)) g_add4kSrc = kAdd4KTemplate;
    g_add4kStamp = st;
    std::string err;
    if (!CompileAdd4K(dev, g_add4kSrc, g_psoAdd4K.GetAddressOf(), err)) {
        Log("热编译: 初版失败，退回内置模板 —— %s", err.c_str());
        g_add4kSrc = kAdd4KTemplate;
        if (!CompileAdd4K(dev, g_add4kSrc, g_psoAdd4K.GetAddressOf(), err)) {
            Log("热编译: 模板也编译失败 —— %s", err.c_str());
            return false;
        }
    }
    Log("热编译: Add4K 就绪（%s，%zu 字节）", g_add4kPath.c_str(), g_add4kSrc.size());
    return true;
}
// 每 60 帧查一次文件是否变了
static void PollAdd4KReload() {
    if (g_add4kPath.empty()) return;
    INT64 st = FileStamp(g_add4kPath);
    if (!st || st == g_add4kStamp) return;
    g_add4kStamp = st;
    std::string src;
    if (!ReadTextFile(g_add4kPath, src) || src.empty()) return;
    std::string err;
    ComPtr<ID3D12PipelineState> np;
    if (!CompileAdd4K(g_dev.Get(), src, np.GetAddressOf(), err)) {
        Log("热编译: 失败，保留旧版本 —— %s", err.c_str());
        return;
    }
    g_psoAdd4KPrev.Reset();          // 释放上上一代（已过去两代，肯定不在飞）
    g_psoAdd4KPrev = g_psoAdd4K;     // 旧的那代延后一代释放
    g_psoAdd4K = np;
    g_add4kSrc = src;
    Log("热编译: Add4K 已更新（%zu 字节）", src.size());
}

static bool BuildResources(ID3D12Device* dev, ID3D12Resource* color,
                           ID3D12Resource* mv, ID3D12Resource* dep, ID3D12Resource* outp) {
    D3D12_RESOURCE_DESC cd = color->GetDesc();
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    g_cw = (UINT)cd.Width; g_ch = cd.Height; g_cfmt = cd.Format;
    g_outW = (UINT)od.Width; g_outH = od.Height;
    Log("pColor %ux%u fmt=%d | pOutput %ux%u fmt=%d | 槽位=%d 延迟=%d 帧",
        g_cw, g_ch, (int)g_cfmt, g_outW, g_outH, (int)od.Format, K, g_kDelay);

    for (int i = 0; i < K; ++i) {
        g_slots[i].color = MakeTex(dev, g_cw, g_ch, g_cfmt, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (mv)  g_slots[i].mv    = MakeTex(dev, (UINT)mv->GetDesc().Width, mv->GetDesc().Height, mv->GetDesc().Format, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (dep) g_slots[i].depth = MakeTex(dev, (UINT)dep->GetDesc().Width, dep->GetDesc().Height, dep->GetDesc().Format, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (!g_slots[i].color) { Log("槽位纹理失败"); return false; }
        g_slots[i].answer.assign((size_t)kNetW * kNetH * 4, 0);
        g_slots[i].feedInput.assign((size_t)kNetW * kNetH * 4, 0);
    }
    g_netS = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g_pre  = MakeTex(dev, g_cw, g_ch, g_cfmt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g_resTex = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COPY_DEST, false);
    g_resSmooth = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    if (g_resSmooth) {
        // 初始化为中性 0.5（无残差）
        Log("残差时域状态纹理已建");
    }
    if (!g_netS || !g_pre || !g_resTex || !g_resSmooth) { Log("资源创建失败"); return false; }

    D3D12_HEAP_PROPERTIES dh{};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    dh.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    dh.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    dh.CreationNodeMask = 1; dh.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES uh = dh; uh.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rh = dh; rh.Type = D3D12_HEAP_TYPE_READBACK;

    auto foot = [&](ID3D12Resource* r, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
        D3D12_RESOURCE_DESC d = r->GetDesc();
        D3D12_RESOURCE_DESC pl = d; pl.Flags = D3D12_RESOURCE_FLAG_NONE;
        UINT rows = 0; UINT64 rs = 0, tot = 0;
        dev->GetCopyableFootprints(&pl, 0, 1, 0, &fp, &rows, &rs, &tot);
        return tot;
    };
    g_bytesRes = foot(g_resTex.Get(), g_fpRes);
    g_bytesRb  = foot(g_netS.Get(), g_fpRb);

    auto mkbuf = [&](UINT64 n, bool upload) {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = n; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(upload ? &uh : &rh, D3D12_HEAP_FLAG_NONE, &bd,
            upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        return r;
    };
    g_resUp = mkbuf(g_bytesRes, true);
    g_rb    = mkbuf(g_bytesRb, false);
    {   // 诊断用回读缓冲（残差 / 时域状态 各一张 1080p）
        D3D12_RESOURCE_DESC dd = g_resSmooth->GetDesc();
        D3D12_RESOURCE_DESC pl = dd; pl.Flags = D3D12_RESOURCE_FLAG_NONE;
        UINT rws = 0; UINT64 rps = 0;
        dev->GetCopyableFootprints(&pl, 0, 1, 0, &g_dumpFp, &rws, &rps, &g_dumpBytes);
        g_dumpRb1 = mkbuf(g_dumpBytes, false);
        g_dumpRb2 = mkbuf(g_dumpBytes, false);
        Log("诊断回读缓冲: %llu 字节 行距 %u", (unsigned long long)g_dumpBytes, g_dumpFp.Footprint.RowPitch);
    }

    D3D12_DESCRIPTOR_RANGE r0{}, r1{}, r2{}, r3{}, r4{}, ru{};
    r0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r0.NumDescriptors = 1; r0.BaseShaderRegister = 0;
    r1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r1.NumDescriptors = 1; r1.BaseShaderRegister = 1;
    r2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r2.NumDescriptors = 1; r2.BaseShaderRegister = 2;
    r3.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r3.NumDescriptors = 1; r3.BaseShaderRegister = 3;
    r4.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r4.NumDescriptors = 1; r4.BaseShaderRegister = 4;
    ru.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ru.NumDescriptors = 1; ru.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE ru1{};
    ru1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ru1.NumDescriptors = 1; ru1.BaseShaderRegister = 1;
    D3D12_ROOT_PARAMETER rp[8] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &r0;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &r1;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &r2;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[3].DescriptorTable.NumDescriptorRanges = 1; rp[3].DescriptorTable.pDescriptorRanges = &r3;
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[4].DescriptorTable.NumDescriptorRanges = 1; rp[4].DescriptorTable.pDescriptorRanges = &r4;
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[5].DescriptorTable.NumDescriptorRanges = 1; rp[5].DescriptorTable.pDescriptorRanges = &ru;
    rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[6].DescriptorTable.NumDescriptorRanges = 1; rp[6].DescriptorTable.pDescriptorRanges = &ru1;
    rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[7].Constants.ShaderRegister = 0; rp[7].Constants.RegisterSpace = 0;
    rp[7].Constants.Num32BitValues = 20;
    rp[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sm{};
    sm.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sm.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sm.MaxLOD = D3D12_FLOAT32_MAX;
    sm.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 8; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &sm;
    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { Log("根签名失败"); return false; }
    if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(g_rs.GetAddressOf())))) return false;

    struct { const char* n; ID3D12PipelineState** p; } ps[] = {
        { "DownCS", &g_psoDown }, { "ResidCS", &g_psoResid }
    };
    for (auto& e : ps) {
        ComPtr<ID3DBlob> cs, er;
        if (FAILED(D3DCompile(kHLSL, strlen(kHLSL), "v9.hlsl", nullptr, nullptr, e.n, "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &er))) {
            Log("shader %s 编译失败: %s", e.n, er ? (char*)er->GetBufferPointer() : "?");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_rs.Get();
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(e.p)))) { Log("PSO %s 失败", e.n); return false; }
    }
    {   // MV 打包 PSO（用 g_rs 的表 0=SRV / 表 5=UAV）
        ComPtr<ID3DBlob> cs, er;
        if (FAILED(D3DCompile(kMvPackHLSL, strlen(kMvPackHLSL), "mvpack.hlsl", nullptr, nullptr,
                              "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &er))) {
            Log("MV 打包 shader 编译失败: %s", er ? (char*)er->GetBufferPointer() : "?");
        } else {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_rs.Get();
            pd.CS.pShaderBytecode = cs->GetBufferPointer();
            pd.CS.BytecodeLength = cs->GetBufferSize();
            if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoMvPack)))) Log("MV 打包 PSO 失败");
        }
    }
    {   // 4K 后置：10bit→8bit 转换 PSO（同样用 g_rs：表 0=SRV，表 5=UAV）
        ComPtr<ID3DBlob> cs, er;
        if (FAILED(D3DCompile(kBformFmt8, strlen(kBformFmt8), "fmt8.hlsl", nullptr, nullptr,
                              "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &er))) {
            Log("4K 后置 shader 编译失败: %s", er ? (char*)er->GetBufferPointer() : "?");
        } else {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_rs.Get();
            pd.CS.pShaderBytecode = cs->GetBufferPointer();
            pd.CS.BytecodeLength = cs->GetBufferSize();
            if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_psoFmt8)))) Log("4K 后置 PSO 失败");
        }
    }
    if (!InitAdd4K(dev)) Log("!! Add4K 不可用（resid4k=1 时会跳过 4K 叠加）");
    if (!BwInit(dev)) Log("!! 带宽测试不可用（不影响主流程）");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvCount4;   // 覆盖全部映射（+ B1 的三个 + 4K 后置的 8bit 转换目标）
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(g_heap.GetAddressOf())))) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto mkSrv = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
        if (!r) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = f; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)slot * g_inc;
        dev->CreateShaderResourceView(r, &sd, h);
    };
    auto mkUav = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
        if (!r) return;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = f; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = c; h.ptr += (SIZE_T)slot * g_inc;
        dev->CreateUnorderedAccessView(r, nullptr, &ud, h);
    };
    for (int i = 0; i < K; ++i) mkSrv(g_slots[i].color.Get(), g_cfmt, (UINT)i);
    {   // S5-2：每个槽位一份"被喂进去的 1080p 输入"
        int made = 0;
        for (int i = 0; i < K; ++i) {
            g_fedTex[i] = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_RESOURCE_STATE_COPY_DEST, false);
            if (g_fedTex[i]) { mkSrv(g_fedTex[i].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvFed + i); made++; }
        }
        Log("S5-2: fed 纹理 %d/%d 已建（SRV 槽 %d~%d）", made, K, kSrvFed, kSrvFed + K - 1);
    }
    for (int i = 0; i < K; ++i) if (g_slots[i].mv) mkSrv(g_slots[i].mv.Get(), g_slots[i].mv->GetDesc().Format, kSrvMvBase + (UINT)i);
    mkSrv(g_resTex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvRes);
    mkUav(g_netS.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvNetS);
    mkUav(g_pre.Get(), g_cfmt, kSrvPre);
    mkUav(g_resSmooth.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvSmooth);
    g_cpuResSmooth = c; g_cpuResSmooth.ptr += (SIZE_T)kSrvSmooth * g_inc;
    {   // B1：4K 叠加用的三张视图（outp 的 SRV、g_pre 的 SRV、g_pre4k 的 UAV）
        g_pre4k = MakeTex(dev, g_outW, g_outH, od.Format, D3D12_RESOURCE_STATE_COMMON, true);
        if (g_pre4k) {
            mkSrv(outp, od.Format, kSrvOutp);
            mkSrv(g_pre.Get(), g_cfmt, kSrvPreSrv);
            mkUav(g_pre4k.Get(), od.Format, kSrvPre4K);
            Log("B1: 4K 叠加资源就绪 gout=%ux%u fmt=%d（槽 %d/%d/%d）",
                g_outW, g_outH, (int)od.Format, (int)kSrvOutp, (int)kSrvPreSrv, (int)kSrvPre4K);
        } else {
            Log("B1: g_pre4k 创建失败，4K 叠加不可用（会自动退回原路径）");
        }
    }
    {   // MV 打包：1440p(8B) → 1080p RG16F(4B)，传输量 29.5MB → 8.3MB
        g_mvPacked = MakeTex(dev, kNetW, kNetH, DXGI_FORMAT_R16G16_FLOAT,
                             D3D12_RESOURCE_STATE_COMMON, true);
        if (g_mvPacked && mv) {
            mkSrv(mv, mv->GetDesc().Format, kSrvMvCur);
            mkUav(g_mvPacked.Get(), DXGI_FORMAT_R16G16_FLOAT, kSrvMvPacked);
            g_mvPackReady = true;
            Log("MV打包: 就绪（%dx%d RG16F），原始MV格式 %d", kNetW, kNetH, (int)mv->GetDesc().Format);
        } else {
            Log("MV打包: 不可用（继续用原始 MV 传输）");
        }
    }
    Log("游戏侧资源就绪");
    // 注意：B 形态的跨卡 buffer 不能在这里建 —— 此时 g_worker.dev 还没创建（WorkerInit 在后面），
    //       会空指针崩在 OpenSharedHandle 上（已踩）。改为首次需要时懒加载（见 ProxyExecute）。
    return true;
}

// 把答案编码成残差（answer − feedInput），0.5 中性
static void BuildResidual(const Slot& S, std::vector<uint8_t>& enc) {
    enc.resize((size_t)kNetW * kNetH * 4);
    if (g_bypass) { memcpy(enc.data(), S.answer.data(), enc.size()); return; }   // 直出：上传答案本身
    for (size_t i = 0; i < enc.size(); ++i) {
        int dv = (int)S.answer[i] - (int)S.feedInput[i];
        int v = dv + 128;
        if (v < 0) v = 0; if (v > 255) v = 255;
        enc[i] = (uint8_t)v;
    }
}

static void ProxyImGuiDraw(ID3D12GraphicsCommandList* cl, ID3D12Resource* rt);
// ---------- IAT hook：菜单打开时让游戏"看不到"键盘 ----------
typedef SHORT (WINAPI* PfnGaks)(int);
typedef BOOL  (WINAPI* PfnGks)(PBYTE);
static PfnGaks g_origGaks = nullptr;
static PfnGks  g_origGks = nullptr;
typedef UINT (WINAPI* PfnGrid)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static PfnGrid g_origGrid = nullptr;
static SHORT WINAPI HookGaks(int vk) { if (g_menuOpen) return 0; return g_origGaks ? g_origGaks(vk) : 0; }
static UINT WINAPI HookGrid(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    if (g_menuOpen) { if (size) *size = 0; return (UINT)-1; }
    return g_origGrid ? g_origGrid(h, cmd, data, size, hdr) : (UINT)-1;
}
static BOOL WINAPI HookGks(PBYTE st) {
    BOOL r = g_origGks ? g_origGks(st) : FALSE;
    if (g_menuOpen && st) memset(st, 0, 256);
    return r;
}
static bool PatchIATIn(HMODULE base, const char* dllName, const char* fnName, void* hook, void** orig) {
    if (!base) return false;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char* dll = (const char*)((BYTE*)base + imp->Name);
        if (_stricmp(dll, dllName) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        auto thunk = (IMAGE_THUNK_DATA*)((BYTE*)base + imp->FirstThunk);
        auto nameT = (IMAGE_THUNK_DATA*)((BYTE*)base + imp->OriginalFirstThunk);
        for (; thunk->u1.Function; ++thunk, ++nameT) {
            if (nameT->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto hint = (IMAGE_IMPORT_BY_NAME*)((BYTE*)base + nameT->u1.AddressOfData);
            if (strcmp((const char*)hint->Name, fnName) != 0) continue;
            DWORD oldProt = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) return false;
            if (orig && !*orig) *orig = (void*)thunk->u1.Function;
            thunk->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProt, &oldProt);
            return true;
        }
    }
    return false;
}
static void PatchInputIAT() {
    int nGaks = 0, nGks = 0, nGrid = 0, nMod = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                nMod++;
                HMODULE mb = (HMODULE)me.modBaseAddr;
                if (mb == g_self) continue;   // 不能 hook 自己：我们要用原始状态读鼠标/Del
                if (PatchIATIn(mb, "USER32.dll", "GetAsyncKeyState", (void*)HookGaks, (void**)&g_origGaks)) nGaks++;
                if (PatchIATIn(mb, "USER32.dll", "GetKeyboardState", (void*)HookGks, (void**)&g_origGks)) nGks++;
                if (PatchIATIn(mb, "USER32.dll", "GetRawInputData", (void*)HookGrid, (void**)&g_origGrid)) nGrid++;
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    Log("IAT hook: 模块 %d 个 | GetAsyncKeyState=%d GetKeyboardState=%d GetRawInputData=%d", nMod, nGaks, nGks, nGrid);
}

// =====================================================================================
// 【第 1 步·只观察】队列虚表 hook：截获"游戏那条 DIRECT 队列"的指针
// -------------------------------------------------------------------------------------
// 目的：将来要把 41.5MB 的跨卡拷贝挪到我们自己的 copy 队列上（游戏回到 60fps、画质不变），
//       而"排序"需要在游戏队列上 Signal 围栏 —— 必须先拿到队列对象。
//       本步【只记录 + 打日志】，不 Signal、不改任何时序 ⇒ 出问题删掉即恢复。
// 开关：xess_proxy.queuehook.txt 写 1 才安装（默认不装，可做 kill switch）
// 说明：ExecuteCommandLists 在 ID3D12CommandQueue 的虚表里位置固定（COM 接口顺序），
//       用我们自己创建的 DIRECT 队列即可拿到同一张虚表，不需要扫内存找地址。
// =====================================================================================
typedef void (*PFN_ECL)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static PFN_ECL        g_origECL      = nullptr;
static volatile LONG  g_eclCalls     = 0;
static volatile PVOID g_gameQueuePtr = nullptr;
static bool           g_eclInstalled = false;

static void XferMaybeKick(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists);   // 前向声明（定义在下方）


static void HookECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    g_origECL(q, n, lists);                     // ★ 无条件先调原函数：行为完全不变
    XferMaybeKick(q, n, lists);                  // 第 2 步：认出含我们转换的 list → Signal + 启动颜色拷贝
    LONG c = InterlockedIncrement(&g_eclCalls);
    if (!g_gameQueuePtr) {
        g_gameQueuePtr = q;
        Log("QUEUEHOOK: 首次截获队列 %p（本次提交 %u 条 list）", (void*)q, n);
    }
    if ((c % 3000) == 0)
        Log("QUEUEHOOK[帧%d] 已观察 %ld 次提交 | 游戏队列=%p", (int)g_frame, c, g_gameQueuePtr);
}

static void InstallQueueHook() {
    if (g_eclInstalled || g_origECL) return;
    std::string p = ModuleDir() + "xess_proxy.queuehook.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "r") != 0 || !f) { Log("QUEUEHOOK: 未启用（没有 %s）", p.c_str()); return; }
    char b[8] = { 0 }; size_t rd = fread(b, 1, 7, f); fclose(f);
    if (rd == 0 || b[0] != '1') { Log("QUEUEHOOK: 未启用（文件内容不是 1）"); return; }
    if (!g_worker.q) { Log("QUEUEHOOK: worker 队列还没建好，放弃"); return; }

    void** vt = *(void***)g_worker.q.Get();
    if (!vt) { Log("QUEUEHOOK: 拿不到虚表，放弃"); return; }
    const int kIdx = 10;   // IUnknown(3) + ID3D12Object(4) + ID3D12DeviceChild(1) + Update/CopyTileMappings(2) = 10
    if (!vt[kIdx]) { Log("QUEUEHOOK: 槽位[%d] 为空，放弃", kIdx); return; }

    HMODULE hm = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)vt[kIdx], &hm) || !hm) { Log("QUEUEHOOK: 定位模块失败，放弃"); return; }
    char mod[MAX_PATH] = { 0 }; GetModuleFileNameA(hm, mod, MAX_PATH);
    Log("QUEUEHOOK: 虚表=%p 槽位[%d]=%p 所属模块=%s", (void*)vt, kIdx, vt[kIdx], mod);

    DWORD old = 0;
    if (!VirtualProtect(vt, sizeof(void*) * (kIdx + 1), PAGE_READWRITE, &old)) {
        Log("QUEUEHOOK: 改内存保护失败，放弃"); return;
    }
    g_origECL = (PFN_ECL)vt[kIdx];
    vt[kIdx] = (void*)&HookECL;
    VirtualProtect(vt, sizeof(void*) * (kIdx + 1), old, &old);
    g_eclInstalled = true;
    Log("QUEUEHOOK: 已安装（第 1 步：只观察，不改时序）");
}

// =====================================================================================
// 【第 2 步】把 33.2MB 的 4K 颜色拷贝挪到我们自己的 COPY 队列
// -------------------------------------------------------------------------------------
// 要解决的两个问题：
//  ① 竞态：CPU 录帧 N 时 GPU 可能还在写帧 N-1 的缓冲 → 用 HookECL 在游戏提交后 Signal 内部围栏，
//     我们的拷贝队列 Wait 过它才拷（g_xqFe）
//  ② B 读太早：颜色比 MV 晚到 → 用【跨卡共享围栏】g_xqFe2 通知 B，B 解码前 Wait（GPU 侧等待）
// 开关：xess_proxy.queuehook.txt = 1（与第 1 步共用；写 0 即完全回到直拷路径）
// =====================================================================================

static bool XferEnsure() {
    if (g_xqQ) return true;
    if (!g_dev) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 2); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_xqQ)))) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 4); return false; }
    for (int xi = 0; xi < 3; ++xi) {
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_xqAl[xi])))) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 6); return false; }
        if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_xqAl[xi].Get(), nullptr, IID_PPV_ARGS(&g_xqCl[xi])))) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 7); return false; }
        g_xqCl[xi]->Close();
    }
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_xqFe)))) return false;   // 内部记号围栏（同卡即可）
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&g_xqFe2)))) { static int de=0; if(de++<3) Log("XFERDBG3: XferEnsure 第 %d 步失败", 10); return false; }
    g_xferOwn = true;
    if (FAILED(g_dev->CreateSharedHandle(g_xqFe2.Get(), nullptr, GENERIC_ALL, nullptr, &g_xqShare))) return false;   // ★ 跨卡共享句柄（B 端用它 OpenSharedHandle）
    Log("XFER: 独立 copy 队列 + 跨卡共享围栏就绪（33.2MB 颜色改由我们的队列拷）");
    return true;
}

// 在我们的 COPY 队列上：等 vSig 过了 → 拷 g_outp8[parity] → 槽 slot 的颜色平面 → Signal 共享围栏
static bool XferKickColor(ID3D12Resource* src4k, UINT slot, UINT64 vSig) {
    if (!XferEnsure() || !src4k || !g_upBuf) { static int d0=0; if(d0++<3) Log("XFERDBG2: 早退 ensure/src/buf (xferOwn=%d src=%p buf=%p)", (int)g_xferOwn, (void*)src4k, (void*)g_upBuf.Get()); return false; }
    int ai = g_xqAi; g_xqAi = (g_xqAi + 1) % 3;
    // 该组上次的拷贝还没跑完 → 跳过并计数（这就是"半帧不更新"的来源，必须能看见）
    if (g_xqFvUsed[ai] && g_xqFe2 && g_xqFe2->GetCompletedValue() < g_xqFvUsed[ai])
    { InterlockedIncrement(&g_xferSkip); return false; }
    if (FAILED(g_xqAl[ai]->Reset()) || FAILED(g_xqCl[ai]->Reset(g_xqAl[ai].Get(), nullptr)))
    { InterlockedIncrement(&g_xferSkip); return false; }
    ID3D12GraphicsCommandList* cl = g_xqCl[ai].Get();
    D3D12_RESOURCE_BARRIER bb{};
    bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb.Transition.pResource = src4k;
    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &bb);
    bb.Transition.pResource = g_upBuf.Get();
    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->ResourceBarrier(1, &bb);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_upBuf.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = (UINT64)slot * g_upSlotBytes + g_upOffC;
    dst.PlacedFootprint.Footprint.Format = g_upFmtC;
    dst.PlacedFootprint.Footprint.Width = g_upW;
    dst.PlacedFootprint.Footprint.Height = g_upH;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g_upPitchC;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = src4k;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    bb.Transition.pResource = src4k;
    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl->ResourceBarrier(1, &bb);
    bb.Transition.pResource = g_upBuf.Get();
    bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl->ResourceBarrier(1, &bb);
    if (FAILED(cl->Close())) { InterlockedIncrement(&g_xferSkip); return false; }
    g_xqQ->Wait(g_xqFe.Get(), vSig);
    ID3D12CommandList* lm[] = { cl };
    g_xqQ->ExecuteCommandLists(1, lm);
    UINT64 v2 = ++g_xqFv2;
    g_xqQ->Signal(g_xqFe2.Get(), v2);
    g_xqFvUsed[ai] = v2;
    g_slotReadyFv[slot] = v2;
    g_slotTag[slot].jx = 0.0f; g_slotTag[slot].jy = 0.0f;
    g_slotTag[slot].frame = (int)(g_xferPendFrameTag);   // ★ 颜色拷贝成功后才发布面单
    g_upLastSlot = (int)slot;
    LONG n = InterlockedIncrement(&g_xferKick);
    if ((n % 300) == 1)
        Log("XFER[帧%d] 已拷 %ld 次 | 跳过 %ld | 槽 %u（该槽记录帧 %d）| 就绪围栏=%llu", (int)g_frame, n, g_xferSkip, slot, (int)g_slotTag[slot].frame, (unsigned long long)v2);
    return true;
}
// 在钩子里判断：这次提交的 list 里是否含"我们录过转换的那条" → 是则 Signal + 启动我们队列上的颜色拷贝
// 改为"每次提交都打一个记号"：不再依赖 list 指针身份（实测游戏提交的 list 与我们录的不是同一对象）
//   记号只会往后排 ⇒ 只要等"最新一次提交的记号"，就能保证此前已入队的换算全都执行完
static void XferMaybeKick(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    static int dq = 0; if (dq++ < 3) Log("XFERDBG3: XferMaybeKick 被调用 | xqFe=%p xferOwn=%d", (void*)g_xqFe.Get(), (int)g_xferOwn);
    (void)n; (void)lists;
    if (!g_xqFe) return;
    UINT64 v = (UINT64)InterlockedIncrement64((volatile LONG64*)&g_xqFv);
    q->Signal(g_xqFe.Get(), v);
    g_lastSigV = v;
}
// 游戏内共享可行性探测：游戏设备 ↔ worker 设备
// ===== S1：共享纹理基础（只创建，不使用；行为零变化）=====
static volatile bool g_useShared = false;         // 总开关：true 才走共享纹理路径
static bool g_shareOk = false;
static ComPtr<ID3D12Resource> g_shareIn[K],  g_shareInW[K];    // 1080p 输入（游戏侧/worker侧视图）
static ComPtr<ID3D12Resource> g_shareAns[K], g_shareAnsW[K];   // 1080p 答案

// ===== R2：跨卡共享 buffer（颜色进 / 答案出）=====
          // 头部 512B，载荷从 +512 开始

static bool MakeXBuf(UINT64 bytes, const char* tag,
                     ComPtr<ID3D12Heap>& hA, ComPtr<ID3D12Resource>& bA,
                     ComPtr<ID3D12Heap>& hB, ComPtr<ID3D12Resource>& bB) {
    const UINT64 ALIGN = 65536;
    UINT64 sz = ((bytes + ALIGN - 1) / ALIGN) * ALIGN;
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = sz;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Properties.CreationNodeMask = 1; hd.Properties.VisibleNodeMask = 1;
    hd.Alignment = ALIGN;
    hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
    HRESULT h = g_dev->CreateHeap(&hd, IID_PPV_ARGS(hA.GetAddressOf()));
    if (FAILED(h)) { Log("R2 %s: 建堆 0x%08X", tag, (unsigned)h); return false; }
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = sz; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    h = g_dev->CreatePlacedResource(hA.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(bA.GetAddressOf()));
    if (FAILED(h)) { Log("R2 %s: 游戏侧 placed 0x%08X", tag, (unsigned)h); return false; }
    HANDLE sh = nullptr;
    h = g_dev->CreateSharedHandle(hA.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
    if (FAILED(h)) { Log("R2 %s: 导出 0x%08X", tag, (unsigned)h); return false; }
    h = g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(hB.GetAddressOf()));
    CloseHandle(sh);
    if (FAILED(h)) { Log("R2 %s: worker 打开 0x%08X", tag, (unsigned)h); return false; }
    h = g_worker.dev->CreatePlacedResource(hB.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(bB.GetAddressOf()));
    if (FAILED(h)) { Log("R2 %s: worker 侧 placed 0x%08X", tag, (unsigned)h); return false; }
    Log("R2 %s: OK (%llu 字节)", tag, (unsigned long long)sz);
    return true;
}

static void XAdapterInit() {
    if (!g_dev || !g_worker.dev) return;
    LUID la = g_dev->GetAdapterLuid();
    LUID lb = g_worker.dev->GetAdapterLuid();
    const bool sameCard = (la.HighPart == lb.HighPart && la.LowPart == lb.LowPart);
    Log("R2: 游戏设备 LUID=%08X:%08X | worker LUID=%08X:%08X | 同卡=%d",
        (unsigned)la.HighPart, (unsigned)la.LowPart, (unsigned)lb.HighPart, (unsigned)lb.LowPart, (int)sameCard);
    const UINT64 PAY = 1920ull * 1080ull * 4ull;
    bool a = MakeXBuf(PAY, "颜色进", g_xHeapIn,  g_xBufIn,  g_xHeapInW,  g_xBufInW);
    bool b = MakeXBuf(XHDR + PAY * (UINT64)K, "答案出(K段)", g_xHeapOut, g_xBufOut, g_xHeapOutW, g_xBufOutW);
    HRESULT h1 = g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(g_xFence.GetAddressOf()));
    HRESULT h2 = E_FAIL, h3 = E_FAIL;
    if (SUCCEEDED(h1)) {
        HANDLE sh = nullptr;
        h2 = g_dev->CreateSharedHandle(g_xFence.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
        if (SUCCEEDED(h2)) { h3 = g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(g_xFenceW.GetAddressOf())); CloseHandle(sh); }
    }
    g_xOk = a && b;
    Log("R2 结果: buffer=%d 共享fence 建=0x%08X 导出=0x%08X worker打开=0x%08X", (int)g_xOk, (unsigned)h1, (unsigned)h2, (unsigned)h3);
    // S5-4：本地中转 buffer（普通 buffer 才能建 SRV）
    if (g_xOk && g_heap) {
        D3D12_HEAP_PROPERTIES lh{};
        lh.Type = D3D12_HEAP_TYPE_DEFAULT;
        lh.CreationNodeMask = 1; lh.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC ld{};
        ld.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ld.Width = ANS_BYTES; ld.Height = 1; ld.DepthOrArraySize = 1; ld.MipLevels = 1;
        ld.Format = DXGI_FORMAT_UNKNOWN; ld.SampleDesc.Count = 1;
        ld.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hl = g_dev->CreateCommittedResource(&lh, D3D12_HEAP_FLAG_NONE, &ld,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(g_ansLocal.GetAddressOf()));
        if (SUCCEEDED(hl)) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.FirstElement = 0;
            sd.Buffer.NumElements = (UINT)(ANS_BYTES / 4);
            sd.Buffer.StructureByteStride = 0;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            D3D12_CPU_DESCRIPTOR_HANDLE dh = g_heap->GetCPUDescriptorHandleForHeapStart();
            dh.ptr += (SIZE_T)kSrvAns * g_inc;
            g_dev->CreateShaderResourceView(g_ansLocal.Get(), &sd, dh);
            Log("S5-4: 本地中转 buffer %llu 字节 + SRV（槽 %d）已建", (unsigned long long)ANS_BYTES, (int)kSrvAns);
        } else {
            Log("S5-4: 本地 buffer 建失败 0x%08X", (unsigned)hl);
        }
    }
    if (g_xOk) {
        D3D12_HEAP_PROPERTIES uh{};
        uh.Type = D3D12_HEAP_TYPE_UPLOAD;
        uh.CreationNodeMask = 1; uh.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC ud{};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = XHDR; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.SampleDesc.Count = 1;
        ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(g_dev->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(g_xHdrUp.GetAddressOf())))) {
            void* pm = nullptr;
            D3D12_RANGE zr{ 0, 0 };
            if (SUCCEEDED(g_xHdrUp->Map(0, &zr, &pm)) && pm) {
                struct XH { unsigned int magic, ver, w, h, fmt, pitch, frame, pad; };
                XH* hh = (XH*)pm;
                hh->magic = 0x31524E58u; hh->ver = 1; hh->w = 1920; hh->h = 1080;
                hh->fmt = 28; hh->pitch = 1920 * 4; hh->frame = 0; hh->pad = 0;
                Log("R3: 头部已写 magic=XNR1 1920x1080 pitch=7680 头部=%d B", (int)XHDR);
            }
        }
    }
}

static bool ShareInit() {
    if (!g_dev || !g_worker.dev) { Log("S1: 设备未就绪，跳过"); return false; }
    auto mkShared = [&](ComPtr<ID3D12Resource>& own, ComPtr<ID3D12Resource>& peer, const char* tag) -> bool {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 1920; td.Height = 1080; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT h = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(own.GetAddressOf()));
        if (FAILED(h)) { Log("S1: %s 建失败 0x%08X", tag, (unsigned)h); return false; }
        HANDLE sh = nullptr;
        h = g_dev->CreateSharedHandle(own.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
        if (FAILED(h)) { Log("S1: %s 导出失败 0x%08X", tag, (unsigned)h); return false; }
        h = g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(peer.GetAddressOf()));
        CloseHandle(sh);
        if (FAILED(h)) { Log("S1: %s 对端打开失败 0x%08X", tag, (unsigned)h); return false; }
        return true;
    };
    if (!g_useShared) { Log("S1: 共享纹理跳过（跨卡下不可用）"); return false; }
    int okIn = 0, okAns = 0;
    for (int i = 0; i < K; ++i) {
        if (mkShared(g_shareIn[i],  g_shareInW[i],  "输入纹理")) okIn++;
        if (mkShared(g_shareAns[i], g_shareAnsW[i], "答案纹理")) okAns++;
    }
    // 共享 fence 探测（S6 会用到）
    HRESULT hf = E_FAIL, hf2 = E_FAIL, hf3 = E_FAIL;
    {
        ComPtr<ID3D12Fence> f1, f2;
        hf = g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(f1.GetAddressOf()));
        if (SUCCEEDED(hf)) {
            HANDLE sh = nullptr;
            hf2 = g_dev->CreateSharedHandle(f1.Get(), nullptr, GENERIC_ALL, nullptr, &sh);
            if (SUCCEEDED(hf2)) { hf3 = g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(f2.GetAddressOf())); CloseHandle(sh); }
        }
    }
    g_shareOk = (okIn == K && okAns == K);
    Log("S1: 共享纹理 输入 %d/%d 答案 %d/%d | 共享fence 建=0x%08X 导出=0x%08X 打开=0x%08X | g_useShared=%d",
        okIn, K, okAns, K, (unsigned)hf, (unsigned)hf2, (unsigned)hf3, (int)g_useShared);
    return g_shareOk;
}
static void TestSharedInGame() {
    if (!g_dev || !g_worker.dev) return;
    auto luidOf = [](ID3D12Device* d) -> LUID {
        LUID l{};
        Microsoft::WRL::ComPtr<IDXGIDevice> gd;
        if (d && SUCCEEDED(d->QueryInterface(__uuidof(IDXGIDevice), (void**)gd.GetAddressOf()))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
            if (SUCCEEDED(gd->GetAdapter(ad.GetAddressOf()))) { DXGI_ADAPTER_DESC dd{}; ad->GetDesc(&dd); l = dd.AdapterLuid; }
        }
        return l;
    };
    LUID la = luidOf(g_dev.Get());
    LUID lb = luidOf(g_worker.dev.Get());
    Log("共享探测: 游戏设备 LUID=%08X:%08X | worker LUID=%08X:%08X | 同卡=%d",
        (unsigned)la.HighPart, (unsigned)la.LowPart, (unsigned)lb.HighPart, (unsigned)lb.LowPart,
        (int)(la.HighPart == lb.HighPart && la.LowPart == lb.LowPart));

    // ① buffer：普通 SHARED
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 4u << 20; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        HRESULT h1 = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &bd,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        HANDLE sh = nullptr;
        HRESULT h2 = SUCCEEDED(h1) ? g_dev->CreateSharedHandle(r.Get(), nullptr, GENERIC_ALL, nullptr, &sh) : h1;
        ComPtr<ID3D12Resource> r2;
        HRESULT h3 = SUCCEEDED(h2) ? g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(r2.GetAddressOf())) : h2;
        if (sh) CloseHandle(sh);
        Log("共享探测① buffer: 建=0x%08X 导出=0x%08X 对端打开=0x%08X", (unsigned)h1, (unsigned)h2, (unsigned)h3);
    }
    // ② 纹理：1920x1080 RGBA8（就是我们要传的颜色尺寸）
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 1920; td.Height = 1080; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        HRESULT h1 = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(r.GetAddressOf()));
        HANDLE sh = nullptr;
        HRESULT h2 = SUCCEEDED(h1) ? g_dev->CreateSharedHandle(r.Get(), nullptr, GENERIC_ALL, nullptr, &sh) : h1;
        ComPtr<ID3D12Resource> r2;
        HRESULT h3 = SUCCEEDED(h2) ? g_worker.dev->OpenSharedHandle(sh, IID_PPV_ARGS(r2.GetAddressOf())) : h2;
        if (sh) CloseHandle(sh);
        Log("共享探测② 纹理1920x1080: 建=0x%08X 导出=0x%08X 对端打开=0x%08X", (unsigned)h1, (unsigned)h2, (unsigned)h3);
    }
}
static bool ProxyImGuiInit();
// ---------------- S1 镜像叠层（定义在文件末尾；这里只前置声明） ----------------
static void MirrorTick(ID3D12GraphicsCommandList* cl, ID3D12Resource* outp);

// B1 开关：xess_proxy.resid4k.txt 里写 1/0（每 300 帧读一次）
static void PollResid4K() {
    static int s_last = -1;
    std::string p = ModuleDir() + "xess_proxy.resid4k.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "r") != 0 || !f) return;
    char b[8] = { 0 };
    size_t n = fread(b, 1, 7, f);
    fclose(f);
    if (!n) return;
    int v = (b[0] == '1') ? 1 : 0;
    if (v == s_last) return;
    s_last = v;
    if ((v != 0) != g_resid4k) {
        g_resid4k = (v != 0);
        Log("B1: 残差落点 -> %s（帧 %d）", g_resid4k ? "4K(上采样后)" : "pColor(上采样前)", g_frame);
    }
}

extern "C" int ProxyExecute(void* ctx, void* cmdList, const void* params) {
    using Fn = int(*)(void*, void*, const void*);
    Fn real = (Fn)g_xessTab[30];
    if (!params || !cmdList) return real ? real(ctx, cmdList, params) : -1;
    if (g_disabled) {
        BformPresentShow(false);
        return real ? real(ctx, cmdList, params) : -1;
    }
    // 注：NR 关闭时【不再】在这里早退 —— 见下面"预建网络"的说明。

    // ★ 启动时自动载入用户存档（xess_proxy.saved.ini）
    //   以前只有菜单里的「恢复上次保存」会读它 → 每次启动都退回代码默认值，
    //   用户调好的参数（例如拖尾调优的 gateK=15 / mvConf=2.0）第二天就白调了。
    //   放在"记录起始参数"之前，这样「撤销改动」回到的是【载入后的值】，符合直觉。
    {
        static bool autoLoaded = false;
        if (!autoLoaded) {
            autoLoaded = true;
            Params ap = CaptureParams();
            if (LoadParamsFromFile(ap)) {
                ApplyParams(ap);
                Log("启动: 已自动载入上次保存的设置（xess_proxy.saved.ini）→ gateK=%.1f mvConf=%.1f gain=%.2f limit=%.2f",
                    (float)g_gateK, (float)g_mvConf, (float)g_gain, (float)g_limit);
            } else {
                Log("启动: 没有用户存档（xess_proxy.saved.ini），使用代码默认值");
            }
        }
    }

    // ★ NR 初始状态：默认【关闭】
    //   理由：进游戏先进原生画面，想用再按 F10 开；避免一进游戏就被 NR 接管出画。
    //   想要"默认就开"：在游戏目录放 xess_proxy.nr.txt，内容写 1。
    {
        static bool nrInitDone = false;
        if (!nrInitDone) {
            nrInitDone = true;
            // ★ 不再无条件写 false：那样会冲掉玩家在代理加载前就按下的 F10。
            //   只在存在 xess_proxy.nr.txt 时按它的值覆盖（0=默认关 / 1=默认开）。
            std::string np = ModuleDir() + "xess_proxy.nr.txt";
            FILE* nf = nullptr;
            if (fopen_s(&nf, np.c_str(), "r") == 0 && nf) {
                char nb[32] = { 0 };
                size_t gn = fread(nb, 1, sizeof(nb) - 1, nf);
                nb[gn] = 0;
                fclose(nf);
                if (nb[0] == '0') { g_nrOn = false; }
                else if (nb[0] == '1') { g_nrOn = true; }
            }
            Log("启动: NR 初始状态 = %s（F10 切换；xess_proxy.nr.txt 写 0/1 可固定默认）", g_nrOn ? "开" : "关");
        }
    }

    // 记一次“本次启动的起始参数”，供菜单“撤销改动”回退
    if (!g_sessionStartValid) {
        g_sessionStart = CaptureParams();
        g_sessionStartValid = true;
        Log("菜单: 已记录本次启动的初始参数（撤销用）");
    }

    const unsigned __int64* q = (const unsigned __int64*)params;
    ID3D12Resource* color = (ID3D12Resource*)q[0];
    ID3D12Resource* mvRes = (ID3D12Resource*)q[1];
    ID3D12Resource* depRes = (ID3D12Resource*)q[2];
    ID3D12Resource* outp  = (ID3D12Resource*)q[5];
    g_outpRes = outp;                     // ★ 4K 后置模式要用（取 XeSS 上采样之后的画面）

    // ---------- 诊断（与形态无关，前 5 帧一次）：jitter + MV 契约 + 游戏原始 MV 尺寸 ----------
    //   偏移按 intel/xess 的 xess_d3d12_execute_params_t：
    //   +0 pColor +8 pVelocity +16 pDepth +24 pExposure +32 pResponsiveMask +40 pOutput
    //   +48 jitterX +52 jitterY +56 exposureScale +60 resetHistory +64 inputWidth +68 inputHeight
    //   注：XeSS 没有 MV 缩放参数；RE 引擎 MV 是归一化(NDC)单位，像素 = 值 × (W/2)
    if (color) {
        static int mvdbg = 0;
        if (mvdbg < 5) {
            const unsigned char* pb = (const unsigned char*)params;
            float jx0 = 0, jy0 = 0, ev0 = 0; unsigned rst = 0, iw = 0, ih = 0;
            memcpy(&jx0, pb + 48, 4); memcpy(&jy0, pb + 52, 4);
            memcpy(&ev0, pb + 56, 4); memcpy(&rst, pb + 60, 4);
            memcpy(&iw, pb + 64, 4);  memcpy(&ih, pb + 68, 4);
            D3D12_RESOURCE_DESC cd = color->GetDesc();
            D3D12_RESOURCE_DESC md{}; int mf = -1; unsigned mw = 0, mh = 0;
            if (mvRes) { md = mvRes->GetDesc(); mw = (unsigned)md.Width; mh = (unsigned)md.Height; mf = (int)md.Format; }
            Log("DIAG-MV: jitter=(%.6f,%.6f) |len|=%.6f  契约[-0.5,0.5]", jx0, jy0, sqrtf(jx0*jx0 + jy0*jy0));
            Log("DIAG-MV: 契约 input=%ux%u expo=%.4f reset=%u | XeSS 无 MV 缩放参数（单位由引擎定）", iw, ih, ev0, rst);
            Log("DIAG-MV: 游戏颜色 %ux%u fmt=%d | 游戏MV %ux%u fmt=%d | 打包 %dx%d | 空间比例=%.4f | 归一化像素=值×%.1f",
                (unsigned)cd.Width, (unsigned)cd.Height, (int)cd.Format, mw, mh, mf, kNetW, kNetH,
                (mw && kNetW) ? ((float)kNetW / (float)mw) : 0.0f, iw ? iw / 2.0f : 0.0f);
            ++mvdbg;
        }
    }
    bool did4kUpscale = false;            // 4K 后置：本帧是否已在"敲铃前"提前做过 XeSS 上采样
    ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmdList;
    float jx = 0, jy = 0;
    memcpy(&jx, (const char*)params + 48, 4);
    memcpy(&jy, (const char*)params + 52, 4);

    static bool inited = false, gaveUp = false;
    if (gaveUp) return real ? real(ctx, cmdList, params) : -1;
    if (!inited) {
        if (!color || !outp) return real ? real(ctx, cmdList, params) : -1;
        if (FAILED(color->GetDevice(__uuidof(ID3D12Device), (void**)g_dev.GetAddressOf())) || !g_dev) {
            gaveUp = true; return real ? real(ctx, cmdList, params) : -1;
        }
        if (!BuildResources(g_dev.Get(), color, mvRes, depRes, outp) || !WorkerInit()) {
            gaveUp = true; Log("!! 初始化失败，永久放弃"); return real ? real(ctx, cmdList, params) : -1;
        }
        ProxyImGuiInit();
        TestSharedInGame();
        ShareInit();
        XAdapterInit();
        g_worker.th = std::thread(WorkerMain);
        inited = true;
        Log("初始化完成（方案 B：槽位%d / 喂帧滞后%d / 呈现延迟%d / 余量%d 帧）", K, kFeedLag, (int)g_kDelay, (int)g_kDelay - kFeedLag);
    }

    // ★ 预建网络（解决"第一次按 F10 要等约 0.9 秒"）：
    //   初始化上面已经跑完（含 WorkerCreateSession → 运行时开始建网络），即使 NR 关着也先建好。
    //   实测证据：默认关时，首次开 NR 的日志出现 "建网络等待结束（等了 9×100ms）" + "#1 911.2 ms"，
    //   期间没有任何答案 ⇒ 不合成 ⇒ 画面不变，用户以为没生效就会再按一次（反而关掉）。
    //   现在网络在加载阶段就建，进游戏后按 F10 即刻出画面。
    if (!g_nrOn) {
        // 默认就是这条路：网络已预建，但用户还没按 F10 ⇒ 本帧走原生，不合成、不留覆盖层
        BformPresentShow(false);
        return real ? real(ctx, cmdList, params) : -1;
    }

    auto tA = Clock::now();
    if ((g_frame % 300) == 0) PollResid4K();          // B1 开关（文件）
    if ((g_frame % 60) == 0) {
        if (!g_gameHwnd) ResolveGameHwnd();           // ★ 不依赖菜单：主动定位游戏主窗口（B 出画要用）
        BformPollFile();                              // B 形态开关（文件，每 60 帧读一次）
        Diag5PollFile();                              // 探针 5 模式（文件，每 60 帧读一次）
        DumpPollFile();                                // 抓帧导出（文件开关）
        TaaPollFile();                                // TAA 的 alpha/clampK（文件，每 60 帧读一次）
        // ★ 第 1 步·只观察：尝试截获游戏队列指针（需 xess_proxy.queuehook.txt = 1）
        {
            static bool qhTried = false;
            if (!qhTried && g_worker.q) { qhTried = true; InstallQueueHook(); }
        }
        // B 形态懒初始化：必须等 g_worker.dev 建好之后再建跨卡 buffer
        if (g_bformOn && !g_bformReady && color && mvRes && depRes)
            BformInit(g_dev.Get(), color, mvRes, depRes);
        // ★ 看门狗：stage7 若让 worker 卡死，游戏会一直挂着。超过 3 秒没心跳就自动降回 stage 6
        //   （降级后 A 会继续出画、只是没有 NR；不会让游戏永久卡死）
        if (g_bformStage >= 7 && g_workerBeat) {
            ULONGLONG nowTick = GetTickCount64();
            if (nowTick - g_workerBeat > 3000) {
                Log("!! 看门狗：worker 超过 3 秒没心跳（stage7 疑似卡死）→ 自动降回 stage 6");
                g_bformStage = 6;
                g_ppHavePrev = false;
            }
        }
    }
    if ((g_frame % 60) == 0) PollAdd4KReload();       // Add4K 热重编译（改 hlsl 保存即生效）
    // 覆盖层可见性跟随"B 是否真的出画"：只有 B 接管时才显示/保持覆盖层；
    // 否则（stage<6、或覆盖层还没建起来、或 NR 关了）一律卸载，避免盖住 A 的真实画面
    BformPresentShow(g_bformOn && g_bformStage >= 6 && g_bpReady);
    ID3D12DescriptorHeap* heaps[] = { g_heap.Get() };
    cl->SetDescriptorHeaps(1, heaps);

    const int N = g_frame++;
    Slot& cur = g_slots[N % K];
    const int presentFrame = N - g_kDelay;

    // ---------- ① [CPU] 取答案（阻塞）----------
    //   ★ 只有【B 真的在出画】时才让 A 偷懒（省掉这 30ms 等待）；B 没接管（没按 Del、
    //     覆盖层没建起来）时 A 必须照常合成，否则会"两头都没有 NR"的静默失败（10-28 踩过）
    const bool bPresenting = (g_bformOn && g_bformStage >= 6 && g_bpReady);
    if (presentFrame >= 0 && !bPresenting) {
        Slot& P = g_slots[presentFrame % K];
        auto w0 = Clock::now();
        {
            std::unique_lock<std::mutex> lk(g_worker.m);
            // 身份化等待：必须等到【属于 presentFrame 这一帧内容】的答案，而不是"随便最新的一份"。
            // 注意：这里【绝对不能 return】—— 它下面还有"喂下一帧给 worker"，早退会饿死 worker，
            // 于是永远等不到答案（踩过一次：NR 全程失效）。失败只是本帧不合成。
            bool gotId = g_worker.cvOut.wait_for(lk, std::chrono::milliseconds(30),
                        [&] { return !g_worker.ok || g_worker.ansFrame[presentFrame % K] == presentFrame; });
            if (gotId && g_worker.ansFrame[presentFrame % K] == presentFrame) {
                memcpy(P.answer.data(), g_worker.ansBuf[presentFrame % K].data(), P.answer.size());
                P.hasAnswer = true;
            } else {
                P.hasAnswer = false;
                static int missCnt = 0;
                if ((missCnt++ % 60) == 0)
                    Log("!! 等不到第 %d 帧的答案（槽%d 现有=%d ok=%d）本帧不合成，累计 %d 次",
                        presentFrame, presentFrame % K, g_worker.ansFrame[presentFrame % K],
                        (int)g_worker.ok, missCnt);
            }
            {

                // 静止检测：和上一次呈现的那一帧比较（抽样），决定时域混合强度
                {
                    Slot& Q = g_slots[(presentFrame - 1 + K * 4) % K];
                    if (Q.hasAnswer && Q.feedInput.size() == P.feedInput.size()) {
                        size_t cnt = 0, changed = 0;
                        for (size_t i = 0; i < P.feedInput.size(); i += 64) {
                            int d = (int)P.feedInput[i] - (int)Q.feedInput[i];
                            if (d < 0) d = -d;
                            if (d > 16) changed++;   // 噪声不会超过 16，真运动才会
                            cnt++;
                        }
                        float motion = cnt ? (float)changed / (float)cnt : 0.0f;
                        // 连续过渡：运动量在下界→用静止 alpha；上界→1.0（不用历史）；中间平滑插值
        // 旧写法是二元硬切换 (motion < 0.02 ? static : 1.0)，
        // 运动量在 0.02 附近抖动时 alpha 会在 0.02 ↔ 1.0 之间反复跳 → 画面闪烁
        float denom = g_motionHi - g_motionLo;
        if (denom < 1e-4f) denom = 1e-4f;
        float mt = (motion - g_motionLo) / denom;
        mt = (mt < 0.0f) ? 0.0f : ((mt > 1.0f) ? 1.0f : mt);
        mt = mt * mt * (3.0f - 2.0f * mt);   // smoothstep
        // 运动时不再到 1.0，而是到 g_motionAlpha；实际历史量再由 shader 的 conf/gate 决定
        g_alpha = g_temporalStatic + (g_motionAlpha - g_temporalStatic) * mt;
                        if (presentFrame < 8) g_alpha = 1.0f;   // 前几帧让状态纹理自愈（初始内容是垃圾）
                        static int logN = 0;
                        if ((logN++ % 150) == 0) Log("alpha: 运动量 %.4f -> alpha %.2f (区间 %.2f~%.2f)", motion, g_alpha, g_motionLo, g_motionHi);
                    }
                }
            }
        }
        double wms = std::chrono::duration<double, std::milli>(Clock::now() - w0).count();
        g_sumWait += wms; g_nWait++;
        if (g_nWait % 150 == 0)
            Log("stats: 帧%d 等待均值 %.1fms 槽位[%d]答案=%d", N, g_sumWait / g_nWait, presentFrame % K, (int)P.hasAnswer);
    }

    // ---------- ② [CPU] Map 上一帧的回读 → 喂 worker（slot[N-1]）----------
    if (N >= kFeedLag) {
        Slot& F = g_slots[((N - kFeedLag) % K + K) % K];   // 回读内容属于第 N-2 帧的槽位（结构固定）
        D3D12_RANGE rr{ 0, (SIZE_T)g_bytesRb };
        void* m = nullptr;
        if (SUCCEEDED(g_rb->Map(0, &rr, &m)) && m) {
            UINT pitch = g_fpRb.Footprint.RowPitch;
            for (UINT y = 0; y < kNetH; ++y)
                memcpy(&F.feedInput[(size_t)y * kNetW * 4], (unsigned char*)m + (size_t)y * pitch, (size_t)kNetW * 4);
            D3D12_RANGE nw{ 0, 0 };
            g_rb->Unmap(0, &nw);

            std::lock_guard<std::mutex> lk(g_worker.m);
            std::vector<uint8_t> tmp(kNetW * kNetH * 4);
            for (UINT y = 0; y < kNetH; ++y)
                memcpy(&tmp[(size_t)y * kNetW * 4], &F.feedInput[(size_t)y * kNetW * 4], (size_t)kNetW * 4);
            g_worker.inBuf.swap(tmp);
            g_worker.inIndex = ((N - kFeedLag) % K + K) % K;
            g_worker.inFrame = N - kFeedLag;
            g_worker.hasIn = true;
            g_worker.cv.notify_one();
        }
    }

    // ---------- ③ [GPU] 拷本帧原始 pColor / MV / depth 进槽位 ----------
    if (color) {
        Barrier(cl, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, cur.color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(cur.color.Get(), color);
        Barrier(cl, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    if (mvRes && cur.mv) {
        Barrier(cl, mvRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyResource(cur.mv.Get(), mvRes);
        Barrier(cl, mvRes, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (depRes && cur.depth) {
        Barrier(cl, depRes, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyResource(cur.depth.Get(), depRes);
        Barrier(cl, depRes, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_READ);
    }
    cur.jx = jx; cur.jy = jy; cur.frame = N; cur.captured = true;

    // ---------- ③.5 [GPU] 【B 形态·一趟方案】把本帧原始 pColor/MV/深度 单向传给 B ----------
    // 一趟方案：A 只做"送原料"，NR / 残差合成 / 上采样 / 出画 全在 B 上做。
    // 好处：少一整次跨卡往返（B→A 的答案回传整条通道可以去掉）、延迟更低、同步退化成单向流。
    if (g_bformOn && g_bformReady && color && mvRes && depRes) {
        if (g_mvPackReady && g_psoMvPack) {
            // 先把 MV 打包成 1080p RG16F（29.5MB → 8.3MB），再把打包结果传过去
            Barrier(cl, mvRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, g_mvPacked.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            SetPass(cl, g_psoMvPack.Get(), kSrvMvCur, kSrvMvCur, kSrvMvCur, kSrvMvCur, kSrvMvCur,
                    kSrvMvPacked, kSrvMvPacked, kNetW, kNetH);
            Barrier(cl, g_mvPacked.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, mvRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (!g_bform4k) {
                BformCopy(cl, color, g_mvPacked.Get(), depRes, jx, jy);
            } else {
                // ★ 4K 后置：B 形态下 A 不合成 ⇒ XeSS 可以提前到"敲铃之前"，顺序：
                //   ⑥ XeSS(本帧 2K 原图 → 4K) → 转 8bit → 拷进槽位 →（之后照常敲铃）
                //   这样 B 拿到的是【本帧】完整的 4K，零额外延迟。
                char b4[256];
                memcpy(b4, params, sizeof(b4));
                if (g_jitterScale != 1.0f) {
                    float jx4 = 0, jy4 = 0;
                    memcpy(&jx4, b4 + 48, 4); memcpy(&jy4, b4 + 52, 4);
                    jx4 *= g_jitterScale; jy4 *= g_jitterScale;
                    memcpy(b4 + 48, &jx4, 4); memcpy(b4 + 52, &jy4, 4);
                }
                if (real) real(ctx, cl, b4);
                did4kUpscale = true;
                // ★ B 形态下菜单要能被看见：B 显示的就是这张 4K 底图，
                //   所以把 ImGui 画进 pOutput 再转换 → 菜单自然跟着底图跨卡上屏。
                //   （原来菜单只画在 A 的输出上，被 B 的覆盖层盖住 → B 形态看不见菜单）
                ProxyImGuiDraw(cl, outp);
                    const int par8 = (int)(g_frame & 1);   // 双缓冲：本帧写哪一份
                    XferEnsure();   // ★ 必须先建好围栏：否则钩子不 Signal → 拷永远不触发（逻辑死锁）
                    // ★ 关键修复：游戏会双缓冲 pOutput（每帧换一张资源），而我们的 SRV 只绑了初始化那张
                    //   ⇒ 一半的帧我们转换的是"旧那张"（内容还是启动加载画面）→ 画面逐帧闪回加载画面
                    {
                        static ID3D12Resource* lastOutp = nullptr;
                        if (lastOutp != outp) {
                            lastOutp = outp;
                            D3D12_SHADER_RESOURCE_VIEW_DESC sd9{};
                            sd9.Format = g_outpRes ? g_outpRes->GetDesc().Format : DXGI_FORMAT_R10G10B10A2_UNORM;
                            sd9.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                            sd9.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            sd9.Texture2D.MipLevels = 1;
                            D3D12_CPU_DESCRIPTOR_HANDLE hSrv = g_heap->GetCPUDescriptorHandleForHeapStart();
                            hSrv.ptr += (SIZE_T)kSrvOutp * g_inc;
                            g_dev->CreateShaderResourceView(outp, &sd9, hSrv);
                            { static int sw9 = 0; if (sw9++ < 6) Log("4K 后置: pOutput 资源切换 → 重建 SRV（%p）第 %d 次", (void*)outp, sw9); }
                        }
                    }
                    DumpFlush(0);
                if (!g_outp8Ready[par8]) {
                    D3D12_HEAP_PROPERTIES hp8{}; hp8.Type = D3D12_HEAP_TYPE_DEFAULT;
                    D3D12_RESOURCE_DESC td8{};
                    td8.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    td8.Width = g_outW; td8.Height = g_outH;
                    td8.DepthOrArraySize = 1; td8.MipLevels = 1;
                    td8.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td8.SampleDesc.Count = 1;
                    td8.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    if (SUCCEEDED(g_dev->CreateCommittedResource(&hp8, D3D12_HEAP_FLAG_NONE, &td8,
                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_outp8[par8]))) && g_outp8[par8]) {
                        D3D12_UNORDERED_ACCESS_VIEW_DESC ud8{};
                        ud8.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        ud8.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                        D3D12_CPU_DESCRIPTOR_HANDLE h8 = g_heap->GetCPUDescriptorHandleForHeapStart();
                        h8.ptr += (SIZE_T)(par8 ? kSrvOutp8b : kSrvOutp8) * g_inc;   // ★ 两张缓冲各用自己的 UAV
                        g_dev->CreateUnorderedAccessView(g_outp8[par8].Get(), nullptr, &ud8, h8);
                        g_outp8Ready[par8] = true;
                        Log("BFORM: 4K 后置转换目标就绪 %ux%u（8bit）", g_outW, g_outH);
                    }
                }
                // ★ 直接拷 pOutput（原生 10bit）：-- 实验失败，已回退（见下）
                if (g_outp8Ready[par8] && g_psoFmt8) {   // 8bit 转换路径（10bit 直传实验已回退：B 端拿到的是脏数据）
                    Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    Barrier(cl, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    ID3D12DescriptorHeap* hh9[] = { g_heap.Get() };
                    cl->SetDescriptorHeaps(1, hh9);
                    // (删除了重复的 SetPass 行)
                    SetPass(cl, g_psoFmt8.Get(), kSrvOutp, kSrvOutp, kSrvOutp, kSrvOutp, kSrvOutp,
                            (par8 ? kSrvOutp8b : kSrvOutp8), (par8 ? kSrvOutp8b : kSrvOutp8), g_outW, g_outH);
                    Barrier(cl, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    // ★ 4 坑 FIFO：每帧登记一个待拷；只要"又发生过提交"就逐个启动，任何一帧都不会被覆盖
                    for (int pi = 0; pi < 4; ++pi) {
                        if (!g_pend[pi].valid) continue;
                        if (g_lastSigV <= g_pend[pi].sig) continue;     // 该帧的转换还没提交完
                        if (!g_pend[pi].valid) continue;
                        int pp = g_pend[pi].parity; UINT ps = (UINT)g_pend[pi].slot; int pf = g_pend[pi].frameTag;
                        if (!g_outp8[pp]) { g_pend[pi].valid = false; continue; }
                        g_pend[pi].valid = false;
                        g_xferPendFrameTag = pf;
                        XferKickColor(g_outp8[pp].Get(), ps, g_lastSigV);
                    }
                    // 登记本帧（取写指针那一坑；若仍在使用中，说明积压了，计数并覆盖最旧的）
                    {
                        LONG w = InterlockedIncrement(&g_pendW) & 3;
                        if (g_pend[w].valid) { InterlockedIncrement(&g_xferSkip); }
                        g_pend[w].slot = (int)(g_frame % K);
                        g_pend[w].parity = par8;
                        g_pend[w].sig = g_lastSigV;
                        g_pend[w].frameTag = (int)g_frame;
                        g_pend[w].valid = true;
                    }
                    g_ourList = cl;




                    BformCopy(cl, g_outp8[par8].Get(), g_mvPacked.Get(), depRes, 0.0f, 0.0f);  // 内部跳颜色
                    Barrier(cl, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                    DumpRecord(cl, g_dev.Get(), 0, g_outp8[par8].Get(), D3D12_RESOURCE_STATE_COMMON);
                }
            }
        } else {
            if (!g_bform4k) BformCopy(cl, color, mvRes, depRes, jx, jy);
        }
    }

    // ---------- ④ [GPU] 把 slot[N-1] 降采样到 1080p → 回读 ----------
    if (N >= 1) {
        Slot& F = g_slots[(N - 1) % K];
        g_curJitX = F.jx; g_curJitY = F.jy;   // 去抖动必须用【这一帧被降采样的槽位】的 jitter
        Barrier(cl, F.color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        SetPass(cl, g_psoDown.Get(), (UINT)((N - 1) % K), kSrvRes, kSrvRes, kSrvRes, kSrvRes, kSrvNetS, kSrvSmooth, kNetW, kNetH);
        Barrier(cl, F.color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(cl, g_netS.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rd{}; rd.pResource = g_rb.Get();
        rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; rd.PlacedFootprint = g_fpRb;
        D3D12_TEXTURE_COPY_LOCATION rs{}; rs.pResource = g_netS.Get();
        rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; rs.SubresourceIndex = 0;
        cl->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
        {   // S5-2：把这一帧的输入存进它自己的槽位（供之后算残差）
            int fi = (N - 1 + K) % K;
            if (g_fedTex[fi]) {
                D3D12_TEXTURE_COPY_LOCATION fd{};
                fd.pResource = g_fedTex[fi].Get();
                fd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                fd.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION fs{};
                fs.pResource = g_netS.Get();
                fs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                fs.SubresourceIndex = 0;
                Barrier(cl, g_fedTex[fi].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyTextureRegion(&fd, 0, 0, 0, &fs, nullptr);
            }
        }
        if (g_xWrite && g_xOk && g_xBufIn) {
            Barrier(cl, g_xBufIn.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION xd{};
            xd.pResource = g_xBufIn.Get();
            xd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            xd.PlacedFootprint.Offset = XHDR;
            xd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            xd.PlacedFootprint.Footprint.Width = kNetW;
            xd.PlacedFootprint.Footprint.Height = kNetH;
            xd.PlacedFootprint.Footprint.Depth = 1;
            xd.PlacedFootprint.Footprint.RowPitch = kNetW * 4;
            D3D12_TEXTURE_COPY_LOCATION xs{};
            xs.pResource = g_netS.Get();
            xs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            xs.SubresourceIndex = 0;
            cl->CopyTextureRegion(&xd, 0, 0, 0, &xs, nullptr);
            Barrier(cl, g_xBufIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            static int cnt = 0;
            if ((cnt++ % 300) == 0) Log("R3: 已把 1080p 写入跨卡 buffer（第 %d 次）", cnt + 1);
        }
        Barrier(cl, g_netS.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // ---------- ⑤ [GPU] 合成：slot[presentFrame] + 残差 → pColor ----------
    Slot* P = (presentFrame >= 0) ? &g_slots[presentFrame % K] : nullptr;
    bool composed = false;
    if (P && P->hasAnswer) {
        // B1：第一次合成前把时域状态显式清成中性 0.5（不再依赖预热帧覆盖）
        static bool stateCleared = false;
        if (!stateCleared && g_cpuResSmooth.ptr) {
            stateCleared = true;
            D3D12_GPU_DESCRIPTOR_HANDLE gh = g_heap->GetGPUDescriptorHandleForHeapStart();
            gh.ptr += (SIZE_T)kSrvSmooth * g_inc;
            const FLOAT neutral[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
            cl->ClearUnorderedAccessViewFloat(gh, g_cpuResSmooth, g_resSmooth.Get(), neutral, 0, nullptr);
            Log("B1: 时域状态已清为中性 0.5（均值应为 127）");
        }
        // 状态纹理预热：前 12 次合成强制 alpha=1，把初值（0=强压暗）彻底覆盖掉
        if (g_stateWarmup > 0) { g_alpha = 1.0f; --g_stateWarmup; }
        // 【诊断】A 侧同口径：|答案 − 喂入|（与 B 侧 DIAG4 的"|答案-喂入|"是同一个定义）
        //   两边都是 1080p RGBA8 裸字节，且按帧身份配对，所以 A/B 这两个数可以直接比
        {
            static int aCnt = 0;
            if ((aCnt++ % 150) == 0 && !P->answer.empty() && P->answer.size() == P->feedInput.size()) {
                const size_t n = P->answer.size();
                double s = 0.0; size_t cnt = 0;
                for (size_t i = 0; i + 16 <= n; i += 16) {   // 每 4 个像素取 1 个，取 RGB 三通道
                    for (int c = 0; c < 3; ++c) {
                        int d = (int)P->answer[i + c] - (int)P->feedInput[i + c];
                        s += (d < 0) ? -d : d;
                    }
                    cnt += 3;
                }
                double mean = cnt ? s / (double)cnt : 0.0;
                Log("DIAGA[%d]: |答案-喂入| 均值 = %.2f/255（%.1f%%）← A 形态，与 B 的 DIAG4 同口径",
                    N, mean, mean * 100.0 / 255.0);
            }
        }
        std::vector<uint8_t> enc;
        BuildResidual(*P, enc);
        D3D12_RANGE nr{ 0, 0 };
        void* m = nullptr;
        if (SUCCEEDED(g_resUp->Map(0, &nr, &m)) && m) {
            UINT pitch = g_fpRes.Footprint.RowPitch;
            for (UINT y = 0; y < kNetH; ++y)
                memcpy((unsigned char*)m + (size_t)y * pitch, &enc[(size_t)y * kNetW * 4], (size_t)kNetW * 4);
            D3D12_RANGE nw{ 0, (SIZE_T)g_bytesRes };
            g_resUp->Unmap(0, &nw);

            D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = g_resTex.Get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = g_resUp.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = g_fpRes;
            cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            Barrier(cl, g_resTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            {
                ID3D12Resource* fr = g_fedTex[presentFrame % K].Get();
                if (fr) Barrier(cl, fr, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            if (g_ansLocal && g_xBufOut) {   // S5-4：答案从共享 buffer 拷到本地（供 SRV 读）
                Barrier(cl, g_xBufOut.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cl, g_ansLocal.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyBufferRegion(g_ansLocal.Get(), 0, g_xBufOut.Get(), XHDR + (UINT64)(presentFrame % K) * ANS_BYTES, ANS_BYTES);
                Barrier(cl, g_ansLocal.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cl, g_xBufOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                static bool loggedS54 = false;
                if (!loggedS54) { loggedS54 = true; Log("S5-4: 答案已拷到本地 buffer（可建 SRV）"); }
            }
            // GPU 残差路径下 t1 改绑【上一帧槽位颜色】，供颜色一致性验证使用
    UINT srvPrevCol = g_useGpuResid ? (UINT)((presentFrame + K - 1) % K) : kSrvRes;
    SetPass(cl, g_psoResid.Get(), (UINT)(presentFrame % K), srvPrevCol, (UINT)(kSrvMvBase + (presentFrame % K)), (UINT)(kSrvFed + (presentFrame % K)), (UINT)kSrvAns, kSrvPre, kSrvSmooth, g_cw, g_ch);
            {
                ID3D12Resource* fr = g_fedTex[presentFrame % K].Get();
                if (fr) Barrier(cl, fr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            Barrier(cl, g_resTex.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

            // g_pre → pColor
            // B1（g_resid4k）：不改"配对"，仍喂**同一个槽位帧**给上采样器，
            //                 但 pColor 里只放【槽位原图】，残差 e 留到上采样后用 Add4K 叠到 4K 上。
            if (!g_resid4k) {
                Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cl, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyResource(color, g_pre.Get());
                Barrier(cl, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            } else {
                Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cl, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                cl->CopyResource(color, P->color.Get());
                Barrier(cl, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                static int l4k = 0;
                if ((l4k++ % 300) == 0) Log("B1: 帧%d pColor ← 槽位%d 原图（残差留到 4K 叠加）", N, presentFrame % K);
            }
            composed = true;
        }
    }
    // ---------- ⑥ 调真 XeSS（替换成该槽位的 MV/深度/jitter）----------
    int rc = 0;
    if (!did4kUpscale) {   // 4K 后置模式下，XeSS 已经提前在"敲铃之前"调过了（见 ② 区），这里不再调
        char buf[256];
        memcpy(buf, params, sizeof(buf));
        if (composed && P) {
            if (P->mv)    *(void**)(buf + 8)  = P->mv.Get();
            if (P->depth) *(void**)(buf + 16) = P->depth.Get();
            memcpy(buf + 48, &P->jx, 4);
            memcpy(buf + 52, &P->jy, 4);

        }
        // jitter 缩放实验：0=完全不抖（静止最稳，无时域增益），1=原样
        if (g_jitterScale != 1.0f) {
            float jx = 0, jy = 0;
            memcpy(&jx, buf + 48, 4); memcpy(&jy, buf + 52, 4);
            jx *= g_jitterScale; jy *= g_jitterScale;
            memcpy(buf + 48, &jx, 4); memcpy(buf + 52, &jy, 4);
        }
        rc = real ? real(ctx, cmdList, buf) : -1;
    }

    // ---------- ⑥.2 【bform=9】把 XeSS 刚产出的 4K 画面拷进跨卡槽位（同帧，零额外延迟）----------
    //   为什么能同帧：XeSS 是我们自己在这个命令列表里调用的（⑥），所以拷贝可以排在它后面。
    //   若改成"下一帧再从 pOutput 取"，就会白晚一帧。
    if (!did4kUpscale && g_bform4k && g_bformOn && g_bformStage >= 6 && g_outpRes && g_psoFmt8) {
        if (!g_outp8Ready[0]) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = g_outW; td.Height = g_outH;
            td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_outp8[0]))) && g_outp8[0]) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += (SIZE_T)kSrvOutp8 * g_inc;
                g_dev->CreateUnorderedAccessView(g_outp8[0].Get(), nullptr, &ud, h);
                g_outp8Ready[0] = true;
                Log("BFORM: 4K 后置转换目标就绪 %ux%u（8bit）", g_outW, g_outH);
            }
        }
        if (g_outp8Ready[0]) {
            Barrier(cl, g_outpRes.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cl, g_outp8[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* hh9[] = { g_heap.Get() };
            cl->SetDescriptorHeaps(1, hh9);
            SetPass(cl, g_psoFmt8.Get(), kSrvOutp, kSrvOutp, kSrvOutp, kSrvOutp, kSrvOutp,
                    kSrvOutp8, kSrvOutp8, g_outW, g_outH);
            Barrier(cl, g_outpRes.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cl, g_outp8[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            // ★ 注意：这里【不】拷进槽位！拷贝挪到下一帧开头（敲铃之前），见 BformCopy 调用处。
            //   原因：worker 在 A 敲铃时就会去解码槽位，如果拷贝发生在敲铃之后，
            //   B 会读到"正在被写"的槽位 → 每帧抖动 = 闪。
        }
    }

    // ---------- ⑥.5 [B1] 残差在 4K 上叠加：outp += bilinear(g_pre − 槽位原图) ----------
    if (g_resid4k && composed && P && g_pre4k && g_psoAdd4K) {
        Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, g_pre4k.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* h4[] = { g_heap.Get() };
        cl->SetDescriptorHeaps(1, h4);
        SetPass(cl, g_psoAdd4K.Get(), kSrvOutp, kSrvPreSrv,
                (UINT)(presentFrame % K), kSrvPre4K, kSrvPre4K,
                kSrvPre4K, kSrvPre4K, g_outW, g_outH);
        Barrier(cl, g_pre4k.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, outp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(outp, g_pre4k.Get());
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, g_pre4k.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, P->color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(cl, g_pre.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        static int l4k2 = 0;
        if ((l4k2++ % 300) == 0) Log("B1: 帧%d 已把残差叠到 4K（outp %ux%u）", N, g_outW, g_outH);
    }

    if (!did4kUpscale) ProxyImGuiDraw(cl, outp);   // 4K 模式下已在 ⑥ 之后提前画过（要跟着底图跨卡上屏）
    MirrorTick(cl, outp);       // S1：镜像叠层（默认关；关时立即返回）
    if ((g_frame % 300) == 0) BwPollFile();
    BwTick();                   // 带宽测试（默认关；关时立即返回）

    double ms = std::chrono::duration<double, std::milli>(Clock::now() - tA).count();
    if ((N % 150) == 0)
        Log("stats: 帧%d 提交 %.2fms worker=%.1fms #%d 合成=%d", N, ms, g_worker.lastMs, g_worker.processed, (int)composed);
    return rc;
}


// ---------------- ImGui 覆盖层（Del 唤出，键盘导航） ----------------
static ComPtr<ID3D12DescriptorHeap> g_imguiSrvHeap;
static ComPtr<ID3D12DescriptorHeap> g_imguiRtvHeap;
static bool g_imguiInited = false, g_imguiFail = false;
static WNDPROC g_origWndProc = nullptr;

// 菜单打开时把键鼠消息吞掉，不传给游戏（OptiScaler 的做法）
static LRESULT CALLBACK ProxyWndProc(HWND, UINT, WPARAM, LPARAM);   // 前置声明
// 按进程 ID + 面积找游戏主窗口（不依赖"前台窗口"，初始化时游戏可能还不是前台）
struct FindWndCtx { DWORD pid; HWND best; long bestArea; };
static BOOL CALLBACK FindWndProc(HWND h, LPARAM lp) {
    auto* c = (FindWndCtx*)lp;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h)) return TRUE;
    RECT r{}; if (!GetClientRect(h, &r)) return TRUE;
    long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > c->bestArea) { c->bestArea = area; c->best = h; }
    return TRUE;
}
static HWND FindGameWindowOnce() {
    FindWndCtx c{ GetCurrentProcessId(), nullptr, 0 };
    EnumWindows(FindWndProc, (LPARAM)&c);
    return c.best;
}
// 只定位句柄、不装 WndProc：B 形态出画需要它，但装 WndProc（吞键鼠）只在开菜单时才需要。
// 以前两者绑在一起、且唯一入口是 Del → 不开菜单 B 就没有窗口可用。
static HWND ResolveGameHwnd() {
    if (g_gameHwnd && IsWindow(g_gameHwnd)) return g_gameHwnd;
    HWND h = FindGameWindowOnce();
    if (!h) return nullptr;
    RECT r{};
    if (!GetClientRect(h, &r)) return nullptr;
    const long cw = r.right - r.left, ch = r.bottom - r.top;
    if (cw < 640 || ch < 360) return nullptr;   // 太小的窗口（启动器/提示框）不是主渲染窗口
    g_gameHwnd = h;
    char cls[128] = { 0 }, ttl[256] = { 0 };
    GetClassNameA(h, cls, 128); GetWindowTextA(h, ttl, 256);
    Log("WND: 已定位游戏主窗口 类=%s 标题=%s 客户区=%ldx%ld（不依赖菜单）", cls, ttl, cw, ch);
    return g_gameHwnd;
}
static void EnsureWndHook() {
    ResolveGameHwnd();
    if (g_gameHwnd && !g_origWndProc) {
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)ProxyWndProc);
        char cls2[128] = { 0 }, ttl2[256] = { 0 };
        GetClassNameA(g_gameHwnd, cls2, 128); GetWindowTextA(g_gameHwnd, ttl2, 256);
        RECT rr{}; GetClientRect(g_gameHwnd, &rr);
        Log("ImGui: hook 窗口 类=%s 标题=%s 客户区=%dx%d 原过程=%p", cls2, ttl2, (int)(rr.right-rr.left), (int)(rr.bottom-rr.top), (void*)g_origWndProc);
    }
}
static LRESULT CALLBACK ProxyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_menuOpen) {
        switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR: case WM_INPUT:
            return 0;   // 吞掉
        default: break;
        }
    }
    return g_origWndProc ? CallWindowProcW(g_origWndProc, h, m, w, l) : DefWindowProcW(h, m, w, l);
}
static bool g_menuVisible = false;

static bool ProxyImGuiInit() {
    if (g_imguiInited || g_imguiFail) return g_imguiInited;
    if (!g_dev) { g_imguiFail = true; return false; }
    __try {
        D3D12_DESCRIPTOR_HEAP_DESC sh{};
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.NumDescriptors = 8;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(g_imguiSrvHeap.GetAddressOf())))) { g_imguiFail = true; return false; }
        D3D12_DESCRIPTOR_HEAP_DESC rh{};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 1;
        if (FAILED(g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(g_imguiRtvHeap.GetAddressOf())))) { g_imguiFail = true; return false; }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();                 // ★ 上次漏掉这句 → 必然崩溃
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // 中文字体：默认字体没有汉字，必须从系统字体加载（失败则回退默认字体）
        {
            const char* cands[] = {
                "C:\\Windows\\Fonts\\msyh.ttc",  "C:\\Windows\\Fonts\\msyhbd.ttc",
                "C:\\Windows\\Fonts\\simhei.ttf","C:\\Windows\\Fonts\\simsun.ttc",
                "C:\\Windows\\Fonts\\Deng.ttf"
            };
            bool fontOk = false;
            for (const char* p : cands) {
                if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
                if (io.Fonts->AddFontFromFileTTF(p, 20.0f, nullptr,
                        io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) { fontOk = true; break; }
            }
            if (!fontOk) { io.Fonts->AddFontDefault(); Log("ImGui: 未找到中文字体，回退默认字体"); }
            else Log("ImGui: 中文字体已加载");
        }
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2((float)g_outW, (float)g_outH);
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        if (!ImGui_ImplDX12_Init(g_dev.Get(), 4, DXGI_FORMAT_R10G10B10A2_UNORM, g_imguiSrvHeap.Get(),
                                 g_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                                 g_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart())) {
            g_imguiFail = true; Log("ImGui: 后端初始化失败（已禁用）"); return false;
        }
        ImGui::StyleColorsDark();
        g_baseStyle = ImGui::GetStyle();          // 保存未缩放的基准
        ImGui::GetStyle().ScaleAllSizes(1.6f);    // 基础缩放（界面缩放滑条在此之上再乘）
        io.FontGlobalScale = 1.0f;
        Log("ImGui: hook 推迟到打开菜单时再装（那时游戏一定是前台）");
        PatchInputIAT();
        g_imguiInited = true;
        Log("ImGui: 覆盖层就绪（Del 开关）");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_imguiFail = true;
        Log("ImGui: 初始化异常（已禁用）");
        return false;
    }
    return g_imguiInited;
}

static void ProxyImGuiDraw(ID3D12GraphicsCommandList* cl, ID3D12Resource* rt) {
    if (!g_imguiInited || g_imguiFail || !rt) return;
    g_menuVisible = g_menuOpen;
    if (!g_menuVisible) return;
    __try {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)g_outW, (float)g_outH);
        io.DeltaTime = 1.0f / 60.0f;
        auto key = [&](ImGuiKey k, int vk) { io.AddKeyEvent(k, (GetAsyncKeyState(vk) & 0x8000) != 0); };
        key(ImGuiKey_UpArrow, VK_UP);     key(ImGuiKey_DownArrow, VK_DOWN);
        key(ImGuiKey_LeftArrow, VK_LEFT); key(ImGuiKey_RightArrow, VK_RIGHT);
        key(ImGuiKey_Enter, VK_RETURN);   key(ImGuiKey_Escape, VK_ESCAPE);
        key(ImGuiKey_Tab, VK_TAB);
        key(ImGuiKey_Delete, VK_DELETE);
        // 鼠标：屏幕坐标 → 客户区 → 显示尺寸
        POINT p{};
        if (GetCursorPos(&p) && g_gameHwnd) {
            ScreenToClient(g_gameHwnd, &p);
            RECT rc{}; GetClientRect(g_gameHwnd, &rc);
            float cw = (float)(rc.right - rc.left), ch = (float)(rc.bottom - rc.top);
            if (cw > 0 && ch > 0)
                io.AddMousePosEvent((float)p.x * (float)g_outW / cw, (float)p.y * (float)g_outH / ch);
        }
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        io.MouseDrawCursor = true;

        // 界面缩放：值变了就重建样式（字体按 FontGlobalScale 缩放，不需要重建字体纹理）
        if (g_uiScale != g_uiApplied) {
            g_uiApplied = g_uiScale;
            ImGui::GetStyle() = g_baseStyle;
            ImGui::GetStyle().ScaleAllSizes(1.6f * g_uiApplied);
            io.FontGlobalScale = g_uiApplied;
        }
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(900, 780), ImGuiCond_FirstUseEver);
        ImGui::Begin("跨卡 NR 参数（Del 关闭 · 鼠标操作 · 悬停看说明）", &g_menuVisible,
                     ImGuiWindowFlags_NoCollapse);   // 允许拖动(NoMove 未设)与缩放
        ImGui::TextDisabled("════ 强度（改多少 / 改多硬）════");
        float g = (float)g_gain;
        if (ImGui::SliderFloat("残差增益", &g, 0.0f, 2.0f, "%.2f")) g_gain = g;
        ImGui::SetItemTooltip("网络修正量的整体放大倍数。调大：效果更明显、闪烁也更明显；调小：更稳但更弱。默认 0.66");
        float lim = (float)g_limit;
        if (ImGui::SliderFloat("残差上限", &lim, 0.02f, 1.0f, "%.2f")) g_limit = lim;
        ImGui::SetItemTooltip("每个像素最多改多少，防异常大改动。调大：细节更足但双轮廓/亮边风险上升。默认 0.20");
        ImGui::SameLine(); if (ImGui::SmallButton("0.20")) g_limit = 0.20f;
        ImGui::SameLine(); if (ImGui::SmallButton("0.25")) g_limit = 0.25f;
        ImGui::SameLine(); if (ImGui::SmallButton("0.35")) g_limit = 0.35f;
        ImGui::SameLine(); if (ImGui::SmallButton("1.00")) g_limit = 1.00f;
        float eg = (float)g_edge;
        if (ImGui::SliderFloat("边缘保护", &eg, 0.0f, 4.0f, "%.2f")) g_edge = eg;
        ImGui::SetItemTooltip("梯度越强的地方改得越少，抑制亮边。调大：边缘更干净但边缘锐化减弱。默认 0.6");
        float sa = (float)g_sat;
        if (ImGui::SliderFloat("残差去饱和", &sa, 0.0f, 1.0f, "%.2f")) g_sat = sa;
        ImGui::SetItemTooltip("压掉残差里的彩色成分，防彩色噪点。调小：色彩更浓但彩色噪点增加。默认 0.9");
        // 空间平滑已按要求移除（实测只变糊、不治闪烁）；g_smooth 恒为 0

        ImGui::Separator();
        ImGui::TextDisabled("════ 稳定性（防闪烁 / 防拖影）════");
        float tp = (float)g_temporalStatic;
        if (ImGui::SliderFloat("时域强度", &tp, 0.0f, 1.0f, "%.3f")) { g_temporalStatic = tp; g_stateWarmup = 4; }
        {
            float ma = (float)g_motionAlpha;
            if (ImGui::SliderFloat("运动时域上限", &ma, 0.02f, 1.0f, "%.2f")) g_motionAlpha = ma;
            ImGui::SetItemTooltip("运动中允许使用的历史权重上限。1.0=完全不用历史（旧行为，运动内部会闪）；0.2~0.4=保留历史，靠颜色一致性防拖影");
        }
        {
            float mlo = (float)g_motionLo, mhi = (float)g_motionHi;
            if (ImGui::SliderFloat("运动下界", &mlo, 0.0f, 0.10f, "%.3f")) g_motionLo = mlo;
            ImGui::SetItemTooltip("运动量低于此值 → 完全时域平滑（静止场景）");
            if (ImGui::SliderFloat("运动上界", &mhi, 0.01f, 0.30f, "%.3f")) g_motionHi = mhi;
            ImGui::SetItemTooltip("运动量高于此值 → 完全不用历史（防拖影）。两个值之间是连续过渡。上界应大于下界");
        }
        ImGui::SetItemTooltip("每帧新残差占的比重，0.02 = 98% 沿用历史。调大：响应快但闪烁；调小：更稳但可能滞后。默认 0.02");
        float mv = (float)g_mvScale;
        if (ImGui::SliderFloat("MV 比例", &mv, -0.002f, 0.002f, "%.5f")) g_mvScale = mv;
        ImGui::SetItemTooltip("运动矢量的单位校准（RE2 实测 -0.00045 = 像素单位且反向）。错了会拖影或对不上");
        float mc = (float)g_mvConf;
        if (ImGui::SliderFloat("MV 置信衰减", &mc, 0.0f, 30.0f, "%.1f")) g_mvConf = mc;
        ImGui::SetItemTooltip("运动量越大越不信旧数据。调大：快速运动更少用历史（防拖影）；调小：更依赖历史（可能拖影）。默认 1.0");
        float gk = (float)g_gateK;
        if (ImGui::SliderFloat("逐像素门限", &gk, 0.0f, 80.0f, "%.0f")) g_gateK = gk;
        ImGui::SetItemTooltip("重投影后旧残差与当前差太多就丢弃。调大：更保守（防拖影、可能闪）；调小：更稳（可能拖）。默认 40");

        ImGui::Separator();
        ImGui::TextDisabled("════ 风格化（画风 / 观感）════");
        float tr = (float)g_transfer;
        if (ImGui::SliderFloat("官方细节强度", &tr, 0.0f, 1.0f, "%.2f")) g_transfer = tr;
        ImGui::SetItemTooltip("运行时自己的细节转移强度（字段 +64，需 flags bit0）。提现程度取决于画风，默认 1.0");
        float cs = (float)g_colorStr;
        if (ImGui::SliderFloat("官方色彩强度", &cs, 0.0f, 1.0f, "%.2f")) g_colorStr = cs;
        ImGui::SetItemTooltip("运行时自己的色彩转移强度（字段 +68，需 flags bit0）。默认 1.0");
        ImGui::Spacing();
        ImGui::TextDisabled("── 网络风格（mochizuki 控制块 · 立即生效，不重建会话）──");
        {
            bool on = g_nrCtrlOn;
            if (ImGui::Checkbox("风格化总开关（控制块 0x2C，必须开否则整块无效）", &on)) {
                g_nrCtrlOn = on; g_ctrlDirty = true;
            }
            float fi = (float)g_nrIntensity;
            if (ImGui::SliderFloat("网络强度", &fi, 0.0f, 2.0f, "%.2f")) { g_nrIntensity = fi; g_ctrlDirty = true; }
            ImGui::SetItemTooltip("mochizuki 自己的强度（0x08）。与我们合成层的「残差增益」是两层，可叠加");
            int st = g_nrStyle;
            if (ImGui::SliderInt("风格 STYLE（0默认 / 1自然 / 2电影）", &st, 0, 2)) { g_nrStyle = st; g_ctrlDirty = true; }
            ImGui::SetItemTooltip("交接包实测：1=更平更冷、2=更软更青、0=最硬最保留原色");
            float ft = (float)g_nrTone, fs = (float)g_nrStruct, fk = (float)g_nrSkin;
            if (ImGui::SliderFloat("局部影调", &ft, 0.0f, 2.0f, "%.2f")) { g_nrTone = ft; g_ctrlDirty = true; }
            if (ImGui::SliderFloat("局部结构", &fs, 0.0f, 2.0f, "%.2f")) { g_nrStruct = fs; g_ctrlDirty = true; }
            if (ImGui::SliderFloat("皮肤结构", &fk, -1.0f, 2.0f, "%.2f")) { g_nrSkin = fk; g_ctrlDirty = true; }
            ImGui::SetItemTooltip("皮肤结构要配合下面的「自动人物掩码」才明显（-1 = 不参与）");
            bool am = g_nrAutoMask;
            if (ImGui::Checkbox("自动人物/皮肤掩码", &am)) { g_nrAutoMask = am; g_ctrlDirty = true; }
            float gr = (float)g_nrGrade;
            if (ImGui::SliderFloat("风格分级 grade（0x20）", &gr, 0.0f, 8.0f, "%.1f")) { g_nrGrade = gr; g_ctrlDirty = true; }
            ImGui::SetItemTooltip("mochizuki 缺省 = 1.0（≈1/255，效果很弱）；2.0 才明显（≈9/255，交接包实测）；上限 8");
            ImGui::TextDisabled("（已下发: %s）", g_ctrlDirty ? "等待 worker 下发" : "已生效");
        }

        ImGui::Separator();
        ImGui::TextDisabled("════ 模型（换模型 = 重建会话，不换后端）════");
        ImGui::Text("当前请求: %s", g_modelSel ? "Original 71-block（旧）" : "Small 31-block（新）");
        if (ImGui::Button("切到 Original（旧模型）")) {
            g_modelSel = 1;
            Log("菜单: 请求切到 Original 71-block（worker 会重建会话）");
        }
        ImGui::SetItemTooltip("和 AMDNR 的做法一致：旧会话继续出帧，worker 用新请求重建会话，切完时域状态重来");
        ImGui::SameLine();
        if (ImGui::Button("切到 Small（新模型）")) {
            g_modelSel = 0;
            Log("菜单: 请求切到 Small 31-block（worker 会重建会话）");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("（生效后看下面“运行时状态”的 model=）");
        bool kdAuto = false; (void)kdAuto;
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "喂帧滞后固定 %d 帧；可调的是「余量」", kFeedLag);
        int margin = (int)g_kDelay - kFeedLag;
        if (ImGui::SliderInt("余量（帧）", &margin, 1, K - 1 - kFeedLag)) {
            g_kDelay = kFeedLag + margin;
            Log("菜单: 余量=%d 帧 → kDelay=%d（K=%d）", margin, (int)g_kDelay, K);
        }
        ImGui::SetItemTooltip("余量 = kDelay − 喂帧滞后，也就是 worker 有多少帧的时间把答案交出来\n1 帧 ≈ 3 ms 抗抖（Original）\n2 帧 ≈ 20 ms 抗抖，代价是屏幕上多一帧延迟（+16.7ms@60fps）");
        ImGui::TextDisabled("kDelay = %d（上限 K−1 = %d）｜屏上内容延迟 ≈ %d 帧 ≈ %.0f ms@60fps",
                            (int)g_kDelay, K - 1, (int)g_kDelay, g_kDelay * 16.7f);
        ImGui::TextDisabled("（历史：滞后写成 1 或 kDelay=2 均实测错位——回读天然滞后一帧，不是时间不够）");
        ImGui::SetItemTooltip("硬约束 kDelay ≥ feedLag + 1；实测下限 3（跨卡 CPU 中转每帧 ~8.3MB 回读，1 帧预算装不下）");

        ImGui::Separator();
        ImGui::TextDisabled("════ 调试 ════");
        float jsc = (float)g_jitterScale;
        if (ImGui::SliderFloat("抖动缩放", &jsc, 0.0f, 1.5f, "%.2f")) g_jitterScale = jsc;
        ImGui::SetItemTooltip("乘到传给 XeSS 的 jitter 上。0=完全不抖（静止最稳，但无时域增益）；1.0=原样。用于诊断上采样抖动");
        bool djd = g_dejitter;
        if (ImGui::Checkbox("NR 输入去抖动", &djd)) g_dejitter = djd;
        ImGui::SetItemTooltip("用该帧的 jitter 反向偏移降采样，使 NR 看到投影稳定的图像。静止物体闪烁时可试");
        if (g_dejitter) {
            ImGui::SameLine();
            float sg = (float)g_dejitterSign;
            if (ImGui::SliderFloat("符号", &sg, -1.0f, 1.0f, "%.0f")) g_dejitterSign = (sg < 0) ? -1.0f : 1.0f;
            ImGui::SetItemTooltip("+1 / -1：符号反了会变成双倍抖动，两个都试一下");
        }
        int dv = g_debugView;
        if (ImGui::SliderInt("调试视图", &dv, 0, 4)) g_debugView = dv;
        ImGui::SetItemTooltip("0=正常 1=代理 2=纯网络 3=差异x20（看残差） 4=着色。需 flags bit1，配合直出使用");
        bool bp = g_bypass;
        if (ImGui::Checkbox("直出运行时输出（bypass）", &bp)) g_bypass = bp;
        ImGui::SetItemTooltip("直接把运行时输出当画面，不叠原图。此时上面所有强度/稳定性参数都会失效");
        // ---- S1：镜像叠层（纯观测，不改画质）----
        {
            bool mo = g_mirOn;
            if (ImGui::Checkbox("镜像叠层（S1·测 present 代价，F11）", &mo)) {
                g_mirOn = mo;
                Log("*** 菜单: 镜像叠层 %s ***", mo ? "开" : "关");
            }
            ImGui::SetItemTooltip("把成品帧再用『合成交换链+DComp 视觉』盖一层到游戏窗口上。"
                                  "不改画质、不改 NR；只用来测『自己拥有 present』的代价。"
                                  "关掉后叠层立即撤掉。默认关。");
            ImGui::SameLine();
            const char* st = (g_mirState == 1) ? "就绪" : (g_mirState == 2 ? "失败" : (g_mirState == 3 ? "已关" : "未建"));
            ImGui::TextDisabled("状态: %s", st);
        }
        if (g_bypass) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "参数不生效"); }

        ImGui::Separator();
        if (ImGui::Button("保存设置（独立备份）")) {
            SaveParamsToFile(CaptureParams());
        }
        ImGui::SetItemTooltip("把当前参数写进 xess_proxy.saved.ini。出厂默认在代码里，任何按钮都不会覆盖它");
        ImGui::SameLine();
        if (ImGui::Button("撤销改动")) {
            if (g_sessionStartValid) {
                ApplyParams(g_sessionStart);
                Log("菜单: 已撤销本次改动（回到本次启动时的参数）");
            } else {
                Log("菜单: 撤销不可用（还没记录启动状态）");
            }
        }
        ImGui::SetItemTooltip("丢弃本次游戏里所有未保存的调整，回到刚启动时的参数");
        ImGui::SameLine();
        if (ImGui::Button("恢复上次保存")) {
            Params p = CaptureParams();
            if (LoadParamsFromFile(p)) {
                ApplyParams(p);
                Log("菜单: 已恢复上次保存的设置");
            }
        }
        ImGui::SetItemTooltip("读取 xess_proxy.saved.ini 并应用；没有存档时只在日志里提示，不改动参数");
        ImGui::SameLine();
        if (ImGui::Button("恢复默认设置")) {
            ApplyParams(FactoryParams());
            // 把"恢复成了什么"写进日志：以后再出现"点了没反应"，一眼能分辨
            // 是按钮没吃到点击、还是数值回了但画面没回。
            Log("菜单: 已恢复默认设置 → 网络强度=%.2f 风格=%d 影调=%.2f 结构=%.2f 皮肤=%.2f 掩码=%d grade=%.1f 总开关=%d"
                "（mochizuki 运行时真默认；你的存档文件保留）",
                (float)g_nrIntensity, (int)g_nrStyle, (float)g_nrTone, (float)g_nrStruct,
                (float)g_nrSkin, (int)g_nrAutoMask, (float)g_nrGrade, (int)g_nrCtrlOn);
        }
        ImGui::SetItemTooltip("回到 mochizuki 运行时的真默认：网络强度1.00 / 风格0 / 影调1.00 / 结构1.00 / 皮肤-1.00 / 掩码关 / grade 1.0 / 总开关开"
                             "（以前给的是 grade 2.00，视觉上明显更重）；不会删除 xess_proxy.saved.ini");
        ImGui::SameLine(); ImGui::TextDisabled("拖标题栏移动 · 拖右下角缩放");
        float us = (float)g_uiScale;
        if (ImGui::SliderFloat("界面缩放", &us, 0.6f, 2.5f, "%.2f")) g_uiScale = us;
        ImGui::SetItemTooltip("缩放整个菜单的文字与控件大小（不影响游戏画面）。默认 1.00");
        ImGui::Text("%.0f FPS | worker %.1f ms | 帧 %d", io.Framerate, g_worker.lastMs, g_frame);
        ImGui::TextWrapped("运行时状态: %s", g_statusLine[0] ? g_statusLine : "(还没拿到)");
        ImGui::End();
        ImGui::Render();

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_imguiRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_dev->CreateRenderTargetView(rt, nullptr, rtv);
        Barrier(cl, rt, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{ 0, 0, (float)g_outW, (float)g_outH, 0, 1 };
        D3D12_RECT sc{ 0, 0, (LONG)g_outW, (LONG)g_outH };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
        Barrier(cl, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        // 恢复我们自己的描述符堆（ImGui 会改掉它；游戏随后会自设，但这里先还原）
        ID3D12DescriptorHeap* hs[] = { g_heap.Get() };
        cl->SetDescriptorHeaps(1, hs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_imguiFail = true;
        Log("ImGui: 绘制异常（已禁用）");
    }
}
// =====================================================================================
// S1 镜像叠层：把游戏已经出好的成品帧（outp）再叠一层到游戏窗口上
// -------------------------------------------------------------------------------------
// 目的：只测「自己拥有 present」本身的代价（合成 vs iFlip / 输入穿透 / 稳定性），
//       用来决定新形态（B 自己出画）值不值得做。
//   · 不改 NR 逻辑、不改 pColor/pOutput 内容、不碰游戏自己的交换链
//   · 用「合成交换链 + DComp 视觉」贴在游戏窗口的 DComp 目标上：不建独立窗口
//     → 不抢焦点、不进 alt-tab（present_probe4 已用绿屏验证过这条路可行）
//   · 节奏：本帧把成品拷进后备缓冲，下一帧才 Present（保证每次翻上去的都是写完的缓冲；
//     代价 +1 帧，对"合成代价"的测量没有影响）
//   · 任何一步失败 → 永久关闭本功能，游戏照常
// 开关：ini 键 mirror=1 / 菜单「镜像叠层」/ F11
// =====================================================================================
static ComPtr<IDCompositionDevice>         g_mirDev;
static ComPtr<IDCompositionTarget>         g_mirTarget;
static ComPtr<IDCompositionVisual>         g_mirVisual;
static ComPtr<IDCompositionScaleTransform> g_mirScale;
static ComPtr<IDXGISwapChain3>             g_mirSc;    // 需要 3 才有 SetColorSpace1 / GetCurrentBackBufferIndex
static bool g_mirPending = false;
static UINT g_mirW = 0, g_mirH = 0;

// 合成交换链只接受少数格式（本机实测：R8G8B8A8_UNORM 可用，R10G10B10A2_UNORM 直接
// 返回 DXGI_ERROR_INVALID_CALL）。所以两条路：
//   ① 交换链格式 == outp 格式 → CopyResource（零 shader，最省）
//   ② 否则退回 R8G8B8A8_UNORM → 一次全屏 blit（outp 当 SRV 采样，格式自动转换）
static bool  g_mirBlit = false;
static int   g_mirMode = 0;        // 1=DComp 合成视觉  2=自建置顶穿透窗口
static HWND  g_mirWnd  = nullptr;
// ★ D3D12 的 CreateSwapChain* 要求的 pDevice 是【命令队列】，不是 ID3D12Device
//   （传设备会稳定返回 DXGI_ERROR_INVALID_CALL 0x887A0001）
static ComPtr<ID3D12CommandQueue>   g_mirQueue;
// 阶段一（纯色块）：自己的分配器/列表/fence，全程自己的队列，绝不跨队列碰游戏的后备缓冲
static ComPtr<ID3D12CommandAllocator>      g_mirAlloc[2];
static ComPtr<ID3D12GraphicsCommandList>   g_mirList;
static ComPtr<ID3D12Fence>                 g_mirFence;
static UINT64 g_mirFenceVal = 0;
static UINT64 g_mirAllocFence[2] = { 0, 0 };   // 每个分配器上次被使用时的 fence 值
static int    g_mirAllocIdx = 0;
static bool   g_mirSolid = true;    // true=只画纯色（阶段一，安全） false=贴真画面（阶段二）
// 阶段二：outp 的快照（3 槽，游戏队列写 w，我们的队列只读 2 帧前那张 → 永不读写同一张）
static ComPtr<ID3D12Resource> g_mirSnap[3];
static UINT g_mirSnapW = 0, g_mirSnapH = 0;
static DXGI_FORMAT g_mirSnapFmt = DXGI_FORMAT_UNKNOWN;
static int  g_mirSnapWrite = 0;
static int  g_mirGrabCnt = 0;
static DXGI_FORMAT g_mirSwapFmt = DXGI_FORMAT_UNKNOWN;
static ComPtr<ID3D12RootSignature>  g_mirRs;
static ComPtr<ID3D12PipelineState>  g_mirPso;
static ComPtr<ID3D12DescriptorHeap> g_mirSrvHeap;
static ComPtr<ID3D12DescriptorHeap> g_mirRtvHeap;
static UINT  g_mirIncSrv = 0, g_mirIncRtv = 0;

static const char* kMirHLSL = R"(
Texture2D<float4> gSrc : register(t0);
SamplerState      gSmp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
float4 PSMain(VSOut i) : SV_Target { return gSrc.Sample(gSmp, i.uv); }
)";

// ---- 备选路径：自建置顶穿透窗口 + CreateSwapChainForHwnd（合成交换链走不通时用）----
// 社区先例：Magpie / Lossless Scaling 双卡都是这条。代价：多一个窗口对象（但点击穿透、不进 alt-tab）
static LRESULT CALLBACK MirrorWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) return 0;
    return DefWindowProcW(h, m, w, l);
}

static bool MirrorBuildOwnWindow(IDXGIFactory2* fac, DXGI_FORMAT fmt) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MirrorWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CrossGpuNrMirrorWnd";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("MIR: RegisterClassExW 失败 %lu", GetLastError()); return false;
    }
    if (!g_mirWnd) {
        g_mirWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
            L"CrossGpuNrMirrorWnd", L"", WS_POPUP,
            0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_mirWnd) { Log("MIR: CreateWindowExW 失败 %lu", GetLastError()); return false; }
    }
    RECT cr{}; GetClientRect(g_gameHwnd, &cr);
    POINT tl{ 0, 0 }; ClientToScreen(g_gameHwnd, &tl);
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    if (cw < 8 || ch < 8) { cw = (int)g_mirW; ch = (int)g_mirH; }
    SetWindowPos(g_mirWnd, HWND_TOPMOST, tl.x, tl.y, cw, ch, SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_mirW; sd.Height = g_mirH; sd.Format = fmt;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = fac->CreateSwapChainForHwnd(g_mirQueue.Get(), g_mirWnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr) || !sc1) { Log("MIR: CreateSwapChainForHwnd(自建窗口) 失败 0x%08X", (unsigned)hr); return false; }
    hr = sc1.As(&g_mirSc);
    if (FAILED(hr) || !g_mirSc) { Log("MIR: QI IDXGISwapChain3(自建窗口) 失败 0x%08X", (unsigned)hr); return false; }
    g_mirMode = 2;
    Log("MIR: 自建窗口就绪 | 交换链 %dx%d | 窗口 %dx%d @ (%ld,%ld)",
        (int)g_mirW, (int)g_mirH, cw, ch, tl.x, tl.y);
    return true;
}

static bool MirrorBuildBlit(ID3D12Device* dev, DXGI_FORMAT fmt) {    D3D12_DESCRIPTOR_RANGE r{};
    r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r.NumDescriptors = 1; r.BaseShaderRegister = 0; r.RegisterSpace = 0;
    r.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 1;
    p.DescriptorTable.pDescriptorRanges = &r;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ss.ShaderRegister = 0; ss.RegisterSpace = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1; rs.pParameters = &p;
    rs.NumStaticSamplers = 1; rs.pStaticSamplers = &ss;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) { Log("MIR: 序列化根签名失败 0x%08X %s", (unsigned)hr, err ? (char*)err->GetBufferPointer() : ""); return false; }
    hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_mirRs));
    if (FAILED(hr)) { Log("MIR: CreateRootSignature 失败 0x%08X", (unsigned)hr); return false; }

    ComPtr<ID3DBlob> vs, ps, e2;
    hr = D3DCompile(kMirHLSL, strlen(kMirHLSL), "mir.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &e2);
    if (FAILED(hr)) { Log("MIR: 编译 VS 失败 %s", e2 ? (char*)e2->GetBufferPointer() : ""); return false; }
    hr = D3DCompile(kMirHLSL, strlen(kMirHLSL), "mir.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &e2);
    if (FAILED(hr)) { Log("MIR: 编译 PS 失败 %s", e2 ? (char*)e2->GetBufferPointer() : ""); return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_mirRs.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_mirPso));
    if (FAILED(hr)) { Log("MIR: CreateGraphicsPipelineState 失败 0x%08X", (unsigned)hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_mirSrvHeap));
    if (FAILED(hr)) { Log("MIR: 建 SRV 堆失败 0x%08X", (unsigned)hr); return false; }
    g_mirIncSrv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // 注意：RTV 堆由 MirrorBuildRtv 负责，这里【绝对不能】再建一次 ——
    //   重建会得到一个「没有 RTV 视图」的堆，纯色帧随即拿未初始化描述符去清屏 → 设备挂死（已崩过一次）
    return true;
}

// RTV 堆与后备缓冲视图（纯色块模式也要用，所以单独建、总是建）
static bool MirrorBuildRtv(ID3D12Device* dev) {
    if (!g_mirRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 4;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_mirRtvHeap));
        if (FAILED(hr) || !g_mirRtvHeap) { Log("MIR: 建 RTV 堆失败 0x%08X", (unsigned)hr); return false; }
        g_mirIncRtv = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_mirRtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < 3; ++i) {
            ComPtr<ID3D12Resource> b;
            if (FAILED(g_mirSc->GetBuffer(i, IID_PPV_ARGS(&b))) || !b) { Log("MIR: 取后备缓冲 %u 失败", i); return false; }
            dev->CreateRenderTargetView(b.Get(), nullptr, h);
            h.ptr += g_mirIncRtv;
        }
    }
    return true;
}

// 我们自己的提交设施（纯色块模式全程用自己的队列，绝不跨队列碰游戏的后备缓冲）
static bool MirrorBuildSubmit(ID3D12Device* dev) {
    for (int i = 0; i < 2; ++i) {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_mirAlloc[i])))) {
            Log("MIR: CreateCommandAllocator %d 失败", i); return false;
        }
    }
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_mirAlloc[0].Get(), nullptr, IID_PPV_ARGS(&g_mirList)))) {
        Log("MIR: CreateCommandList 失败"); return false;
    }
    g_mirList->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_mirFence)))) {
        Log("MIR: CreateFence 失败"); return false;
    }
    g_mirFenceVal = 0;
    return true;
}

static bool MirrorBuild(ID3D12Resource* outp) {
    // 游戏窗口句柄只在「打开过菜单」时才会被填 → 这里自己补找一次
    if (!g_gameHwnd) g_gameHwnd = FindGameWindowOnce();
    if (!g_dev || !outp || !g_gameHwnd) {
        Log("MIR: 还缺东西（dev=%d outp=%d 窗口=%d）→ 本次跳过，可再试",
            g_dev ? 1 : 0, outp ? 1 : 0, g_gameHwnd ? 1 : 0);
        g_mirState = 3;          // 3 = 可重试（不永久放弃）
        return false;
    }
    // 重建前：先有界等待自己的 GPU 工作落地（≤200ms），否则释放仍在飞行中的对象会挂死
    if (g_mirFence && g_mirQueue && g_mirFenceVal) {
        if (g_mirFence->GetCompletedValue() < g_mirFenceVal) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev) {
                if (SUCCEEDED(g_mirFence->SetEventOnCompletion(g_mirFenceVal, ev)))
                    WaitForSingleObject(ev, 200);
                CloseHandle(ev);
            }
        }
    }
    // 重建前先彻底释放（同一个 HWND 反复建 target 不保险）
    g_mirSc.Reset(); g_mirVisual.Reset(); g_mirScale.Reset(); g_mirTarget.Reset(); g_mirDev.Reset();
    g_mirQueue.Reset();
    g_mirPending = false;

    D3D12_RESOURCE_DESC od = outp->GetDesc();
    g_mirW = (UINT)od.Width; g_mirH = od.Height;

    // ★ 两个坑都在这里：
    //   ① ID3D12Device【不实现 IDXGIDevice】（那是 D3D11 的用法，QI 会返回 E_NOINTERFACE）
    //      → 只能用 GetAdapterLuid() 反查 DXGI 适配器
    //   ② 交换链工厂必须来自该设备所在的那张卡，否则 CreateSwapChainForComposition
    //      返回 DXGI_ERROR_INVALID_CALL(0x887A0001)（本机两张卡，默认工厂常常不是游戏那张）
    HRESULT hr = S_OK;
    LUID devLuid = g_dev->GetAdapterLuid();

    ComPtr<IDXGIFactory4> fac4;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&fac4));
    if (FAILED(hr) || !fac4) { Log("MIR: CreateDXGIFactory2 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }

    ComPtr<IDXGIAdapter1> mirAd;
    hr = fac4->EnumAdapterByLuid(devLuid, IID_PPV_ARGS(&mirAd));
    if (FAILED(hr) || !mirAd) { Log("MIR: EnumAdapterByLuid 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    DXGI_ADAPTER_DESC1 adDesc{};
    if (SUCCEEDED(mirAd->GetDesc1(&adDesc)))
        Log("MIR: 设备所在卡 = %ls (LUID %08X:%08X)", adDesc.Description,
            (unsigned)adDesc.AdapterLuid.HighPart, (unsigned)adDesc.AdapterLuid.LowPart);

    ComPtr<IDXGIFactory2> fac;
    hr = mirAd->GetParent(IID_PPV_ARGS(&fac));
    if (FAILED(hr) || !fac) { Log("MIR: 取适配器父工厂失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width  = g_mirW;
    sd.Height = g_mirH;
    sd.Format = od.Format;                 // 必须与 outp 一致（CopyResource 要求同格式同尺寸）
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.Scaling     = DXGI_SCALING_STRETCH;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;   // 不透明：整块盖住
    // 建我们自己的命令队列：CreateSwapChain* 的 pDevice 必须是队列
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_mirQueue));
        if (FAILED(hr) || !g_mirQueue) { Log("MIR: CreateCommandQueue 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    }

    // 先按 outp 的原生格式建（能成就能零 shader 直拷）；不成再退回 RGBA8 + blit
    ComPtr<IDXGISwapChain1> sc1;
    hr = fac->CreateSwapChainForComposition(g_mirQueue.Get(), &sd, nullptr, &sc1);
    g_mirBlit = false;
    if (FAILED(hr) || !sc1) {
        Log("MIR: 合成交换链不接受 fmt=%d（0x%08X）→ 退回 R8G8B8A8_UNORM + blit", (int)od.Format, (unsigned)hr);
        sc1.Reset();
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = fac->CreateSwapChainForComposition(g_mirQueue.Get(), &sd, nullptr, &sc1);
        g_mirBlit = true;
        if (FAILED(hr) || !sc1) {
            Log("MIR: RGBA8 合成交换链也失败 0x%08X（%ux%u）", (unsigned)hr, g_mirW, g_mirH);
            // 诊断：把环境状态钉死，便于判断是不是 DWM/独占全屏的问题
            {
                RECT wr{}; GetWindowRect(g_gameHwnd, &wr);
                RECT cr2{}; GetClientRect(g_gameHwnd, &cr2);
                RECT mr{}; GetWindowRect(GetDesktopWindow(), &mr);
                DWORD st = (DWORD)GetWindowLongPtrW(g_gameHwnd, GWL_STYLE);
                DWORD ex = (DWORD)GetWindowLongPtrW(g_gameHwnd, GWL_EXSTYLE);
                Log("MIR: 诊断 窗口=(%ld,%ld)-(%ld,%ld) 客户区=%ldx%ld 桌面=%ldx%ld style=0x%08X ex=0x%08X",
                    wr.left, wr.top, wr.right, wr.bottom,
                    cr2.right - cr2.left, cr2.bottom - cr2.top,
                    mr.right - mr.left, mr.bottom - mr.top, st, ex);
            }
            g_mirBlit = true;
            if (!MirrorBuildOwnWindow(fac.Get(), DXGI_FORMAT_R8G8B8A8_UNORM)) { g_mirState = 2; return false; }
            if (!MirrorBuildRtv(g_dev.Get())) { g_mirState = 2; return false; }
            if (!MirrorBuildSubmit(g_dev.Get())) { g_mirState = 2; return false; }
            if (!MirrorBuildBlit(g_dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM)) { g_mirState = 2; return false; }
            g_mirState = 1;
            Log("MIR: 就绪 | 模式 自建窗口+blit | 输出 %ux%u", g_mirW, g_mirH);
            return true;
        }
    }
    hr = sc1.As(&g_mirSc);
    if (FAILED(hr) || !g_mirSc) { Log("MIR: QI IDXGISwapChain3 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    g_mirSc->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);   // SDR；失败不致命
    if (!MirrorBuildRtv(g_dev.Get())) { g_mirState = 2; return false; }
    if (!MirrorBuildSubmit(g_dev.Get())) { g_mirState = 2; return false; }

    // DComp 设备：D3D12 设备不能当 IDXGIDevice 传进来。
    // 先试 nullptr（默认 DXGI 设备）；失败则在【同一张卡】上造一个 D3D11 设备来提供 IDXGIDevice。
    hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&g_mirDev));
    if (FAILED(hr) || !g_mirDev) {
        Log("MIR: DCompositionCreateDevice(nullptr) 失败 0x%08X → 改走同卡 D3D11 设备", (unsigned)hr);
        ComPtr<ID3D11Device> d11;
        D3D_FEATURE_LEVEL fl{};
        HRESULT h2 = D3D11CreateDevice(mirAd.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                       nullptr, 0, D3D11_SDK_VERSION, &d11, &fl, nullptr);
        ComPtr<IDXGIDevice> d11Dxgi;
        if (FAILED(h2) || !d11 || FAILED(d11.As(&d11Dxgi)) || !d11Dxgi) {
            Log("MIR: 建 D3D11 设备/取 IDXGIDevice 失败 0x%08X", (unsigned)h2);
            g_mirState = 2; return false;
        }
        hr = DCompositionCreateDevice(d11Dxgi.Get(), IID_PPV_ARGS(&g_mirDev));
        if (FAILED(hr) || !g_mirDev) {
            Log("MIR: DCompositionCreateDevice(D3D11) 失败 0x%08X", (unsigned)hr);
            g_mirState = 2; return false;
        }
    }

    hr = g_mirDev->CreateTargetForHwnd(g_gameHwnd, TRUE, &g_mirTarget);   // TRUE = 在窗口内容之上
    if (FAILED(hr) || !g_mirTarget) { Log("MIR: CreateTargetForHwnd 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    hr = g_mirDev->CreateVisual(&g_mirVisual);
    if (FAILED(hr) || !g_mirVisual) { Log("MIR: CreateVisual 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }

    g_mirVisual->SetContent(g_mirSc.Get());

    // 铺满客户区：输出分辨率 → 客户区 的等比缩放（客户区=4K 时就是 1:1）
    RECT rc{}; GetClientRect(g_gameHwnd, &rc);
    float cw = (float)(rc.right - rc.left), ch = (float)(rc.bottom - rc.top);
    float sx = 1.0f, sy = 1.0f;
    if (cw > 1 && ch > 1 && g_mirW && g_mirH) {
        sx = cw / (float)g_mirW; sy = ch / (float)g_mirH;
        if (SUCCEEDED(g_mirDev->CreateScaleTransform(&g_mirScale)) && g_mirScale) {
            g_mirScale->SetScaleX(sx);
            g_mirScale->SetScaleY(sy);
            g_mirScale->SetCenterX(0.0f);
            g_mirScale->SetCenterY(0.0f);
            g_mirVisual->SetTransform(g_mirScale.Get());
        }
    }

    hr = g_mirTarget->SetRoot(g_mirVisual.Get());
    if (FAILED(hr)) { Log("MIR: SetRoot 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }
    hr = g_mirDev->Commit();
    if (FAILED(hr)) { Log("MIR: Commit 失败 0x%08X", (unsigned)hr); g_mirState = 2; return false; }

    if (g_mirBlit) {
        if (!MirrorBuildBlit(g_dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM)) { g_mirState = 2; return false; }
    } else {
        // 直拷模式下也要备好 blit 管线（阶段二贴真画面要用，目标格式 = 交换链实际格式）
        if (!MirrorBuildBlit(g_dev.Get(), sd.Format)) { g_mirState = 2; return false; }
    }
    g_mirSwapFmt = sd.Format;

    g_mirMode = 1;
    g_mirState = 1;
    Log("MIR: 就绪 | 输出 %ux%u fmt=%d | 模式 %s | 客户区 %dx%d | 缩放 %.3f x %.3f",
        g_mirW, g_mirH, (int)od.Format, g_mirBlit ? "RGBA8+blit" : "直拷", (int)cw, (int)ch, sx, sy);
    return true;
}

// 文件开关：xess_proxy.mirror.txt 里写 1/0，每 ~30 帧读一次（纯 CPU，不碰 GPU）
// 用途：让外部脚本（DSH）能在不碰键盘的情况下做 A/B
static void MirrorPollFile() {
    static int s_last = -1;
    std::string p = ModuleDir() + "xess_proxy.mirror.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "r") != 0 || !f) return;
    char buf[16] = { 0 };
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (got == 0) return;
    int v = -1;
    for (size_t i = 0; i < got; ++i) {
        if (buf[i] == '0') { v = 0; break; }
        if (buf[i] == '1') { v = 1; break; }
        if (buf[i] == '2') { v = 2; break; }   // 2 = 纯色自检
    }
    if (v < 0 || v == s_last) return;
    s_last = v;
    bool wantOn = (v != 0);
    bool wantSolid = (v == 2);
    if (wantOn != g_mirOn || (wantOn && wantSolid != g_mirSolid)) {
        g_mirOn = wantOn;
        if (wantOn) g_mirSolid = wantSolid;
        Log("MIR: 文件开关 -> %s%s（帧 %d）", wantOn ? "开" : "关", wantOn ? (wantSolid ? "·纯色" : "·真画面") : "", g_frame);
    }
}

// ---- 阶段二：真画面 ----
// 分工：游戏队列只负责 outp → 快照；我们的队列只负责 快照 → 交换链 → Present。
// 两边都不碰对方飞行中的资源；快照 3 槽，读的是 2 帧前那张。
static bool MirrorEnsureSnap(ID3D12Device* dev, ID3D12Resource* outp) {
    if (!dev || !outp) return false;
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    if (g_mirSnap[0] && (UINT)od.Width == g_mirSnapW && od.Height == g_mirSnapH && od.Format == g_mirSnapFmt) return true;
    for (int i = 0; i < 3; ++i) g_mirSnap[i].Reset();
    g_mirGrabCnt = 0; g_mirSnapWrite = 0;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = od;
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.MipLevels = 1;
    rd.DepthOrArraySize = 1;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    for (int i = 0; i < 3; ++i) {
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_mirSnap[i])))) {
            Log("MIR: 建快照纹理失败（第 %d 张）", i);
            for (int k = 0; k < 3; ++k) g_mirSnap[k].Reset();
            return false;
        }
    }
    g_mirSnapW = (UINT)od.Width; g_mirSnapH = od.Height; g_mirSnapFmt = od.Format;
    Log("MIR: 快照纹理 %ux%u fmt=%d ×3", g_mirSnapW, g_mirSnapH, (int)g_mirSnapFmt);
    return true;
}

// 记在【游戏的命令列表】上：outp → 快照[w]
static void MirrorGrab(ID3D12GraphicsCommandList* cl, ID3D12Resource* outp) {
    if (!cl || !outp) return;
    if (!MirrorEnsureSnap(g_dev.Get(), outp)) return;
    int w = g_mirSnapWrite;
    Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cl, g_mirSnap[w].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(g_mirSnap[w].Get(), outp);
    Barrier(cl, g_mirSnap[w].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    g_mirSnapWrite = (w + 1) % 3;
    if (g_mirGrabCnt < 100) ++g_mirGrabCnt;
}

// 在【我们自己的队列】上：快照[w+2] → 交换链 → Present
static void MirrorPresentReal() {
    if (g_mirGrabCnt < 2) return;
    if (!g_mirSc || !g_mirQueue || !g_mirList || !g_mirFence || !g_mirRtvHeap || !g_mirPso || !g_mirSrvHeap) return;
    UINT64 done = g_mirFence->GetCompletedValue();
    int ai = -1;
    for (int k = 0; k < 2; ++k) { int i = (g_mirAllocIdx + k) % 2; if (done >= g_mirAllocFence[i]) { ai = i; break; } }
    if (ai < 0) return;
    g_mirAllocIdx = (ai + 1) % 2;
    auto& alloc = g_mirAlloc[ai];
    if (FAILED(alloc->Reset())) return;
    if (FAILED(g_mirList->Reset(alloc.Get(), nullptr))) return;

    int w = g_mirSnapWrite;                 // 本帧写入的槽
    int si = (w + 2) % 3;                   // 读 2 帧前那张（Grab 后再看，等价于上一帧写入的那张）
    if (!g_mirSnap[si]) { g_mirList->Close(); return; }

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = g_mirSnapFmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(g_mirSnap[si].Get(), &sv, g_mirSrvHeap->GetCPUDescriptorHandleForHeapStart());

    UINT bi = g_mirSc->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    if (FAILED(g_mirSc->GetBuffer(bi, IID_PPV_ARGS(&bb))) || !bb) { g_mirList->Close(); return; }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_mirRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)bi * g_mirIncRtv;

    Barrier(g_mirList.Get(), bb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12DescriptorHeap* hs[] = { g_mirSrvHeap.Get() };
    g_mirList->SetDescriptorHeaps(1, hs);
    g_mirList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_mirList->SetGraphicsRootSignature(g_mirRs.Get());
    g_mirList->SetGraphicsRootDescriptorTable(0, g_mirSrvHeap->GetGPUDescriptorHandleForHeapStart());
    g_mirList->SetPipelineState(g_mirPso.Get());
    g_mirList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT vp{ 0, 0, (float)g_mirW, (float)g_mirH, 0.0f, 1.0f };
    D3D12_RECT rcs{ 0, 0, (LONG)g_mirW, (LONG)g_mirH };
    g_mirList->RSSetViewports(1, &vp);
    g_mirList->RSSetScissorRects(1, &rcs);
    g_mirList->DrawInstanced(3, 1, 0, 0);
    Barrier(g_mirList.Get(), bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

    if (FAILED(g_mirList->Close())) return;
    ID3D12CommandList* ls[] = { g_mirList.Get() };
    g_mirQueue->ExecuteCommandLists(1, ls);
    UINT64 v = ++g_mirFenceVal;
    g_mirQueue->Signal(g_mirFence.Get(), v);
    g_mirAllocFence[ai] = v;
    g_mirSc->Present(0, 0);
    if ((g_frame % 300) == 0) Log("MIR: 真画面帧 %d（快照槽 %d）", g_frame, si);
}

// 阶段一：只清一块纯色。完全不读 outp、不碰游戏队列 → 排除资源状态与跨队列 Present 两个风险
static void MirrorSolidFrame() {
    if (!g_mirSc || !g_mirQueue || !g_mirList || !g_mirFence || !g_mirRtvHeap) return;
    // 只挑一个「GPU 已经用完」的分配器；两个都在飞就本帧跳过（绝不阻塞、绝不复用飞行中的对象）
    UINT64 done = g_mirFence->GetCompletedValue();
    int idx = -1;
    for (int k = 0; k < 2; ++k) {
        int i = (g_mirAllocIdx + k) % 2;
        if (done >= g_mirAllocFence[i]) { idx = i; break; }
    }
    if (idx < 0) return;
    g_mirAllocIdx = (idx + 1) % 2;
    auto& alloc = g_mirAlloc[idx];
    if (FAILED(alloc->Reset())) return;
    if (FAILED(g_mirList->Reset(alloc.Get(), nullptr))) return;
    UINT bi = g_mirSc->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    if (FAILED(g_mirSc->GetBuffer(bi, IID_PPV_ARGS(&bb))) || !bb) { g_mirList->Close(); return; }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_mirRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)bi * g_mirIncRtv;
    Barrier(g_mirList.Get(), bb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float col[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    g_mirList->ClearRenderTargetView(rtv, col, 0, nullptr);
    Barrier(g_mirList.Get(), bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(g_mirList->Close())) return;
    ID3D12CommandList* ls[] = { g_mirList.Get() };
    g_mirQueue->ExecuteCommandLists(1, ls);
    UINT64 v = ++g_mirFenceVal;
    g_mirQueue->Signal(g_mirFence.Get(), v);
    g_mirAllocFence[idx] = v;
    g_mirSc->Present(0, 0);
    if ((g_frame % 300) == 0) Log("MIR: 纯色帧 %d（%ux%u）", g_frame, g_mirW, g_mirH);
}

static void MirrorTick(ID3D12GraphicsCommandList* cl, ID3D12Resource* outp) {
    static int s_poll = 0;
    if ((s_poll++ % 30) == 0) MirrorPollFile();        // 文件开关（外部 A/B 用）

    // 每次「关→开」都允许重新尝试（否则一次失败就永久死了，见 22471 帧那次）
    static bool s_prevOn = false;
    if (g_mirOn && !s_prevOn && g_mirState == 2) { g_mirState = 0; Log("MIR: 重新开启，重试初始化"); }
    s_prevOn = g_mirOn;
    // 关闭态：把视觉内容摘掉（叠层消失，游戏照常出画），对象留着以便随时再开
    if (!g_mirOn) {
        if (g_mirState == 1) {
            if (g_mirVisual) g_mirVisual->SetContent(nullptr);
            if (g_mirDev) g_mirDev->Commit();
            if (g_mirWnd) ShowWindow(g_mirWnd, SW_HIDE);
            g_mirPending = false;
            g_mirState = 3;
            Log("MIR: 已关（叠层撤掉；F11 / 菜单可再开）");
        }
        return;
    }
    // ★ 关键：已经建好、只是被藏起来（state=3）时，【只挂回去】，绝不重建。
    //   重建要 Reset 交换链/队列/分配器，而上一轮的 GPU 工作可能还在飞 → 会挂死（已闪退一次）。
    if (g_mirState == 3 && g_mirSc) {
        if (g_mirMode == 1 && g_mirVisual) {
            g_mirVisual->SetContent(g_mirSc.Get());
            if (g_mirDev) g_mirDev->Commit();
        }
        if (g_mirMode == 2 && g_mirWnd) ShowWindow(g_mirWnd, SW_SHOWNOACTIVATE);
        g_mirState = 1;
        Log("MIR: 重新挂上（沿用已建好的交换链，未重建）");
    }
    if (g_mirState == 2) return;                       // 失败过，不再重试
    if (g_mirState != 1) { if (!MirrorBuild(outp)) return; }
    if (g_mirSolid) { MirrorSolidFrame(); return; }    // 阶段一：纯色块
    if (!cl || !outp) return;
    MirrorGrab(cl, outp);                              // 游戏队列：outp → 快照
    MirrorPresentReal();                               // 我们的队列：快照 → 交换链 → Present
    return;

    // 分辨率变了（切 XeSS 档位/窗口变化）→ 下一帧重建
    D3D12_RESOURCE_DESC od = outp->GetDesc();
    if ((UINT)od.Width != g_mirW || od.Height != g_mirH) {
        Log("MIR: 输出尺寸 %llux%u → %ux%u，准备重建", (unsigned long long)g_mirW, g_mirH, (UINT)od.Width, od.Height);
        if (g_mirVisual) g_mirVisual->SetContent(nullptr);
        g_mirState = 3; return;
    }

    // ① 先把上一帧已经写好的缓冲翻上去（纯 CPU 调用，不进命令流）
    if (g_mirMode == 2 && g_mirWnd) {          // 自建窗口：抽消息 + 保证可见
        MSG msg;
        while (PeekMessageW(&msg, g_mirWnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!IsWindowVisible(g_mirWnd)) ShowWindow(g_mirWnd, SW_SHOWNOACTIVATE);
    }
    if (g_mirPending) { g_mirSc->Present(0, 0); g_mirPending = false; }

    // ② 把本帧成品拷进当前后备缓冲（下一帧才 Present）
    ComPtr<ID3D12Resource> bb;
    if (FAILED(g_mirSc->GetBuffer(g_mirSc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb))) || !bb) return;

    if (!g_mirBlit) {
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, bb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(bb.Get(), outp);
        Barrier(cl, bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, outp, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    } else {
        // 格式不同（outp 的 fmt ≠ 交换链 fmt）→ 全屏 blit，顺便完成格式转换
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = od.Format;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        g_dev->CreateShaderResourceView(outp, &sv, g_mirSrvHeap->GetCPUDescriptorHandleForHeapStart());

        UINT bi = g_mirSc->GetCurrentBackBufferIndex();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_mirRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)bi * g_mirIncRtv;

        Barrier(cl, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cl, bb.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);

        ID3D12DescriptorHeap* mh[] = { g_mirSrvHeap.Get() };
        cl->SetDescriptorHeaps(1, mh);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cl->SetGraphicsRootSignature(g_mirRs.Get());
        cl->SetGraphicsRootDescriptorTable(0, g_mirSrvHeap->GetGPUDescriptorHandleForHeapStart());
        cl->SetPipelineState(g_mirPso.Get());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT vp{ 0, 0, (float)g_mirW, (float)g_mirH, 0.0f, 1.0f };
        D3D12_RECT rcs{ 0, 0, (LONG)g_mirW, (LONG)g_mirH };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &rcs);
        cl->DrawInstanced(3, 1, 0, 0);

        Barrier(cl, bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cl, outp, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

        ID3D12DescriptorHeap* gh[] = { g_heap.Get() };     // 还原我们自己的堆（下面还要用）
        cl->SetDescriptorHeaps(1, gh);
    }
    g_mirPending = true;

    if ((g_frame % 300) == 0) Log("MIR: 帧 %d 已叠层 %ux%u（提交 %.2fms 参考）", g_frame, g_mirW, g_mirH, 0.0);
}

static DWORD WINAPI HotkeyThread(LPVOID) {
    bool p10 = false;
    while (true) {
        static bool pDel = false;
        bool dDel = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (dDel && !pDel) {
            g_menuOpen = !g_menuOpen;
            if (g_menuOpen) { EnsureWndHook(); while (ShowCursor(TRUE) < 0) {} ClipCursor(nullptr); }
            else { while (ShowCursor(FALSE) >= 0) {} }
            Log("*** Del: 参数菜单 %s ***", g_menuOpen ? "开" : "关");
        }
        pDel = dDel;
        bool d10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (d10 && !p10) { g_nrOn = !g_nrOn; Log("*** F10: NR %s（帧 %d）***", g_nrOn ? "开" : "关", (int)g_frame); }
        p10 = d10;
        static bool p11 = false;
        bool d11 = false;   // 已停用（改在菜单里调）：VK_F11
        if (d11 && !p11) { g_mirOn = !g_mirOn; Log("*** F11: 镜像叠层 %s ***", g_mirOn ? "开" : "关"); }
        // ★ 小键盘 1（NumLock 关）：游戏内热切换 A 形态 ↔ B-4K 形态（改 bform.txt，轮询器 ~1 秒接手）
        static bool pHk9 = false;
        bool dHk9 = ((GetAsyncKeyState(VK_END) & 0x8000) != 0) && ((GetKeyState(VK_NUMLOCK) & 1) == 0);   // 小键盘 1（NumLock 关）
        if (dHk9 && !pHk9) {
            std::string path = ModuleDir() + "xess_proxy.bform.txt";
            const char* want = g_bform4k ? "0" : "9";
            FILE* f9 = nullptr;
            if (fopen_s(&f9, path.c_str(), "w") == 0 && f9) { fwrite(want, 1, 1, f9); fclose(f9); }
            Log("*** 小键盘1(NumLock关): 请求切换到 %s 形态（%s）***", want[0] == 48 ? "A" : "B-4K",
                want[0] == 48 ? "NR@1440p + 真 XeSS + 游戏 present" : "真 XeSS 4K + B 端 4K NR + B present");
        }
        pHk9 = dHk9;
        p11 = d11;
        static bool p7 = false, p8 = false;
        bool d7 = false;   // 已停用（改在菜单里调）：VK_F7
        bool d8 = false;   // 已停用（改在菜单里调）：VK_F8
        if (d7 && !p7) { g_gain = (g_gain > 0.30f) ? g_gain - 0.06f : 0.24f; Log("*** F7: 残差增益 %.2f ***", g_gain); }
        if (d8 && !p8) { g_gain = (g_gain < 1.20f) ? g_gain + 0.06f : 1.26f; Log("*** F8: 残差增益 %.2f ***", g_gain); }
        p7 = d7; p8 = d8;
        static bool p9 = false;
        // F9 已交给 REFramework 开/关窗口 —— 我们不再占用（平滑改到菜单里调）
        p9 = false;
        static bool p5 = false, p6 = false;
        bool d5 = false;   // 已停用（改在菜单里调）：VK_F5
        bool d6 = false;   // 已停用（改在菜单里调）：VK_F6
        if (d5 && !p5) {
            static int m5 = 0;
            m5 = (m5 + 1) % 8;
            if (m5 == 0)      { g_temporalStatic = 1.0f;  g_stateWarmup = 40; Log("*** F5[0]: 时域稳定 关 ***"); }
            else if (m5 == 1) { g_temporalStatic = 0.60f; g_stateWarmup = 40; Log("*** F5[1]: 0.60 ***"); }
            else if (m5 == 2) { g_temporalStatic = 0.35f; g_stateWarmup = 40; Log("*** F5[2]: 0.35 ***"); }
            else if (m5 == 3) { g_temporalStatic = 0.02f; g_stateWarmup = 40; Log("*** F5[3]: 0.20 ***"); }
            else if (m5 == 4) { g_temporalStatic = 0.10f; g_stateWarmup = 40; Log("*** F5[4]: 0.10 ***"); }
            else if (m5 == 5) { g_temporalStatic = 0.05f; g_stateWarmup = 40; Log("*** F5[5]: 0.05 ***"); }
            else if (m5 == 6) { g_temporalStatic = 0.03f; g_stateWarmup = 40; Log("*** F5[6]: 0.03 ***"); }
            else              { g_temporalStatic = 0.02f; g_stateWarmup = 40; Log("*** F5[7]: 0.02（最强）***"); }
        }
        if (d6 && !p6) {
            static int m6 = 0;
            m6 = (m6 + 1) % 4;
            if (m6 == 0)      { g_varGain = 0.0f; Log("*** F6[0]: 方差门限 关 ***"); }
            else if (m6 == 1) { g_varGain = 0.5f; Log("*** F6[1]: 方差门限 0.5 ***"); }
            else if (m6 == 2) { g_varGain = 1.5f; Log("*** F6[2]: 方差门限 1.5 ***"); }
            else              { g_varGain = 4.0f; Log("*** F6[3]: 方差门限 4.0 ***"); }
        }
        p5 = d5; p6 = d6;
        static bool p3 = false, p4 = false;
        bool d3 = false;   // 已停用（改在菜单里调）：VK_F3
        bool d4 = false;   // 已停用（改在菜单里调）：VK_F4
        if (d3 && !p3) {
            static int m3 = 0; m3 = (m3 + 1) % 6;
            const float v[6] = { 1.0f, -1.0f, 0.5f, -0.5f, 0.00045f, -0.00045f };
            g_mvScale = v[m3]; g_mvScaleIdx = m3;
            Log("*** F3[%d]: MV 比例 %.5f ***", m3, g_mvScale);
        }
        if (d4 && !p4) {
            static int m4 = 0; m4 = (m4 + 1) % 4;
            const float v[4] = { 0.0f, 1.0f, 2.0f, 6.0f };
            g_mvConf = v[m4]; g_mvConfIdx = m4;
            Log("*** F4[%d]: MV 置信衰减 %.1f ***", m4, g_mvConf);
        }
        static bool p2 = false;
        bool d2 = false;   // 已停用（改在菜单里调）：VK_F2
        if (d2 && !p2) {
            static int m2 = 0; m2 = (m2 + 1) % 5;
            const float v[5] = { 0.0f, 2.0f, 6.0f, 15.0f, 40.0f };
            g_gateK = v[m2]; g_gateIdx = m2;
            Log("*** F2[%d]: 逐像素门限 %.1f（0=关）***", m2, g_gateK);
        }
        p2 = d2;
        p3 = d3; p4 = d4;
        Sleep(40);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        OpenLog();
        Log("libxess proxy v9（方案 B：槽位队列 + 延迟呈现）attached");
        // 模型倍率必须在最早期设好：运行时在建会话时读取并缓存，晚于它初始化再设无效
        // （所以只在 worker 里设是不够的）。这里只碰进程环境块，不做文件 I/O、不调 LoadLibrary。
        SetEnvironmentVariableW(L"DLSS5_MULTI_PASS", L"1");   // 1 = Original 71-block；0.5 = Small
        Log("early: 已设 DLSS5_MULTI_PASS=1（请求 Original 71-block）");
        {
            std::string off = ModuleDir() + "xess_proxy_off.txt";
            if (GetFileAttributesA(off.c_str()) != INVALID_FILE_ATTRIBUTES) {
                g_disabled = true;
                Log("检测到 xess_proxy_off.txt：不接管");
            }
        }
        InitForwarders();
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
        Log("转发就绪");
    }
    return TRUE;
}
