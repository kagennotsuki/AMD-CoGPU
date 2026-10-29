# XeSS 接口实测结果（跨卡 NR 的正确集成点）

> 2026-10-24 凌晨，用代理 `libxess.dll` 实测得到。原始日志：`backup\xess_execute_params.log`

## 一、为什么这条路是对的

RE2 **没有原生上采样器**（`OptiScaler.log`：*this game has no native upscaler*），
所以 color/depth/motion/jitter 全部由 **REFramework 的 TemporalUpscaler（pd-upscaler）**
递给后端 DLL —— 也就是游戏目录里真正的 **Intel XeSS SDK 2.0.2.68（`libxess.dll`，74.19 MB）**。

之前那套能用的工具（AMDNR）就是**代理这个 DLL** 拿到全套输入。
我原先在 Present 时刻抓成品帧，**拿不到 depth/MV**，所以网络只能输出接近恒等的结果。

## 二、`xessD3D12Execute` 的参数结构（实测）

签名：`xess_result_t xessD3D12Execute(void* ctx, ID3D12GraphicsCommandList* cmdList, const xess_d3d12_execute_params_t* p)`

实测字段（地址偏移是在 64 位下按 qword 打印后换算）：

| 偏移 | 内容 | 实测值 |
|---|---|---|
| +0 | `pColor` 输入颜色 | 2227×1253，`fmt=87`（B8G8R8A8_UNORM），flags 0x1 |
| +8 | `pVelocity` 运动矢量 | 2227×1253，`fmt=13`（R16G16B16A16_SNORM），flags 0x5 |
| +16 | `pDepth` 深度 | 2227×1253，`fmt=19`（R32G8X24_TYPELESS），flags 0x2 |
| +24 / +32 | 保留（实测为 0） | 0 |
| +40 | `pOutput` 输出 | **3840×2160，`fmt=24`（R10G10B10A2_UNORM）**，flags 0x5 |
| +48 | `jitterOffsetX` | 实测 0.25（Halton 序列，第 1 帧为 0） |
| +52 | `jitterOffsetY` | 实测 −0.1666667 |
| +56 | `exposureScale` | 1.0 |
| +64 | `inputWidth` | 2227 |
| +68 | `inputHeight` | 1253 |

（更后面还有若干指针字段，涉及描述符堆等，接入时再逐个确认。）

## 三、这个集成点的好处

| | Present 抓帧（旧方案） | libxess 代理（新方案） |
|---|---|---|
| 输入 | 只有成品 color | **color + depth + velocity + jitter + exposure** |
| 输入分辨率 | 被迫 1080p 下采样 | **2227×1253（接近原生）** |
| 合成 | 我自己双线性放大（丢 4K 细节） | **pd-upscaler 自己合成**，运行时 detail lift 生效 |
| 出画 | 需要 DComp overlay | **不需要**（输出写进普通纹理） |
| 交换链写回问题 | 反复失败 | **不存在** |

## 四、下一步实现（v2）

1. `ProxyExecute` 里取到 pColor / pDepth / pVelocity / pOutput / jitter / exposure / 尺寸
2. pColor（11 MB）先单独回读到 CPU → 跨卡送到 9070 XT → lmxxf → 结果回传
3. 结果上传并写进 `pOutput`（R10G10B10A2），**不再调用真实 XeSS**（我们替代它）
   - 兜底：若我们的链路未就绪，则照常调用真实 `xessD3D12Execute`
4. 之后再加 depth / velocity（各 11 / 22 MB），让运行时的 detail lift 完整生效
5. 最后做菜单（启停、分辨率档、强度、状态显示）

## 五、代理文件（已生成，可复用）

```
ingame\xess_proxy.def          72 个导出，序号与真身完全一致（实测 0 不一致）
ingame\xess_forwarders.asm     72 个 asm 转发桩（ML64）
ingame\xess_fwd_names.inc      导出名表
ingame\libxess_proxy.cpp       v1：只记录、不改行为（已验证游戏正常运行）
backup\libxess_real.dll        真实 DLL 备份（双保险）
```

