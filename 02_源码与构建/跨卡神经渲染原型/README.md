# 跨卡神经渲染原型（7900 XT 渲染 + 9070 XT 跑 NR）

> 目标：游戏在 **RX 7900 XT** 上渲染，神经网络渲染（AMD 侧 NR）在 **RX 9070 XT** 上执行，
> 帧经 CPU 中转跨卡传输（AMD 不支持跨适配器直连共享，见第三节）。

## 零、最终实测结论（2026-10-24，全部已跑通并复现）

| 分辨率 | 整帧串行 | 网络段 | 传输段 | 串行 fps | 流水线 fps（重叠） |
|---|---|---|---|---|---|
| 1920×1080 | **14.83 ms** | 9.98 ms | 4.73 ms | **67.4** | **99.6** |
| 1280×720 | **8.32 ms** | 5.92 ms | 2.45 ms | **120.2** | 182 |

对照：游戏内同卡串行（danielblnc 17.1ms）约 **37 fps**。

**复现命令：**

```bat
build\Release\pipeline_loop.exe 1920 1080 20
```

## 一、为什么值得做（实测数据，2026-10-24 测得）

| 项 | 数值 |
|---|---|
| AMD 双卡 P2P | ❌ 不支持（`hipDeviceCanAccessPeer` 双向为 0） |
| 跨卡传输路径 | 主机内存中转（D2H + H2D） |
| 实测聚合带宽 | 约 **10~11 GB/s** |
| 1440p 每帧搬运（颜色+深度出、结果回，42MB） | **3.58 ms** |
| 1440p 不含深度（28MB） | 2.47 ms |
| 1080p 含深度（24MB） | 2.26 ms |
| NR 耗时（danielblnc，9070 XT） | **17.1 ms** |
| **流水线上限 1440p** | max(17.1, 3.58) → **约 58 fps** |
| 对照：同卡串行（现状） | 27 ms → **37 fps** |

结论：**传输只占 NR 的 1/5，瓶颈在 NR 本身**。跨卡理论收益 **+57%**（1440p）。
若游戏 1080p 渲染 + NR 1080p（约 9.6ms），上限可到约 **100 fps**。

## 二、判读阈值

`transport_bench` 跑出来的每帧往返时间：

- **< 4 ms** → 值得继续投入（目标 55~58 fps）
- **4 ~ 8 ms** → 收益被吃掉一半，仅 1080p 路线还有意义
- **> 8 ms** → 放弃自研，等 AMDNR 4.0

## 三、目录结构

```
跨卡神经渲染原型/
├─ CMakeLists.txt
├─ verify_symbols.ps1            # 符号校验：源码里每个 D3D12 符号是否存在于 SDK 头文件
├─ ref/LmxxfNrApi.h              # lmxxf 运行时 C ABI 头文件（抓自上游，注意与实际 DLL 有差异）
├─ src/
│  ├─ pipeline_loop.cpp          # ★ 阶段 C：完整闭环（出帧→中转→9070XT 推理→回传）
│  ├─ nr_lmxxf_test3.cpp         # ★ 阶段 B：在 9070 XT 上驱动 lmxxf 跑推理
│  ├─ transport_cpu.cpp          # 阶段 A：CPU 中转跨卡传输基准（可行路径）
│  ├─ transport_bench.cpp        # 阶段 A：跨卡直连版（证明 AMD 不支持）
│  ├─ share_probe.cpp            # 共享能力探针（挖出 CROSS_ADAPTER 不支持）
│  ├─ resource_probe.cpp         # 资源创建诊断（挖出 CPUPageProperty 必须 UNKNOWN）
│  ├─ api_probe.cpp / api_scan.cpp  # 探测运行时的 ABI 版本与结构体尺寸
│  └─ nr_lmxxf_test.cpp / test2.cpp # 早期版本（保留作为尺寸协商过程的记录）
└─ README.md
```

## 三点五、跑通闭环所需的 API 知识（上游文档里没有的）

