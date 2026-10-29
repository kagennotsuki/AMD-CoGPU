# 跨卡 NR · 完整说明（使用 + 技术）

> 项目：在 RE2 上让 DLSS5 神经渲染（NR）跑在**第二张显卡**上，结果合成回游戏画面。
> 硬件：**RX 7900 XT**（游戏渲染）+ **RX 9070 XT**（NR 推理）
> 当前后端：lmxxf（14 ms/帧）；mochizuki 正在移植中
> 版本基线：2026-10-25（gateK 8 / motionAlpha 0.20 / jitter 0.5 / 尺寸自动重建 / 无调试色条）
> 本文档合并了"使用说明"与"技术全链路"，取代 `跨卡DLSS5-全链路.md`

---

# 第一部分 · 使用说明

## 1. 它是什么

```
游戏（RE2）正常渲染 → 我们把画面缩到 1080p 交给另一张卡做 NR
                   → 把 NR 的"改进量（残差）"叠回游戏画面
                   → 再交给真 XeSS 上采样到 4K
```

**能做到**：跨卡 NR 正常出图、静止不闪、运动无拖影、无拖尾、菜单可实时调参、切 XeSS 档位自动重建。

## 2. 前置条件

| 项 | 要求 |
|---|---|
| 显卡 | 两张 AMD 卡；NR 卡必须是 **RX 9070 XT（DeviceId 0x7550 / Navi48）** |
| 系统设置 | `HKCU\Software\Microsoft\DirectX\UserGpuPreferences` 中 `re2.exe` 设 `GpuPreference=1;`（游戏跑 7900 XT，把算力留给 NR 卡） |
| XeSS | RE2 的 XeSS 由 **REFramework 的定制版插件**提供；`dinput8.dll` 不能删 |
| NR 运行时 | `LmxxfNrRuntime.dll` + `LmxxfNrRuntime.pak`（约 486 MB）放在游戏目录 |

## 3. 安装

游戏目录（`...\RESIDENT EVIL 2  BIOHAZARD RE2\`）需要这些文件：

```
libxess.dll            我们的代理（替换游戏原来调用的那个）
libxess_orig.dll       真 XeSS（2.0.2.68），代理会把调用转发给它
LmxxfNrRuntime.dll     NR 运行时
LmxxfNrRuntime.pak     NR 运行时资产（约 486 MB）
```

安装步骤：

```
① 备份原文件（永远先做）
   libxess.dll → libxess.dll.bak-<日期>
