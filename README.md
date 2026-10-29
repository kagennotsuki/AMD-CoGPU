# AMD CoGPU

> 把神经渲染搬到第二张 AMD 显卡上跑：**游戏在 A 卡渲染，NR 在 B 卡推理，B 卡直接出画。**

一个跨卡（cross-adapter）神经渲染管线原型。用两块 Radeon，让原本压在同一张卡上的
「游戏渲染 + 神经网络降噪/重建」变成两张卡并行，并且用第二张卡出画。

**当前状态**：RE2 / RE4 双作实测通过，60 fps，B 卡出画 17.1 ms。

> 架构设计与验证独立完成，代码实现由 AI 编码助手协作。

---

## 效果（实测，非估算）

| 项目 | RE2 | RE4 |
|---|---|---|
| 游戏帧率 | 60 fps | 60 fps |
| B 卡出画耗时 | **17.1 ms** | **16.79 ms** |
| 跨卡拷贝跳过 | 0 | 0 |
| 提交耗时 | 2.7~3.2 ms | 2.7~3.2 ms |

测试平台：`RX 7900 XT`（游戏渲染）+ `RX 9070 XT`（神经渲染，DeviceId `0x7550`）。

---

## 为什么这件事不是「换个卡跑模型」

三件事决定了它必须重新设计，而不是配置一下就能用：

### 1. AMD 驱动不支持跨卡共享内存，只能走 CPU 中转

```
D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER  →  E_INVALIDARG
D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE →  E_INVALIDARG（必须用 UNKNOWN）
```

`D3D12_HEAP_FLAG_SHARED` 本身可用，但**只在同一张卡内有效**。所以 A → B 的每一帧
都必须 `readback → CPU memcpy → upload`，实测带宽 **9.4~10.7 GB/s**
（8 MB → 0.81 ms，14 MB → 1.28 ms，28 MB → 2.90 ms，近似线性）。

### 2. 异步处理必然导致「画面与答案错配」

网络答案晚到 1~3 帧，如果直接贴回当前帧，就会出现错位、双轮廓、周期性抖动。
本项目用**槽位队列（Slot Ring）**解决：给每个在途帧一个带身份校验的槽位，
用 K/L/D 三条约束保证「拿到的答案确实属于这一帧」。
详见 [`01_文档/槽位队列-延迟配对方案.md`](01_文档/槽位队列-延迟配对方案.md)。

### 3. 出画位置决定了画质上限

实测四种出画形态后，留下两个**都实测可用**的形态（RE2 / RE4 均 60 fps；`bform=6` 已弃用）：

| 形态 | 做法 | 特点 |
|---|---|---|
| **A 形态**（`bform=0`） | B 卡在 1440p 上做 NR → 画面回 A 卡 → 由游戏放大出画 | 更省算力；画质略佳；**UI 不受 NR 影响** |
| **B-4K 形态**（`bform=9`） | A 卡真 XeSS 放大到 4K → 跨卡 → B 卡在 4K 上做 NR → B 卡直接出画 | 算力消耗更大；4K 上的降噪负担全在 B 卡 |

> **B-4K 的适用性限制（重要）**
>
> 它把 NR 做在「A 卡**超分之后**的 4K 画面」上。
> 如果游戏是在**超分完成之后**才把 UI（准星 / HUD / 字幕 / 菜单）覆盖上去，
> 这些 UI 会**一起被送进 NR** → 文字发虚、细线糊边、边缘抖动。
>
> **这类游戏请用 A 形态**：它的 NR 做在放大之前的场景图上，
> UI 由游戏在放大后叠加，不会经过神经网络。

---

## 环境要求

| 项 | 要求 |
|---|---|
| 显卡 | 两张 AMD Radeon。**强烈建议 9000 系**（RDNA4）作为神经卡 |
| 系统 | Windows 10/11 x64 |
| 游戏 | 需自带 XeSS 超分（RE2 / RE4 均为 `2.0.2.68`，同一套代理通用） |
| 运行时 | 神经网络降噪运行时由安装器从作者发布页获取并校验 SHA-256，**不在本仓库内** |

> ⚠ 适配器识别**只认 PCI DeviceId / LUID，绝不按显卡名字**。
> 测试机上曾把 RX 7900 XT 改名成 "AMD Radeon RX 9070"，按名字选卡必错。

---

## 安装

