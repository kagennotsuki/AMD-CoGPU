# 深度信息 · 交接包（mochizuki / lmxxf / NGX 路线）

> ★★ 2026-10-30 重要修正（官方口径，用户提供）：**DLSS5 模型运行不使用深度。**
> 因此本包"把深度喂进神经渲染"这一前提**已作废**；深度只剩"用在我们自己的合成层（路线 B）"这一个用途，
> 且已从"待办"降级为**独立的可选优化**。详见
> `01_文档\待办范围-NGX-feature18与B形态取帧-20261030.md` §七。
> 本包保留作历史证据（ABI 136 实测、偏移扫描、NGX 契约）仍然有效。

> 用途：在**新对话**里继续研究"能不能把深度喂进神经渲染、以及用什么方式最划算"。
> 生成时间：2026-10-26 03:50
> 主文档对应章节：`E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\ingame\进度与结论-20261024.md` §48（S2 全域扫描）、§49（NGX 真相）、§50（本包摘要）

---

## 一、已确证的结论（三条，都有实测/官方文档支撑）

### 1. `LmxxfNrGetApi` 这条路**没有**深度入口

| 实测 | 结果 |
|---|---|
| ABI 版本扫描（v0~v4，尺寸 120~256） | 只接受 **v1 / 136 字节**，其余全部返回 2 |
| FrameInfo `struct_size` 扫描（80~176） | 运行时日志恒为 `motion 0x0 DXGI 0, depth DXGI 0` |
| depth 指针**偏移**扫描（+80~+200，步长 8） | 全部无命中 |
| lmxxf 扩展导出 `LmxxfNrSetRuntimeSettings` / `QueryContextCapabilities` | 存在，但任何参数形态都返回 2；反汇编显示其字段是**调优标量**（`motion_max_px` 等），不是纹理输入 |

判据工具（可复用）：
```
mochi_abi_scan.exe                                     ABI 版本/尺寸扫描
mochi_bench.exe  环境变量 MOCHI_FI_SIZE / MOCHI_DEPTH_OFF / MOCHI_VEL_OFF   偏移扫描
lmxxf_ext_probe.exe / lmxxf_ext_probe2.exe            扩展 API 探针（有/无会话）
```
运行时自己的判据日志（mochizuki）：`frame WxH colour DXGI 28, motion 0x.. DXGI .., depth DXGI ..`

### 2. 深度真正走的是 **NGX feature 18（DLSS-NR）**

mochizuki 官方 `docs/ARCHITECTURE.md` 原文：

> **optiscaler 路线**：OptiScaler-NR hooks the game's DLSS/FSR/XeSS upscaler, **gathers colour, depth and the engine's own motion vectors**, and calls NVIDIA's NGX API for **feature 18** (DLSS-NR). This project provides that API: an NGX core (`dlssnr_core.dll`, which OptiScaler loads through `[Libraries] NvngxPath`) implements the NGX entry points and runs our network instead of NVIDIA's.

> **Windows**：DirectX 10/11/12 games keep the system's D3D; the network runs on a Vulkan device of its own (matched by LUID). The frame crosses over in **D3D textures created as shared** and imported into Vulkan, ordered by a **shared D3D fence** imported as a Vulkan timeline semaphore. D3D12/OptiScaler: the command list is cut in the middle. **D3D11 路径的深度用 compute shader 转换后交出**；D3D10 没有 fence，**深度暂不交（未实现）**。

这份 README 还写了：`Motion Without Depth = 1`（默认，等同 NVIDIA DLL）与 `NRDepthMode`／`depth direction`
⇒ **深度是可选项，用途是精修运动矢量**（"边缘/运动物体"），不是必须。

### 3. OptiScaler 侧交帧的参数键

NGX 参数块（`NVSDK_NGX_Parameter`）里交的是 `Color` / `Depth` / `MotionVectors`（及其 Subrect / Jitter / Exposure 变体）。
参数块是纯虚类，**vtable 17 槽**，且 MSVC 对重载的发射顺序与 GCC 相反（见下"坑"）。

---

## 二、两条可行路线（对比后推荐 B）

| | **A：实现 NGX feature-18 core** | **B：自己做深度转换，喂我们自己的时域层** |
|---|---|---|
| 目的 | 让 OptiScaler 把 colour/depth/MV 交给我们 | 让残差合成用上深度（去遮挡判断） |
| 关键工作 | 实现 `NVSDK_NGX_*_Init` / `CreateFeature(18)` / `EvaluateFeature` + 手写 parameter vtable + 与现有代理共存 | 一个 DepthConvertCS（typeless SRV → R32_FLOAT）+ ResidCS 里读深度做遮挡判定 |
| 工程量 | **大** | **小** |
| 收益 | 把 depth/MV 交给**运行时**（但运行时本身吃不到 → 收益打折） | 直接改善遮挡边缘的**残影/双轮廓**（真正想要的效果） |
| 风险 | 高（换架构） | 低（加一个 pass，可开关） |