② 把我们的 libxess.dll 复制进去
③ 启动游戏
```

## 4. 启动后怎么确认它在工作

看游戏目录下的 `xess_proxy.log`，三件事：

```
✓ 选卡正确：  worker: 使用 AMD Radeon RX 9070 XT
✓ 会话就绪：  worker: 会话就绪 (1920x1080, 8294400 字节)
✓ 正在合成：  stats: 帧N 提交 11.0ms worker=14.0ms #M 合成=1
```

`合成=0` 表示还没开始叠残差（前几帧正常），持续 0 说明有问题。

## 5. 热键与菜单

| 键 | 作用 |
|---|---|
| **Del** | 打开/关闭参数菜单 |
| F10 | NR 总开关（关掉后完全走原版 XeSS）。**B 形态下直接生效，不需要先开菜单（c 版起）** |
| 小键盘 1 / **End**（NumLock 关） | A 形态 ↔ B-4K 形态 热切换（写 `xess_proxy.bform.txt`，约 1 秒接手）。**没有小键盘的键盘直接按 End**，NumLock 关闭时它就是同一个键 |
| ~~F2~F9 / F11~~ | 已停用，参数改在 Del 菜单里调 |

> **B 形态出画时机**：日志出现 `BFORM-P: 出画就绪` 后约 1 帧就上屏；
> 稳定态可看 `BFORM-P: 产出 N 帧 | 间隔 均值16.7ms`（≈60fps）。
> 进游戏会先看到一行 `WND: 已定位游戏主窗口 …（不依赖菜单）`，这就是"不需要开菜单"的标志。

菜单分三块：**画面（gain/limit/edge/sat/smooth）· 稳定性（temporal/motionLo/motionHi/motionAlpha/gateK）· 调试（jitterScale / 调试视图 / 直出 / 恢复默认）**。
菜单可拖动、右下角可缩放、Del 关闭。

## 6. 当前参数基线（已验证可用）

| 参数 | 值 | 说明 |
|---|---|---|
| 残差增益 `gain` | 0.66 | 换 mochizuki 时需 2.8~3.0 |
| 残差上限 `limit` | 0.20 | |
| 边缘保护 `edge` | 0.6 | |
| 去饱和 `sat` | 0.9 | |
| 空间平滑 `smooth` | 0 | |
| 时域强度 `temporal` | 0.02 | 静止时的历史权重 |
| 运动下界 `motionLo` | 0.02 | 低于此 = 强平滑 |
| 运动上界 `motionHi` | 0.04 | 高于此 = 进入运动模式 |
| **运动时域上限** `motionAlpha` | **0.20** | 运动中允许的历史上限 |
| **逐像素门限** `gateK` | **8** | 原 40 会静止小幅闪 |
| **抖动缩放** `jitterScale` | **0.5** | 传给真 XeSS 的 jitter 倍数 |

## 7. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| **开菜单/进游戏就卡死** | worker 选错卡（LUID 与 HIP 不匹配）→ 永不出答案 | 已修：按 DeviceId 选卡 + 等待 30ms 上限（超时本帧跳过 NR） |
| **切 XeSS 档位后一块矩形没有 NR / 画面变暗** | 渲染分辨率变了，资源仍按旧尺寸 | 已修：检测尺寸变化 → 重建资源（含补建答案 SRV + 状态清中性） |
| **静止画面轻微闪** | alpha 二元硬切换 + gateK 斜率过陡 | 已修：alpha 连续化 + motionHi 0.04 + gateK 8 |
| **运动物体内部亮度闪** | 可信度按 MV 长度算 → 一动就丢历史 | 已修：改用"颜色一致性" + motionAlpha 0.20 |
| **运动有拖影** | 历史权重过高 | 提高 `motionHi`（如 0.05）或 `gateK`；或降低 `motionAlpha` |
| **画面像没开 NR** | 增益太低 | 提高 `gain`（换 mochizuki 后需 2.8~3.0） |
| **UI（HUD）闪烁** | XeSS 上采样抖动，与 NR 无关 | 试把 `jitterScale` 调 0.40 / 0.35（代价是略软） |
| **显卡被改过名字** | 按名字选卡会错 | 代码只认 DeviceId，改名无影响 |
| **B 形态按 F10 不亮 NR（要开一次菜单才亮）** | 旧版只在 Del 菜单里取游戏窗口句柄，B 出画初始化因此被卡住 | 已修（定版 B4K-20261029c）：句柄自动定位，进游戏直接 F10 就出画；日志见 `WND: 已定位游戏主窗口 …（不依赖菜单）` |
| **B 形态画面贴偏/只占一角** | 覆盖层缩放按建链时的客户区算的 | 已修（c 版起跟随窗口）：日志会打 `BFORM-P: 窗口客户区变为 3840x2160 → 覆盖层缩放 …` |

## 8. 与其它 mod 共存

```
· REFramework：必须保留（XeSS 由它的插件提供）。dinput8.dll 不能删。
· AMDNR / ReShade 路线：与我们的代理互斥（都改最终画面或同层 hook），
  用 tools\切换-AMDNR-与-跨卡NR.ps1 切换（-Status / -To AMDNR / -To CrossGpu）