安装方式（v1 实测有效）：把真身改名 `libxess_orig.dll`，代理放成 `libxess.dll`。
还原：删掉代理、把 `libxess_orig.dll` 改回 `libxess.dll`。

---

## 六、已验证的三项能力（2026-10-24 凌晨，游戏内实测）

| 能力 | 证据 |
|---|---|
| 拦截 `xessD3D12Execute` | 72 个导出序号与真身 0 不一致；XeSS 行为不变 |
| 读取游戏真实 color | `color 回读 #0..#7: 均值 79.8, 非零采样 2725/2739`（真实画面，非黑帧） |
| **写入 pOutput 并显示在屏幕上** | 往 pOutput 左上角写 480x480 纯绿，**用户肉眼确认可见**；无设备丢失 |

第三项是关键：**不需要 DComp overlay、不碰游戏交换链**，且 REFramework / Steam 覆盖层全部正常。

## 七、v3 设计（最终形态）

```
ProxyExecute（游戏帧内，录在游戏自己的 cmdList 上）
  ① pColor 2227x1253 BGRA → 下采样 1920x1080 RGBA（compute，7900 XT）
  ② 回读 8.3MB → CPU → 工作线程
  ③ 工作线程（9070 XT）：上传 → lmxxf → 结果 → CPU
  ④ 下一帧：结果上传 → 7900 XT → 放大到 3840x2160 → 写进 pOutput
```

传输量：8.3MB 前向 + 8.3MB 回程 = **16.6MB ≈ 1.7ms**；加推理 13.8ms ≈ 15.5ms/帧 → NR 约 64fps。

后续增强（按价值排序）：
1. **喂 depth + velocity**（11MB + 22MB）→ 运行时的 detail lift 生效，画质才对得上 AMDNR
2. 试 2560x1440 档（日志显示该档存在：`the 2560x1440 network size is in force`）
3. 菜单（启停 / 分辨率档 / 强度 / 状态）

## 八、回退策略

代理里保留：**若我们的链路未就绪，照常调用真实 `xessD3D12Execute`** —— 游戏永远有正确的画面，最坏情况只是"没有 NR 效果"。
---

## 九、v3/v4 期间的关键实测（2026-10-24 深夜）

### 9.1 运行时只接受一种输入组合（除非先让 HIP 就绪）

```
PrepareFrame 失败 rc=5 err=codec unverified input format/geometry (2560x1440)
PrepareFrame 失败 rc=5 err=codec unverified input format/geometry (2227x1253)
PrepareFrame 失败 rc=5 err=codec unverified input format/geometry (3840x2160 fmt24 UAV)
```

- 1920×1080 + RGBA8 + COMMON → **接受**（网络 12.5ms/帧，真实推理）
- 其它几何（含 AMDNR 日志里的 `3840x2160 fmt 24 state 8` 配方）→ **全部拒绝**

### 9.2 根因：档位策略在 HIP 就绪前就锁定了 1080p

运行时状态行：
```
lmxxf modules_ok=192 hip=0 prepared=0 queue=1 weights=1     ← 我们（会话刚建立）
```
它自己的日志（AMDNR 的 host）：
```
lmxxf runtime up ... hip=0 prepared=0 queue=1 weights=1     ← 同一状态
lmxxf: first frame prepared ... hip=1                        ← 跑通第一帧后才 1
lmxxf: the 2560x1440 network size is in force (tier policy 4u) ← 之后档位才升
SetTierPolicy: the 2560x1440 network size needs ~%llu MB ... 
               and this GPU did not say how much is left      ← 查不到显存就留在 1080
```

**结论：必须先以 1920×1080 跑通第一帧让 HIP 初始化，档位才会升；档位升了 codec 才接受大几何。**

