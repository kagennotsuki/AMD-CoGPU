# AMD CoGPU

> 把神经渲染搬到第二张 AMD 显卡上跑：**游戏在 A 卡渲染，NR 在 B 卡推理，B 卡直接出画。**

一个跨卡（cross-adapter）神经渲染管线原型。用两块 Radeon，让原本压在同一张卡上的
「游戏渲染 + 神经网络降噪/重建」变成两张卡并行，并且用第二张卡出画。

**当前状态**：RE2 / RE4 双作实测通过，60 fps，B 卡出画 17.1 ms。

> **⚠ 目前不具备普遍适用性**
>
> 这是一个**只在特定游戏上验证过的原型**，不是装到任意游戏就能用的通用工具。
>
> - **只验证过 2 个作品**：Resident Evil 2 / Resident Evil 4（RE Engine，自带 XeSS `2.0.2.68`）。
>   其他游戏**完全没有测过**，也没有做任何适配层。
> - **依赖游戏侧的三个前提**：RE Engine + 已安装 **REFramework** + 游戏**自带 XeSS**。
>   缺任何一个都不必指望能跑——代理是靠接管游戏的 `libxess.dll` 才插进渲染管线的。
>
> 换句话说：**能跑起来的前提是"这三样都刚好对上"，不是"它支持任何游戏"。**

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
> B-4K 的底图取自**游戏 XeSS 放大之后的输出**（`pOutput`），B 卡在这张图上做 NR，
> 然后**由 B 卡自己上屏**（DComp 叠在游戏窗口之上）。
>
> 如果游戏是在**超分完成之后**才把 UI（准星 / HUD / 字幕 / 菜单）画到最终帧上，
> 这些 UI **根本不在 B 的底图里**；而 B 出画又盖住了游戏自己的出画
> → 结果是**屏幕上完全看不到 UI**（HUD / 准星 / 字幕消失）。
>
> **这类游戏请用 A 形态**：它的 NR 做在放大之前，画面回写给游戏自己，
> UI 由游戏在放大之后照常叠加，不会丢。

---

## 环境要求

| 项 | 要求 |
|---|---|
| 显卡 | 两张 AMD Radeon。**强烈建议 9000 系**（RDNA4）作为神经卡 |
| 系统 | Windows 10/11 x64 |
| 游戏 | **只保证适用**于：**RE Engine** + 已装 **REFramework** + **游戏自带 XeSS** 的作品。已实测 RE2 / RE4（XeSS 均为 `2.0.2.68`）。其他引擎、无 XeSS、或无 REFramework 的游戏**未验证、不支持** |
| 运行时 | 神经网络降噪运行时由安装器从作者发布页获取并校验 SHA-256，**不在本仓库内** |

> ⚠ 适配器识别**只认 PCI DeviceId / LUID，绝不按显卡名字**。
> 测试机上曾把 RX 7900 XT 改名成 "AMD Radeon RX 9070"，按名字选卡必错。

> ⚠ **适用范围就是上表那一格，不要外推。** 本项目靠"接管游戏自带的 `libxess.dll`"工作，
> 所以只对**已经装了 REFramework 且自带 XeSS 的 RE Engine 作品**有保障。
> 别的引擎（UE / Unity / 自研）、没有 XeSS 的游戏、或没装 REFramework 的环境，
> **没有任何验证数据**，跑不起来属于预期结果，不是 bug 报告的范围。

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

## 视频演示 · 联系作者

- 📺 **[AMD CoGPU 实机演示（B 站）](https://www.bilibili.com/video/BV15qsQ6KEK9)** —— BV15qsQ6KEK9

想看实机画面、进度更新，或者想聊技术细节，都可以到我的 B 站空间找我（视频下方留言 / 私信）。

> Bug 与功能建议仍建议走 GitHub [Issues](https://github.com/kagennotsuki/AMD-CoGPU/issues)，
> 方便留档和后续跟进；B 站适合快速问一句。

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

想先聊聊再走流程，也可以直接通过上面的 [B 站](https://www.bilibili.com/video/BV15qsQ6KEK9) 联系我。

---

## 免责

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

本项目会 hook 第三方游戏的图形链路。使用风险与合规责任由使用者自负，
包括各游戏的反作弊规则与最终用户许可协议。

未获 AMD、NVIDIA、Intel、Capcom 或任何游戏发行商的授权或背书。
"AMD"、"Radeon"、"NVIDIA"、"DLSS"、"XeSS" 等名称归其各自权利人所有。