· 不要在游戏目录同时放两套 dxgi.dll 代理
```

## 9. 回滚 / 卸载

```
① 把 libxess.dll 换回备份（或把 libxess_orig.dll 改名为 libxess.dll）
② 需要完全恢复原状：删除我们的 libxess.dll，恢复游戏原始文件
③ 日志文件 xess_proxy.log 可删（会重新生成）
```

---

# 第二部分 · 技术全链路

## 10. 总体结构（带显卡标注）

```
┌──── GPU A：RX 7900 XT ────┐  ┌─ 跨卡共享内存 ─┐  ┌─ GPU B：RX 9070 XT ─┐
│                            │  │                │  │                      │
│ 游戏渲染 2227×1253 (含HUD)  │  │                │  │                      │
│        │                   │  │                │  │                      │
│ DownCS 2227×1253→1920×1080 │  │                │  │                      │
│        └── 写输入 ─────────┼─►│ 输入 buffer    │  │                      │
│                            │  │ 512B头+8.3MB   │─►│ NR 推理 1920×1080    │
│                            │  │                │  │  lmxxf 14.0 ms       │
│                            │  │ 答案 buffer    │◄─┘  写答案(分段 slot)    │
│                            │  │ 4段 × 8.3MB    │                         │
│                            │  └───────┬────────┘                         │
│ 取答案 CopyBufferRegion ◄──┼──────────┘                                  │
│        ▼                   │                                             │
│ 本地镜像 g_ansLocal (8.3MB)│                                             │
│        │ SRV(槽20)         │                                             │
│ ResidCS 合成残差→pColor    │                                             │
│        ▼                   │                                             │
│ 真 XeSS 2.0.2 → 3840×2160  │                                             │
└────────────────────────────┘  └────────────────┘  └──────────────────────┘
```

**关键点**：
- XeSS 上采样始终在**游戏卡**上，跨卡只承担 NR（所以每帧只搬 1080p 的 8.3MB，不搬 4K）
- 每帧只有两次跨卡穿越：输入出、答案回；中间不经过 CPU 内存

## 11. 介入点：`xessD3D12Execute`

代理导出 `libxess.dll` 的全部符号，核心是拦截：

```cpp
xessD3D12Execute(ctx, cmdList, params)
```

`params` 里用到的偏移（调真 XeSS 时用拷贝 `buf[256]` 并替换）：

| 偏移 | 内容 | 说明 |
|---|---|---|
| +0 | pColor | 2227×1253，fmt=87（R11G11B10_FLOAT） |
| +8 | velocity (MV) | fmt=13 |
| +16 | depth | fmt=19 |
| +40 | pOutput | 3840×2160，fmt=24 |
| +48 / +52 | jitter.x / .y | 亚像素抖动 |

## 12. 每帧流程（游戏侧 5 步）

| # | 步骤 | 在哪张卡 | 关键点 |
|---|---|---|---|
| ① | **等答案** | CPU | 等 3 帧前那一帧的结果，最多 30ms；超时本帧不做 NR |
| ② | **捕获** | 7900 XT | CopyResource 到槽位 `N%4`（color/mv/depth + 记录 jitter），状态切回去 |
| ③ | **降采样 + 送出** | 7900 XT | 用**上一帧**槽位，DownCS 2227×1253 → 1920×1080，GPU 直写跨卡 buffer |
| ④ | **取答案 + 合成残差** | 7900 XT | 答案 → 本地镜像 → SRV → ResidCS 在 2227×1253 网格合成 → 叠回 pColor |
| ⑤ | **调真 XeSS** | 7900 XT | 替换该槽位 MV/depth/jitter（jitter×0.5）→ 3840×2160 |

worker 线程并行：等输入 → 读跨卡 buffer → NR → 写答案分段 → 通知主线程。

时间轴（K=4 槽位，kDelay=3）：

```
帧 N-1 : ③ 送出 N-1 的输入 ──► worker 开始算
帧 N   : ① 等 N-3 的答案 → ④ 合成 → ⑤ 上采样
```

## 13. 跨卡传输（与单卡实现的关键差异）

### 13.1 资源创建（缺一不可）

```cpp
hd.Alignment = 65536;                                   // 必须 64K
hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;         // 跨卡不支持纹理
rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;     // 缺 → 0x80070057
CreatePlacedResource(heap, 0, &rd, ...);