### 9.3 修法（下一步实现）

```
① 先按 1920×1080 建资源、跑通（当前已验证可用的组合）
② 运行中每 N 帧查 GetStatus，出现 hip=1 后
③ 重建资源，切到 1440p 或原生尺寸
④ 之后 depth/MV/look 控制才有意义
```

### 9.4 look 控制需要 144 字节

```
PrepareFrame: controls need struct_size 144
lmxxf: pre-block controls tone=%f structure=%f skin=%f other=%f style=%f - the network is rebuilt
```
FrameInfo 从 120 扩到 144（+depth +velocity +5 个控制量），已实现并验证被接受。

### 9.5 其它已验证事实

| 事 | 证据 |
|---|---|
| 我们写入 pOutput 就是最终画面（无覆盖） | 写成纯绿 → 用户确认整屏绿 |
| 描术符槽位错位会导致"整屏全黑" | base SRV 建在槽3、却读槽6 → 黑 |
| 同窗口不能有两个 flip 交换链 | `0x80070005` E_ACCESSDENIED |
| 合成交换链 + DComp overlay 可行 | present_probe4 绿屏验证通过 |
| color 的拷贝能抓到真数据（均值79） | 但 depth/MV 无论换几种状态都读回 0 |
| NR 的 edit 量级本身很小 | 运行时日志 fresh edit mean 0.014 max 0.236 |
---

## 十、2026-10-24 凌晨的最终突破与结论

### 10.1 depth / MV 能抓到！（此前的 0 是状态猜错）

改用 `PIXEL_SHADER_RESOURCE`（MV）+ `DEPTH_READ`（depth）后，同一命令列表里拷贝成功：

```
MV 校验:    平均|mv| 0.2  最大 5   (SNORM 32767=1.0，即最大约 5 像素)
Depth 校验: 均值 993027709  min=974247284  max=1004589452  (D32，约 0.227~0.234)
```

**这解决了"单卡 AMDNR 能抓、我们抓不到"的核心差异。**

### 10.2 HIP 其实会自己就绪

```
运行时状态 @200:  ... hip=1 ... net=1920x1080 hip_ms=9.5      ← 第一帧之后
运行时状态 @3600: ... hip=1 ... （稳定）
```
会话刚建立时 `hip=0` 只是瞬间状态；**跑通第一帧后就是 1**。
但档位仍是 `net=1920x1080` —— 档位跟随我们喂的几何，而不是反过来。

### 10.3 AMDNR 的真实配置（OptiScaler.ini [DlssNr]）

```
AmdPlacement = pre              ← 关键：NR 在上采样之前
LmxxfFullNetwork = true         AmdRefineMode = full
AmdNeuralLightingStrength = 1.15
AmdTemporalStability = 0.75
LocalStructure = 0.9            SkinStructure = 1.35
AmdLmxxfEditDetail = 0.85       EditSaturation = 0.9
EdgeGuard = 0.6                 OutputSmooth = 0.8
AmdResidualLimit = 0.2          AmdResidualIntensity = 0.85
NrBackend = daniel
```

另外 `[AmdLook]` 段（Tone / ExposureEV / Contrast / Saturation / HighlightCompression /
SpecularControl / LocalLighting）是**"色调光影"观感的来源**，与 NR 网络本身是两回事。

### 10.4 试过但失败的：pre 布局写回 pColor

把 NR 结果写回 pColor（上采样之前）→ **画面卡住 + 变成低分辨率死图**。
原因：改写了 pColor 后真实 XeSS 拿到的输入被破坏。
（AMDNR 的 pre 布局有它自己的合成方式，不能简单照搬）

### 10.5 两次"整屏异常"的直接原因（都是我的低级错）

