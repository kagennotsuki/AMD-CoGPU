# THIRD-PARTY.md — 第三方组件与权利归属

本文件列出本项目使用但不属于本项目的组件。
分发本项目时，必须把这些许可**原文**一并带到分发物里。

> 规则：**本仓库只放源码与许可原文，不放第三方二进制。**
> 二进制与权重由安装程序在用户机上从上游官方 release 获取并校验 SHA-256。

---

## 一、MIT 许可（下列各项共用一份全文）

以下项目的许可全文相同，见本文件末尾「MIT 许可全文」一节。

| 组件 | 版权行（必须原样保留） | 用途 | 是否随仓库分发 |
|---|---|---|---|
| **ImGui** | `Copyright (c) 2014-2024 Omar Cornut` | 游戏内菜单 UI 框架 | ✅ 源码在 `third_party/imgui-*/`，附 `LICENSE.txt` |
| **MGPU Bridge** | `Copyright (c) 2026 Marcelo Guibout` | 跨卡骨架的参考实现 | ✅ 仅参考其公开做法；如含源码须附 `LICENSE` |
| **DLSSNR-AMD** | `Copyright (c) 2026 mochizuki0323` | NR 运行时与模型资产（神经后端） | ❌ 二进制由安装程序从上游获取；许可原文见 `LICENSE-DLSSNR-AMD.txt` |
| **lmxxf** (`dlss5-on-amd-9070xt-porting`) | `Copyright (c) Kien`（**待核对原文**） | lmxxf 网络移植与 kernels | ❌ 二进制由安装程序从上游获取 |

**DLSSNR-AMD 特别说明**：本项目对它有实质依赖，且它是安装包里被下载的主体。
按 MIT 要求，无论以何种方式分发，都必须**随附 `LICENSE-DLSSNR-AMD.txt` 原文**，
并保留 `Copyright (c) 2026 mochizuki0323` 这行。这一条是硬性义务，不是礼貌。

**MGPU Bridge 说明**：本项目的跨卡架构在选型阶段参考了其公开设计（跨卡共享、
fence 编排），但经实测 AMD 驱动不支持 `SHARED_CROSS_ADAPTER`，实现路径已不同。
若仓库中不含其源码，则只需在 NOTICE 里致谢，不必附全文；含源码则必须附。

---

## 二、其他许可

| 组件 | 许可 | 权利归属 | 本项目的处理 |
|---|---|---|---|
| **danielblnc / DLSS-NR on AMD** | 自定义非商业许可，All rights reserved | Daniel Blanco | **不分发**其任何二进制、权重或衍生作品。仅在用户已自行安装时检测其存在。禁止反向工程其 DLL |
| **OptiScaler** | GPL-3.0 | OptiScaler contributors | 未包含其源码。参考了公开文档与配置语义 |
| **AMDNR** | GPL-3.0 + §7 附加条款 | 3zwr1 | 未包含其源码。参考了 ini 键名、参数语义与运行日志。如含其代码须附 `AMDNR_NOTICE.txt` 并署名 `AMDNR by 3zwr1` |
| **AMD FidelityFX SDK** | MIT | Advanced Micro Devices, Inc. | 如分发其 DLL，须附 AMD 的 MIT 通知原文 |
| **Intel XeSS** | Intel 自有许可 | Intel Corporation | **不随包分发** Intel 原始 DLL。本项目以代理 DLL 提供同名接口 |
| **NVIDIA DLSS / NGX / 模型权重** | NVIDIA 专有 | NVIDIA Corporation | **一律不分发**。不使用、不打包、不派生其权重或运行时 |
| **Microsoft DirectX / Detours** | MIT / MS 自有 | Microsoft Corporation | 按各自许可 |

---

## 三、分发前必须存在的文件

打包或上传 Releases 时，压缩包内必须包含：

```
LICENSE                 ← GPL-3.0 全文（本项目代码）
LICENSE-DOCS            ← CC BY-NC-SA 4.0 全文（本项目文档）
NOTICE                  ← 本项目署名与附加条款
THIRD-PARTY.md          ← 本文件
LICENSE-DLSSNR-AMD.txt  ← mochizuki0323 的 MIT 原文
<其他随包的第三方许可原文>
```

缺任何一项，这个包就已经不合规了。

---

## MIT 许可全文

以下全文逐一适用于上面「MIT 许可」表格中的每一项，
每项以各自的版权行替换 `[COPYRIGHT]`：

```
MIT License

[COPYRIGHT]

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