CreateSharedHandle(heap, ...);   // 共享的是 heap 句柄，不是资源句柄
```

### 13.2 数据布局

```
输入 buffer：[0..511] 头（magic "XNR1" | width | height | format | pitch）
            [512..] 载荷 1920×1080×4 = 8,294,400 字节
答案 buffer：512 + slot × 8,294,400（4 段，互不覆盖）
```

### 13.3 索引映射

```hlsl
// buffer 是 1920×1080，合成网格是 2227×1253 → 必须 uv 映射 + clamp
// 且 buffer 不能过滤采样 → 手工 4 邻域双线性
float2 fc = uv * float2(1920,1080) - 0.5;
```

### 13.4 同步：没有 fence

```
跨卡 fence 不能共享（OpenSharedHandle → 0x80070057）
⇒ 靠帧延迟错开：K=4 槽位、kDelay=3
⇒ 兜底：主线程等待有界 30ms，超时跳过本帧 NR
帧延迟规则：(kDelay-1) × 帧时间 > worker 时间 + ~2ms
```

## 14. 残差合成与时域处理

```
e = 双线性(答案 − 当初喂入的输入)
e *= gain；clamp(±limit)；按局部方差/边缘压低；去饱和
conf = 颜色一致性（上一帧槽位颜色在重投影位置比对）
gate = 残差差异（防拖影）
a    = lerp(1.0, gAlpha, conf * gate)
e    = lerp(历史残差, e, a)       // 历史存于 0.5 中性的状态纹理
out  = saturate(pColor + e)
```

**gAlpha 的分段策略（三次迭代的结论）**：

```
静止（运动量 < 0.02）  → alpha = 0.02      → 强平滑，不闪
过渡（0.02 ~ 0.04）    → smoothstep 连续
运动（> 0.04）         → 上限 0.20，再由 conf/gate 决定实际量
遮挡/新露出（颜色不一致）→ conf→0 → alpha→1 → 不用历史，不拖影
```

**两个必须同时存在的条件**（否则失效）：
```
① conf 必须按【颜色一致性】而不是 MV 长度（MV 大会误判成不可信）
② alpha 必须先允许 < 1（否则 a = lerp(1, alpha, conf*gate) 恒为 1，所有保护失效）
```

## 15. 健壮性设计

| 机制 | 起因 | 做法 |
|---|---|---|
| 适配器按 **DeviceId** 选 | 显卡被改名（7900 XT → "9070"/"9070 GRE"），按名字选错卡 | 只认 `DeviceId == 0x7550`，删除一切名字匹配 |
| 等待答案 **30ms 有界** | 选错卡 → 永不出答案 → 主线程无限阻塞 → "开菜单就卡死" | `wait_for` + 超时跳过本帧 NR |
| **尺寸自动重建** | 切 XeSS 档位 → 渲染分辨率变 → 只有一块矩形有 NR | 检测 pColor 尺寸 → 重建全部资源 + **补建槽 20 答案 SRV** + 状态清中性 + warmup 8 帧 |
| 恢复默认补全 | 新参数漏在 reset 之外 | 所有新参数都进 reset |

## 16. 实测数据

| 指标 | 数值 |
|---|---|
| 游戏侧单帧总耗时（提交） | 10.8 ~ 12.4 ms |
| worker（含跨卡读写） | **12.0 ~ 12.6 ms（mochizuki 旧模型；Small 模型 7.8~8.1 ms）** |
| 主线程等待答案均值 | 1.0 ~ 2.3 ms |
| CPU↔GPU 输入传输 | **0**（GPU 直写跨卡 buffer） |
| 答案 CPU 回读 | **0**（GPU 本地镜像 + SRV） |
| 端到端输入延迟 | ≈ kDelay × 帧时间 = **3 帧**（60fps ≈ 50ms；帧率未实测） |

### 16.1 卡内并行：队列数量 ≠ 并行度（2026-10-27 探针）

探针：`02_源码与构建\跨卡神经渲染原型\src\queue_overlap_probe.cpp`（**必须放 ASCII 目录、且不能与 dxgi 代理同目录运行**）

| 组合 | 重叠比 | 结论 |
|---|---|---|
| 同类 ALU 负载 × 2 队列 | **1.85**（≈2.0） | 纯时间片，**0 收益** |
| 同类 ALU 负载 × 4 队列 | **3.70**（≈4.0） | 同上 |
| ALU ∥ 纹理采样 | **0.70** | 约 30% 真重叠 |
| ALU ∥ 拷贝引擎 | 0.88 | 拷贝几乎完全被隐藏 |
| **同卡 33MB/帧拷贝**（真实负载） | **ΔFPS −0.04** | 完全免费（占显存带宽 <0.3%） |

⇒ **两条 ACE 队列共享同一批 SM**；想拿并行只能靠"不同硬件单元"或"不同卡"，**不能靠队列数量**。

### 16.2 ★ B 卡余量：NR 是「延迟受限」，不是「吞吐受限」

探针：`src/bcard_headroom_probe.cpp`（按 DeviceId 0x7550 选 9070 XT）。
方法：NR 正常跑，在 B 卡另一条 compute 队列按档位灌负载，观察 `network ms` 变化。

| 档位（额外负载） | network ms | B 占用 | B 功耗 |
|---|---|---|---|
| **0 空转（只有 NR）** | **14.20 ms** | 88% | **130 W / 304 W** |
| 1（256K 线程，最小档） | 17.86（**+26%**） | 99% | 287 W |
| 4（1M，满载） | 29.21（**+106%**） | 100% | 299 W |

**三个结论**：
1. NR 单独跑只吃 **130W**，说明大量单元是闲的（"88% 占用"只是时间维度）
2. 但**加任何负载都会让 NR 变慢**（1/4 负载就 +26%）
3. ⇒ mochizuki 每帧是一条**长串行依赖链**，任何抢占都直接拉长关键路径。**空闲单元救不了它**

这同时解释了日志里长期的 `worker 等待 GPU 完成超时`。

### 16.3 ★ 跨卡（PCIe）有效带宽

探针：`src/pcie_bandwidth_probe.cpp`。两条路径：A 拷入跨适配器共享堆 → B 从共享堆拷出。

| 尺寸 | 串行 mean | **流水 mean** | p99（流水） | 有效带宽 | 占 16.67ms 预算 |
|---|---|---|---|---|---|
| 8 MB | 1.71 ms | 0.81 ms | 1.03 | 10.4 GB/s | 4.9% |
| 16 MB | 3.27 ms | 1.61 ms | 1.89 | 10.4 GB/s | 9.7% |
| 33 MB | 5.73 ms | 3.16 ms | 4.31 | 10.9 GB/s | 19.0% |
| **44 MB**（B 形态所需：色+MV+深度） | 7.85 ms | **4.16 ms** | **5.14** | **11.1 GB/s** | **25.0%** |
| 64 MB | 11.22 ms | 5.84 ms | 6.59 | 11.5 GB/s | 35.0% |

**解读**：
- 有效带宽稳定 **~11 GB/s**（≈ PCIe 4.0 x8~x16 实际水平），**抖动小**（p99 只比 mean 高 1ms 左右）
- 传输与计算**可重叠**（A 侧走 A 的拷贝引擎、B 侧走 B 的拷贝引擎，都不占计算）
- ⇒ **44MB 可以靠"提前一帧开始传"藏起来**，不占关键路径；代价是延迟 +1 帧
- ⇒ **跨卡传输不是 B 形态的拦路虎**

### 16.4 帧生成选型（详见 内部文档「帧生成-三形态对比」（未随仓库发布））

| 形态 | 小黄鸭 LSFG | FSR4 FG |
|---|---|---|
| **跨卡 A 形态** | ✅ **实测 180 显示帧** | ⚠️ 要抢 present；A 已 305W |
| **跨卡 B 形态** | ✅ 理论最佳（B 自己出画） | ❌ 要搬 depth+MV |
| **9070 XT 单卡** | ✅ 可行 | ✅ 理论最佳（G-buffer 同卡） |

关键区分：**小黄鸭只要最终色（进程外）**；**FSR4 FG 要色+深度+MV+UI 掩码（管线内）**。


## 17. 资源清单

**游戏侧（7900 XT）**
```
g_dev                  游戏设备
g_slots[4]             每槽：color/mv/depth/jx,jy/answer/feedInput
g_netS                 1920×1080 降采样目标（UAV 5）
g_pre                  合成目标（UAV 6，尺寸 = pColor）
g_resTex               1080p 残差（SRV 4，CPU 路径用）
g_resSmooth            1080p 时域状态（UAV 7，0.5 中性）
g_ansLocal             8.3MB 本地镜像（SRV 20）
g_fedTex[4]            每槽被喂入的 1080p 输入（SRV 16+i）
g_heap                 描述符堆（注意 BuildResources 会重建它）
g_psoDown/g_psoResid   两个 Compute PSO
```

**跨卡**
```
g_xHeapIn/Out、g_xBufIn/Out：DEFAULT + 64K + SHARED|SHARED_CROSS_ADAPTER + ALLOW_CROSS_ADAPTER
输入 1 段；答案 4 段
```

## 18. 已知限制

```
· 跨卡无 fence → 依赖帧错开；理论上可能读半写（视觉未见问题）
· 跨卡只能 buffer 不能纹理 → 残差必须手工双线性（已做）
· NR 运行时要求 D3D12 设备 ↔ HIP 设备同卡 → worker 设备位置被绑死
· 深度未参与合成（深度 SRV 在 D32_FLOAT_S8X24 上建不了）
· UI 闪烁来自 XeSS 上采样（不属本层），用 jitter 0.5 缓解
· mochizuki 后端 ABI 136 回退尚未接入（进行中）
· **NR 是延迟受限的**：任何在 B 卡上并行的计算都会拖慢它（+1/4 负载即 +26%）
  → 想在 B 上再加东西（上采样/帧生成）必须先接受"串行叠加"的账