| 症状 | 原因 |
|---|---|
| 整屏全黑 | base 的 SRV 建在描述符槽 3，却去读槽 6（空描述符） |
| 整屏全白 | 根签名要 8 个常量，只传了 4 个 → `gGamma=0` → `pow(x,0)=1` |
| 画面不变 | MV 掩码 `1-|mv|/2`，而 MV 是像素单位（最大 5）→ mask=0，edit 被乘 0 |

### 10.6 明天要做的（顺序明确）

1. **重写 depth/MV 通路**（v6 改坏了，需干净重写）：
   - 游戏侧：depth(D32S8) → 1080p R32_FLOAT；MV(SNORM) → 1080p SNORM（**数值要按分辨率比例缩放**）
   - 回读 → 跨卡 → worker 侧建同几何纹理 → 填 `fi.depth` / `fi.velocity`（偏移 +104 / +112）
2. **让运行时自己做时序校正**，删掉手写 MV 掩码
3. **实现 Look 层**（tone/对比/饱和/高光压缩），对齐 [AmdLook] 的观感
4. 不要再尝试 pre 布局写回 pColor（已验证会破坏 XeSS 输入）

---

## 十一、v7 定案：根因是「结果根本不在 fi.color 里」（2026-10-24 凌晨，离线探针实测）

### 11.1 决定性证据（`nr_probe_output.exe`，在 9070 XT 上 1:1 复刻调用链）

| 配置 | `fi.color` 回读 vs 输入 | `job.private_output` 回读 vs 输入 |
|---|---|---|
| fi=120 | **0 字节不同**（edit 恒 0） | edit 平均 **0.0261** 最大 0.1608，99.98% 像素变化 |
| fi=144（depth/MV 传 0，即 v6） | **0 字节不同** | edit 平均 **0.0305** 最大 0.3804 |
| fi=104 | PrepareFrame 拒绝：`struct_size mismatch` | — |

网络结果**只**写在 `job.private_output`（1920×1080、fmt=28 R8G8B8A8_UNORM、flags=0x4）。
v3~v6 一直回读 `W.color`，而它逐字节等于我们上传的输入 → `edit = 出 − 入 ≡ 0` → 画面当然不动。
这不是网络不干活，是我们把原图当结果搬来搬去。

数值核验（回读图转 PNG 后逐像素统计）：亮度相关 **0.98346**、8×8 块均值相关 **0.98345**、
全图均值只差 −0.23、亮度平均差 **4.81/255**、局部最大 **31.7/255**
→ `private_output` 是同一幅画面的合理精修，不是乱码/错位/色偏。

### 11.2 ABI 扫描（`nr_probe_abi.exe`，**每个配置独立建会话**）

v1 探针的教训：4K 输入会让会话 fatal（`codec unverified input format/geometry`）并进入
`session is poisoned`，之后所有调用都报 UNAVAILABLE，必须销毁重建 —— 否则后面全是假阴性。

- 合法 `struct_size` 只有 **120 / 144 / 160**；`104 / 152 / 168` 一律 `struct_size mismatch`。
- 运行时错误串证实 160 是「带历史掩码」的版本，另有两个我们一直传 NULL 的表项
  `GetImportPoolStats` / `SetRuntimeSettings`（调用返回 OK，但语义未知，正式版不碰）。
- **颜色像素预算 ≈ 1920×1080**：`1920x1080`、`1600x900` 通过；
  `2227x1253` 和 `3840x2160` 都 `codec unverified input format/geometry`。
  → **4K 原生颜色这条路在这份 pak 上不存在**（要走运行时的 NATIVE 模式，需要另一套 native 资产）。
- **motion 有用**：传 SNORM motion（2227×1253）后 `passes` 由 1 变 **3**，
  edit 由 0.0265/0.1686 变成 **0.0312/0.3961**。
- **depth 目前惰性**：`motion+depth(R32_FLOAT)`、`motion+depth(D32S8)` 与「只有 motion」
  输出逐字节同级、数字完全相同。