| 项 | 文档/头文件说 | 实际 DLL（AMDNR 打包的 build） |
|---|---|---|
| `LmxxfNrApi` 表 | 144 字节 / 17 个函数指针 | **160 字节 / 19 个** |
| `LmxxfNrFrameInfo` | 104 字节 | **120 字节** |
| `assets_directory` | 传"资源目录" | **必须传 `.pak` 文件的完整路径** |
| 调用顺序 | 文档给的是 `EnqueueHip` | **用 `ExecuteAfterProducer`；`EnqueueHip` 会报 INVALID_ARGUMENT** |
| 网络输入上限 | 未说明 | **1920×1080**（1440p 要先缩放） |

正确每帧顺序：

```
PrepareFrame → RecordInputs → Close+Execute
→ ExecuteAfterProducer(ctx, job, queue)      # HIP 在这里跑完，状态直达 NR_COMPLETE
→ RecordOutputs → Close+Execute → Retire
```


后续阶段（尚未写）：

```
src/depth_tap.hlsl          渲染卡上把深度重采样成单平面 R32F 全尺寸
src/engine_hip.h/.cpp       在第二块卡上驱动 danielblnc / lmxxf 运行时
src/addon.cpp               ReShade .addon64：抓帧 → 跨卡 → 合成
```

## 四、构建

前置：Visual Studio Build Tools（C++ 工作负载 + Windows 11 SDK）+ CMake。

```bat
:: 在 "x64 Native Tools Command Prompt for VS" 里执行
cd /d E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物：`build\Release\transport_bench.exe`

## 五、运行

```bat
:: 默认 14MB payload，200 次往返
transport_bench.exe

:: 自定义：8MB payload，300 次
transport_bench.exe 8 300
```

输出示例（理想情况）：

```
adapter 0: AMD Radeon RX 7900 XT  (VRAM 20420 MB, LUID ...)
adapter 1: AMD Radeon RX 9070 XT  (VRAM 16191 MB, LUID ...)
devices created; A nodes=1 B nodes=1
shared cross-adapter buffer created on A
>>> cross-adapter share established (handle ...) <<<
RESULT: 3.xxx ms per round-trip, yy.yy GB/s  (200 rounds)
```

若在 `CreateCommittedResource` 处失败，说明该驱动不支持跨适配器共享堆（这是最关键的一条结论）。

## 六、关键实现约束（**已用原生 D3D12 实测，2026-10-24**）

> 以下两条是硬结论，改变了架构设计（原本按 MGPU Bridge 的"跨卡直连共享"规划）。

1. **AMD 驱动拒绝 `D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER`**（返回 `E_INVALIDARG`）
   → GPU 与 GPU 之间**没有**直连共享内存，跨卡只能经 **CPU 中转**：
   `A: src→readback → CPU memcpy → B: 系统内存→upload→dst`
2. **`D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE` 被拒**，必须用 `D3D12_CPU_PAGE_PROPERTY_UNKNOWN`
   （微软文档的标准写法在 AMD 上直接 E_INVALIDARG，这是踩出来的）
3. `D3D12_HEAP_FLAG_SHARED` 本身可用（`CreateSharedHandle` 成功），但只对**同一张卡**有效
4. 实测 CPU 中转带宽约 **9.4~10.7 GB/s**：8MB→0.81ms、14MB→1.28ms、28MB→2.90ms（近似线性）
5. 结果下一帧贴回（用运动矢量补偿一帧延迟），否则每帧都要等完整往返
6. 深度建议在渲染卡上重采样成 **单平面 R32F**，减少中转体积并回避 R32G8 平面布局问题

## 六之二、原 MGPU 架构（仅供参考，AMD 不可用）

- 跨卡共享 buffer + NT 句柄 + `SHARED_CROSS_ADAPTER` fence
- NVIDIA 驱动支持，AMD 驱动不支持（已实测）


## 七、风险

| 风险 | 说明 |
|---|---|
| 闭源引擎 | 依赖的社区闭源运行时一旦改动接口，本项目就得跟着调整 |
| 许可 | 骨架参考 MGPU Bridge（MIT），参数语义参考 OptiScaler / AMDNR（GPL-3.0）的公开文档；代码为本项目独立编写。本项目以 **GPL-3.0** 授权，与其兼容 |
| 无 P2P | 每帧 42MB 走内存，CPU/PCIe 有持续占用（实测可接受） |
| 一帧延迟 | 需要 MV 补偿，否则运动时会看到滞后 |
| 工程量 | 阶段 2~4 约 3~5 周（有原生工具链的前提下） |
