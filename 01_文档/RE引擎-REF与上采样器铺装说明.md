# RE 引擎：REFramework + 上采样器 铺装说明（含与 NR 的分层关系）

> 用途：给《生化危机》系列（RE2/RE3/RE4/RE8 等）铺好"上采样器接入点"，
> 之后再在其上选择一个 NR 实现。
> 依据：2026-10-26 对 RE2 现场文件的实测（字符串/日志/版本），非凭记忆。
> 关联：`E:\AI\跨卡NR\01_文档\社区DLSS5-知识库-20261026.md`（内部文档，未随仓库发布）、`E:\AI\跨卡NR\01_文档\跨卡NR-完整说明.md`

---

## 一、★ 三层模型（重要：前提 ≠ 功能 ≠ 实现）

```
第 0 层【前提】：给 RE 引擎"造出"上采样器调用
    dinput8.dll（REFramework，必须是 PD 定制版）
      └─ TemporalUpscaler 子系统，按文件名加载 PDPerfPlugin.dll
            └─ PDPerfPlugin.dll（UpscalerBasePlugin）
                  └─ 按名字加载具体实现：
                       libxess.dll            → XeSS
                       nvngx_dlss.dll / nvngx → DLSS（仅 NVIDIA）
                       ffx_fsr2_api_x64.dll、_dx11_/_dx12_ → FSR2
    ⇒ 这一层【只提供接入点】，它自己不做 NR。RE2/RE3 原生没有 XeSS，靠它造出来。

第 1 层【NR 功能】：谁截获上采样器调用，把神经渲染插进去
    · 我们的跨卡代理：替换 libxess.dll，在 xessD3D12Execute 里插 NR（+ 跨卡搬运）
    · AMDNR / OptiScaler：以 dxgi 代理身份 hook 同一个上采样器调用，中间插 NR
    ⇒ 这一层才是"NR 功能"；不同实现**互相替代**，不能同时跑两个。

第 2 层【运行时】：真正跑网络的 DLL / 权重
    lmxxf（LmxxfNrRuntime.dll + .pak）、mochizuki（MochizukiNrRuntime.dll + dlssnr-amd\）、
    danielblnc（dlssnr_amd_pass*.dll + weights）
```

**结论**：REF + XeSS 与 NR **不冲突**，前者是后者的前提；
冲突只会发生在"两个第 1 层同时抢同一个上采样器调用"。

---

## 二、第 0 层的组成与验证（实测）

### 2.1 文件清单（放在游戏 exe 同目录）

| 文件 | 作用 | 注意 |
|---|---|---|
| `dinput8.dll` | REFramework 本体（**PD 定制版**） | 官方版**没有** TemporalUpscaler，装它不会出现 XeSS 选项 |
| `PDPerfPlugin.dll` | 上采样器桥（UpscalerBasePlugin） | REF 按这个名字加载；约 264 KB，源自 NVIDIA Streamline 的 NativeRenderingPlugin 示例（2023-08-31 构建） |
| `libxess.dll` | XeSS 实现 | 我们用 XeSS **2.0.2.68**（74.19 MB）正常；`libxess.dll` 正是第 1 层的挂载点 |
| `nvngx_dlss.dll` / `nvngx.dll` | DLSS 实现（可选） | 仅 NVIDIA 卡 |
| `ffx_fsr2_api_x64.dll`、`_dx11_`、`_dx12_` | FSR2 实现（可选） | 想用 FSR2 就放 |

### 2.2 怎么确认第 0 层装对了

```
① 看 dinput8.dll 里有没有 "PDPerf" 引用（纯官方版 = 0 次）
     方法：字符串扫描；RE2 实测：当前版 4 次、pd版 4 次、官方版 0 次
② 看 REFramework 日志（re2_framework_log.txt / re8_framework_log.txt …）：
     出现 "[PluginLoader] Loading plugins..." 正常；
     若出现 "Could not load PDPerfPlugin.dll, TemporalUpscaler will not work"
     ⇒ PDPerfPlugin.dll 没放对位置或文件名不对
③ 进游戏 → REF 菜单 → Temporal Upscaler 区域能开；游戏图形设置里出现 XeSS 选项
```