1. 到 [Releases](https://github.com/kagennotsuki/AMD-CoGPU/releases) 下载最新安装器
2. 双击 `启动安装.bat`，按 8 页向导走完
3. 首次部署会把游戏自带的 XeSS 改名成 `libxess_orig.dll`，再把本项目代理装成 `libxess.dll`

游戏内热键：`Del` 菜单 · `F10` NR 开关 · 小键盘 `1` 或 `End`（NumLock 关）切换 A ↔ B-4K 形态

---

## 目录

| 目录 | 内容 |
|---|---|
| [`01_文档/`](01_文档/) | 设计文档、实测报告、方法论 |
| [`02_源码与构建/`](02_源码与构建/) | 代理源码、跨卡传输层、离线探针工具 |
| [`06_交接包/`](06_交接包/) | 主题交接文档（深度信息、风格测试、安装器） |
| [`08_工具与脚本/`](08_工具与脚本/) | 安装程序、配置工具、发布与自检脚本 |

几个值得先看的：

- [`01_文档/AMD-CoGPU-项目简介.md`](01_文档/AMD-CoGPU-项目简介.md) — 一页纸说清这个项目
- [`01_文档/跨卡NR-完整说明.md`](01_文档/跨卡NR-完整说明.md) — 使用说明 + 技术全链路
- [`01_文档/槽位队列-延迟配对方案.md`](01_文档/槽位队列-延迟配对方案.md) — 通用的异步管线设计
- [`01_文档/帧错配定位方法-跑马灯帧标记-20261028.md`](01_文档/帧错配定位方法-跑马灯帧标记-20261028.md) — 怎么定位错位

---

## 边界与已知限制

```
· 跨卡帧生成（FG）进程内做不到：FG 必须持有游戏 swapchain 与游戏设备的 MV/深度
· 跨卡 fence 不可共享（实测 0x80070057），所以不能用硬件同步编排
· 静止闪烁 / 运动中双轮廓 / 残影 是社区共同难题，本项目也未完全解决
· 这是原型，不是产品。没有做多游戏适配
```

---

## 与社区其他项目的关系

本项目**不是** OptiScaler、AMDNR、MGPU Bridge 中任何一个的分支，是独立实现。

它站在这些项目的肩膀上：

| 项目 | 作者 | 许可 | 本项目用到什么 |
|---|---|---|---|
| [DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD) | mochizuki0323 | MIT | 神经降噪运行时与模型 |
| [dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) | Kien (lmxxf) | MIT | lmxxf 网络移植与 kernels |
| MGPU Bridge | Marcelo Guibout | MIT | 跨卡骨架的参考 |
| OptiScaler / AMDNR | OptiScaler contributors / 3zwr1 | GPL-3.0 | 参数语义与架构选型参考 |

完整第三方清单与许可原文见 [THIRD-PARTY.md](THIRD-PARTY.md) 与
[`08_工具与脚本/安装程序/licenses/`](08_工具与脚本/安装程序/licenses/)。

**本仓库不含**：厂商二进制（NVIDIA / Intel / Microsoft）、模型权重、游戏文件。
运行时由安装器在用户机上从上游官方 release 获取并校验。

---

## License

| 部分 | 许可 |
|---|---|
| **代码** | [GPL-3.0](LICENSE) — 可自由使用、修改、分发（含商用），但衍生作品必须同样以 GPL-3.0 开源并提供源码 |
| **文档与文章** | [CC BY-NC-SA 4.0](LICENSE-DOCS) — 可阅读、翻译、引用、教学使用，**必须署名、不得商用**、修改后须同许可分发 |

署名与附加条款见 [NOTICE](NOTICE)。

**如果你分叉、移植或基于本项目做衍生作品，请在你的 README 中保留：**

> 基于 [AMD CoGPU](https://github.com/kagennotsuki/AMD-CoGPU) by kagennotsuki，以 GPL-3.0 授权。

本项目名称与标识不随代码授权，请勿使用。

**商业授权**：代码本身已以 GPL-3.0 授权，可直接遵守 GPL 使用。
若需要以不同条款商用（例如闭源集成），请到
[Issues](https://github.com/kagennotsuki/AMD-CoGPU/issues) 开一个标题含
`commercial licensing` 的 issue。

---

## 免责

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

本项目会 hook 第三方游戏的图形链路。使用风险与合规责任由使用者自负，
包括各游戏的反作弊规则与最终用户许可协议。

未获 AMD、NVIDIA、Intel、Capcom 或任何游戏发行商的授权或背书。
"AMD"、"Radeon"、"NVIDIA"、"DLSS"、"XeSS" 等名称归其各自权利人所有。