· **B 形态（B 做上采样+出画）零余量**：NR 12 + 上采样 3 = 15ms，预算 16.67ms
· **同卡内拷贝免费，跨卡 ~11GB/s**：44MB/帧 需 4.16ms（流水），可靠提前一帧隐藏
· 两个探针（queue_overlap / bcard_headroom / pcie_bandwidth）**必须放 ASCII 目录**运行：
  路径含中文时 `GetModuleFileNameA` / `fopen` 会错乱；且不能与 dxgi/d3d12 代理同目录（会被劫持）
```


## 19. 文件与日志

```
游戏目录：
  libxess.dll            我们的代理（装机版）
  libxess_orig.dll       真 XeSS 2.0.2.68
  LmxxfNrRuntime.dll/.pak NR 运行时与资产
  xess_proxy.log         运行日志（排障第一现场）

工程目录：
  E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\ingame\libxess_proxy_v9.cpp    全部实现（~95KB）
  E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\ingame\libxess_proxy_v9.cpp.bak-*  各阶段备份
  E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\ingame\进度与结论-20261024.md  开发全记录（§37~§44 是近期主线）
  E:\AI\跨卡NR\02_源码与构建\跨卡神经渲染原型\tools\切换-AMDNR-与-跨卡NR.ps1  两条路线切换
```