### 2.3 PDPerfPlugin 实际用到的输入（说明它把"DLSS 那套输入"喂给 XeSS/FSR2）

```
Depth / MotionVectors / Jitter.Offset.X,Y / MV.Scale / ExposureTexture / Reset / ReactiveMask
Color·Depth·MV Subrect.Base.X,Y
DLSS.Hint.Render.Preset.DLAA | Quality | Balanced | Performance | UltraPerformance
Sharpness / Dynamic Min-Max Render Width,Height
⇒ 也就是说：第 1 层（会 hook 这个调用的人）能拿到 color/depth/motion/jitter 一整套输入
```

---

## 三、给其他 RE 作品的复用手册

```
① 铺第 0 层（与 NR 无关，装完游戏里就有 XeSS）
   · 放 PD 定制版 dinput8.dll
   · 放 PDPerfPlugin.dll
   · 放 libxess.dll（+ 可选 nvngx_dlss.dll / ffx_fsr2_*）
   · 启动确认：REF 菜单 Temporal Upscaler 可用 + 游戏里能选 XeSS

② 选第 1 层（只选一个）
   · 我们的跨卡代理：用它替换 libxess.dll（真 XeSS 改名为 libxess_orig.dll 供转发）
   · 或 AMDNR / OptiScaler：以 dxgi 代理身份工作
   · ⚠️ 两者不要同时启用，否则同一帧被两个 NR 各处理一遍

③ 选第 2 层（NR 运行时）
   · lmxxf：LmxxfNrRuntime.dll + LmxxfNrRuntime.pak
   · mochizuki：MochizukiNrRuntime.dll + dlssnr-amd\
   · danielblnc：dlssnr_amd_pass*.dll + dlssnr_on_amd_weights.bin

④ 想只要 XeSS 不要 NR（现在 RE2 的状态）
   撤掉第 1 层，把真 libxess.dll 放回去即可 —— 第 0 层仍在，XeSS 照常可用
```

### 3.1 各作品的注意点

```
· 引擎版本差异：RE2/RE3 是同一代 RE 引擎，REF 插件通用性较好；
  RE4/RE8 更新、改动更多，REF 与插件都要用对应版本，装前先备份
· 文件虚拟化：微软商店/Xbox 版会重定向写入 → 日志可能落在 _storage_ 目录
· 反作弊：在线游戏不要用
· 每次游戏本体更新后，REF 常常需要更新才能继续加载（现象：启动后崩溃/被反篡改拦）
  —— 这一点在 Resident Evil Requiem 上是已知要求（需 REF 才能过 Capcom 反篡改）
```

---

## 四、与 AMDNR 安装记录的关系（RE2 现状）

RE2 目录里还有 `amd-nr.install.json`（preset=OptiScaler、route=x64、ReShade 6.8.0 记录、
带 `OptiScaler/` 与 `dlssnr-amd/` 全套）—— 那是 **AMDNR Launcher** 装的**另一套第 1 层**。

```
· 它与第 0 层（REF+PDPerf+libxess）不冲突
· 它与我们的跨卡代理（也属于第 1 层）互斥
· 现状态：第 1 层全部停用（我们的代理改名为 .停用-跨卡DLL、dxgi.dll 不存在），
        第 0 层完好 → 游戏能正常开 XeSS，无 NR
```

---

## 五、备份与恢复（RE2 当前）

```
备份
  E:\AI\跨卡NR\04_备份\代理备份-RE2-20261026\     第 1 层（我们的代理）+ 真 XeSS + 运行时
  E:\AI\跨卡NR\04_备份\AMDNR备份-RE2-20261026\          第 1 层（AMDNR/OptiScaler）+ daniel 运行时
恢复第 1 层（二选一）
  del libxess.dll && ren libxess.dll.停用-跨卡DLL libxess.dll     # 我们的代理
  ren dxgi.dll.停用-测试中 dxgi.dll                              # AMDNR
```