- **history 开不起来**：加历史掩码（+144，R16F/R8 @4K）、加 motion、加 depth 之后，
  `GetStatus` 始终是 `hist=off/0`。AMDNR 那套是 `hist=on+guard+disocc` —— 这是下一步专项。
- `flags` 的 0x001~0x080 全部被接受（不报 `unknown flags`）。
- 运行时认的环境变量（从 DLL 字符串里挖出来的，供后续调档）：
  `AMDNR_DEV_NETWORK_HEIGHT`、`AMDNR_NATIVE_PW/PH`、`AMDNR_HISTORY_DISOCC`、
  `AMDNR_HISTORY_CLAMP*`、`AMDNR_VIT_REUSE`、`AMDNR_ALLOW_SLOW_GPU` 等。

### 11.3 与 AMDNR（lmxxf 后端）实录对照

```
lmxxf: rebuild=tier frame 3840x2160 (colour allocation 3840x2160), network fed 1920x1080
       (edit lifted to the frame) | after upscaling: depth 2227x1253 resampled to the output
lmxxf inputs: colour fmt 24 state 8 | motion fmt 13 2227x1253 scale (1113.5,-626.5)
              | depth fmt 19 inverted | exposure none | jitter 0.4375,-0.3889
lmxxf: detail lift on - the edit is the network's answer minus what it was fed,
       lifted to the frame by a guided upsample
lmxxf: carry guard on - the one-frame-late edit is checked against the raw it was made for
stats: fresh edit mean 0.01405 max 0.2360 | keep mean 0.830 | mv mean 16.33 px
       | history hist=on+guard+disocc | controls tone 1, structure 1, skin 3, other 2, style 0
```

要点：**AMDNR 也是「取 private_output 的答案 − 喂进去的输入 = edit，再抬回帧尺寸」**，
和我们 v7 的架构一致；真正的差距只有两条：① 它有 history/carry（用 motion+depth 做历史
重投影和 keep 掩码）；② 它喂的是上采样后的清晰图。控制量恰好是 `ctl=1,1,3,2,0`（我们用的同一组）。

### 11.4 v7 实现要点（已实现并编译通过）

1. 结果取自 `job.private_output`，且必须在 `Retire` **之前**拷（`COMMON→COPY_SOURCE→COMMON`）。
2. 网络输入 = **XeSS 4K 输出经 2×2 盒式下采样到 1080p**（不再喂 pColor 的糊图；也避开像素预算）。
3. 叠加 = **UAV 原地写 outp**（`RWTexture2D` 读改写 + UAV barrier）：不需要猜 outp 当前是什么 state，
   也不再有 4K 中间纹理和整幅拷贝。
4. 跨卡传的 = 「本帧输出 − 本帧输入」的编码差分（`enc = diff + 128`，±0.5 量程、1/255 步长），
   顺带消掉 v6 的「N−2 帧结果减 N−1 帧输入」时间错位。
5. 接真 motion（默认开，F5 关）、depth（默认关，F4 开）。
6. 自证三件套：worker 日志打印真实 edit 的 mean/max；F6 增益 ×8；F3 往 outp 左上角画绿块。

### 11.5 还没做的（下一轮）

- **history / carry**：`hist=off` → 需要定位历史掩码与历史源的确切偏移，并找出打开它的开关
  （`SetRuntimeSettings`？），打开后 motion+depth 才会真正发挥时序作用。
- **引导式上采样**：现在差分抬回 4K 用双线性；AMDNR 用「guided upsample」保住边缘。
- 一帧以上的 edit 延迟（现在约 2 帧）需要靠 carry 补，否则快速平移会看到修正量拖影。

### 11.6 怎么稳定复现一张正确的对比图

完整步骤、验收数值标准和踩坑清单见 **[离线对比图-可复现流程.md](离线对比图-可复现流程.md)**（已验证通过）。
要点：抓帧版代理 → F2 或 capture_trigger.bat → 
r_from_capture.exe → 	ools/make_compare.py → 	ools/check_frame.py 验收。