**结论：推荐 B。** 因为"运行时能不能吃深度"这条已经被证据否掉了；
而我们**自己的合成层**才是真正需要深度的地方（现在靠 `MV 长度 + 残差差异` 近似遮挡）。

---

## 三、B 路线的具体做法（下一步照做）

```
B1  DepthConvertCS
    · 输入：槽位里的游戏深度（D32_FLOAT_S8X24_UINT）
      注意：直接建 R32_FLOAT SRV 会让设备移除（我们踩过）；
            正确做法是建 typeless（R32_FLOAT_X8X24_TYPELESS）SRV 读，
            或按 mochizuki 的做法用 compute shader 转成 R32_FLOAT 线性深度
    · 输出：1920×1080（或 pColor 尺寸）的 R32_FLOAT 深度纹理，状态 NON_PIXEL_SHADER_RESOURCE
    · 先只把结果画到调试视图，确认"能读到、设备不崩"

B2  ResidCS 用深度做遮挡判定
    · prevDepth（重投影位置的上一帧深度）vs 当前深度
    · 差异 > 阈值 → 判定为"新露出/遮挡" → 丢历史（比现在的 MV+残差判据更准）
    · 阈值做成滑条，配合现有 gateK / motionAlpha 一起调

B3  验证（按 §47 检查表）
    A1 错位 / A2 静止闪 / A3 运动中双轮廓 / A4 残影 / A5 拖尾 / A6 区域异常
    客观：合成=1、等待均值 1~2ms、提交与 worker 不异常
```

---

## 四、坑（都已踩过或官方写明）

```
① 中文路径 → LoadLibrary 报 126        → 用 ASCII 目录联结 E:\AI\_归档\mochi_bare
② 源码含非 ASCII → 编译报 C2001        → 探针源码保持纯 ASCII
③ __try 不能出现在带 ComPtr 的函数里    → 抽成纯 POD 辅助函数（C2712）
④ 工具不要在 build\Release 里跑         → 那里有代理 dxgi.dll，DXGI 工厂失败
⑤ NGX parameter vtable：MSVC 对重载按【逆序】发射，GCC 按声明序
   ⇒ 跨编译器直接派生 SDK 类会"调用到同名但不同重载"、参数错寄存器却返回成功
   ⇒ 必须按 MSVC 顺序手写 17 槽（官方已在 nr_ngx_slots.hpp 写好对照表）
⑥ 深度 SRV：在 D32_FLOAT_S8X24 上建 R32_FLOAT typed SRV → 设备被移除
   ⇒ 用 typeless（R32_FLOAT_X8X24_TYPELESS）或 compute 转换
```

---

## 五、参考资料（都是公开可查）

**仓库**：https://github.com/mochizuki0323/DLSSNR-AMD
```
docs/ARCHITECTURE.md                 路线、Windows 共享纹理+fence、DX10 深度未实现的说明
README.md                            NRDepthMode / Motion Without Depth / depth direction 等配置
linux/src/pe/nr_ngx_abi.hpp          NGX 接口形状（枚举、Resource_VK、Parameter 结构）
linux/src/pe/nr_ngx_slots.hpp        parameter vtable 的 17 槽对照（含 MSVC 逆序说明）
linux/src/pe/nr_ngx_core.cpp         NGX 核心实现（61 KB，参数键与调用流程在这里）
linux/src/pe/nr_engine_panel.hpp     "Engine data 面板"——显示网络实际拿到什么（含 motion vectors）
linux/src/pe/nr_reshade_addon.cpp    ReShade 路线（深度来自 ReShade，运动由 VORT 估计）
```

**本机已有素材**
```
E:\AI\跨卡NR\05_运行时与模型\mochizuki-裸文件\                运行时 + 三个模型 + aco/shaders（846 文件 / 396 MB）
E:\AI\_归档\mochi_bare\                      ASCII 联结（源码里用这个路径）
E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\src\              探针源码（mochi_abi_scan / mochi_bench / lmxxf_ext_probe*）
E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\build\Release\    已编译的探针
E:\AI\_归档\tmp\mochi_run\                  探针运行目录（必须）
```

---

## 六、当前项目状态（与本包相关）

```
· 稳定基线：.bak-stable-1026-无条（DLL 633 KB，已装机）
· 合成层参数：temporal 0.02 / motionLo 0.02 / motionHi 0.04 / motionAlpha 0.20 /
              gateK 8 / jitter 0.5 / limit 0.2 / edge 0.6 / sat 0.9 / 双线性取样
· 遮挡判断目前用：MV 长度 + 残差差异（**深度尚未使用**）
· 待办主线（S1~S6）与 §47 检查表在主文档 §44 / §47
```
