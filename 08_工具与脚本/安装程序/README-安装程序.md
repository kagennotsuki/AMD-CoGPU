# AMD CoGPU 安装程序 · 说明

> ⚠ 命名统一（2026-10-29）：
> · **自托管模式** = 作者托管 `runtime.zip`，清单填 `selfHosted.url`，安装器优先下载它（旧的「方案 B」）
> · **零分发模式** = 发布包不含运行时，安装器只做「用户自备 / 指向上游」，作者不托管任何第三方内容
> 详见 `06_交接包\安装器-交接包\交接-零分发模式与安装器问题-20261029.md`

> ⚠ **命名与待办（2026-10-29，由对外发布那条线转入）**
>
> 本文下方第 6 行的「方案 B：自托管壳版本」，在对外发布口径里已改称为 **「自托管模式」**；
> 与之相对的新策略叫 **「零分发模式」**（发布包不含运行时，安装器只从上游下载）。
>
> 两者的取舍理由，以及**待处理的 4 个安装器问题**（1 个阻断 + 1 个会让用户以为卡死的
> UI bug + 2 个一致性项），见：
> `06_交接包\安装器-交接包\交接-零分发模式与安装器问题-20261029.md`
>
> **本文件正文暂未改动**，留给安装器那边的对话统一处理；命名口径也请那边一并统一。

## 给终端用户
双击 `启动安装.bat`，按向导走 8 步即可。安装包只有约 800 KB，运行时（约 900MB）会在安装时从作者仓库下载。

## 运行时分发策略（两种模式，按发布策略选一种）

```
为什么自托管：
  我们的 libxess.dll 通过 LmxxfNrGetApi 这个 ABI 调用运行时，
  需要的文件是 LmxxfNrRuntime.dll + LmxxfNrRuntime.pak（俗称"外壳"版本）。
  上游 mochizuki0323/DLSSNR-AMD 的官方 Windows 包是"原生产品"
  （dlssnr-amd.asi + CC_* 模型 + 自己的安装器），不含这两个文件名
  ⇒ 直接装上游包会缺文件、跑不起来。

  实测：LmxxfNrRuntime.dll 与 MochizukiNrRuntime.dll 是同一份文件（同 SHA256），
        同一个 DLL 同时导出 LmxxfNrGetApi 与 MochizukiNrGet*/SetControls，
        内部含 CC_SMALL_FP8 / CC_SILVER_AARDWOLD 模型名
        ⇒ 它是"mochizuki 网络 + lmxxf 形态 ABI"的双兼容构建。

做法（方案 B）：
  · 我们自托管一份"已验证可跑"的壳版本包，安装器优先下载它
  · 上游地址与作者/MIT 许可信息照样写在清单里（透明、可追溯）
  · 安装器解压后会检查 proxyRequires（LmxxfNrRuntime.dll/.pak）：
    缺任何一个就明确报"与代理不兼容"，绝不假装安装成功
```

## 给作者（发布自托管运行时）
1) 生成运行时分发包 + 自动改写清单（一条命令）：
   ```powershell
   powershell -ExecutionPolicy Bypass -File 发布运行时.ps1 `
       -SourceGameDir "E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2" `
       -RepoUrl "https://github.com/<账号>/<仓库>/releases/download/runtime-v1.0/runtime.zip"
   ```
   可选备用镜像（国内拉 GitHub 慢时自动回退）：
   ```
   -Mirror "https://ghproxy.net/https://github.com/<账号>/<仓库>/releases/download/runtime-v1.0/runtime.zip"
   ```
2) 把 `dist\runtime.zip` 上传到该 Release，确认附件地址与 `-RepoUrl` 完全一致。
3) 脚本已经自动把 url / size / sha256 写进 `runtime-manifest.json`，并跑过自检。
4) 分发：把本目录（除 `dist\`）打包给用户。

## 目录内容
| 文件 | 作用 |
|---|---|
| `启动安装.bat` | 双击启动（自动选 pwsh / Windows PowerShell）|
| `AMD-CoGPU-Setup.ps1` | 向导本体：8 页 + 运行时下载/校验/解压 + 生成使用说明与安装报告 |
| `runtime-manifest.json` | 运行时分发清单（作者维护，用户不碰）|
| `发布运行时.ps1` | 打包 zip + 算 SHA256 + 改写清单 + 自检 |
| `payload\libxess.dll` | 跨卡代理（定版）|
| `payload\xess_proxy.saved.ini` | 参数基线（gateK=15 / mvConf=2.0）|

## 自检
```powershell
powershell -ExecutionPolicy Bypass -File AMD-CoGPU-Setup.ps1 -SelfTest
```
会检查：安装包完整性 / 运行时清单状态 / 显卡枚举（DeviceId + 真实显存）/ 游戏目录 / 干跑生成配置与报告。

## 向导 8 页
1. 游戏内用法（Del / F10 / 小键盘1 或 **End**：没有小键盘的键盘按 End，NumLock 关闭时两者同键）
2. 选择游戏目录
3. 游戏是否有超分功能（否则拒绝安装）
4. 选择游戏卡与神经卡（强烈建议 9000 系）
5. 安装设定（形态 / 队列钩子）
6. **运行时**（自动：已有就跳过，没有从作者仓库下载；可强制下载或跳过）
7. 安装（备份 → 复制 → 写配置 → 下载运行时 → 出报告）
8. 完成（结果摘要 + 打开游戏目录）

## 安装器写入游戏目录的配置
| 文件 | 内容 |
|---|---|
| `xess_proxy.bform.txt` | `9` = B-4K（推荐）／`0` = A 形态 |
| `xess_proxy.queuehook.txt` | `1`（把 33.2MB 跨卡拷贝移出游戏队列）|
| `xess_proxy.gpu.txt` | `nr=dev:<神经卡 DeviceId>`（DLL 启动时按它精确选卡）|
| `xess_proxy.dumpshot.txt` | `0 0`（关闭抓帧）|
## 第三方组件与许可（重要）

| 组件 | 归属 | 许可 | 我们是否分发 |
|---|---|---|---|
| `libxess.dll`（跨卡代理） | AMD CoGPU 项目 | 我们自己的 | ✅ 安装包内 |
| `LmxxfNrRuntime.dll` / `.pak`、`dlssnr-amd\`（模型） | **mochizuki0323** | **MIT** | ✅ 由安装器下载，**必须保留许可原文** |
| `libxess_orig.dll`（Intel XeSS 运行库） | Intel Corporation | Intel XeSS SDK | ❌ 不分发（游戏自带，安装器不碰） |

MIT 的义务是「保留版权声明与许可声明」：
- 运行时包里已带 `dlssnr-amd\LICENSE-DLSSNR-AMD.txt`，安装后保留在原位，**不要删**
- 安装器会额外生成 `AMD-CoGPU-第三方组件声明.txt`（内含 MIT 许可原文）
- 安装后校验会检查该许可文件是否存在（`runtime-manifest.json` 的 `expect`）

**分发前请确认**：如果你的镜像地址指向你自己托管的 `runtime.zip`，这个 zip 里必须包含 `dlssnr-amd\LICENSE-DLSSNR-AMD.txt`（`发布运行时.ps1` 打包整个目录时会自动带上）。
## payload\ 与 payload\runtime\ 是什么

```
payload\                    安装器自带的"行李"，安装时复制进游戏目录
  ├─ libxess.dll            跨卡代理（必备，约 790KB）
  ├─ xess_proxy.saved.ini   参数基线（可选）
  └─ runtime\               运行时的本地副本（可选，约 915MB）
       ├─ LmxxfNrRuntime.dll / MochizukiNrRuntime.dll   1.4MB ×2
       ├─ LmxxfNrRuntime.pak                            486MB
       ├─ dlssnr-amd\（846 文件，含 MIT 许可原文）        395MB
       └─ person-model\（12 文件）                       32MB
```

**运行时来源优先级（auto 模式）**

```
① 目标游戏目录已有运行时        → 跳过（0 流量）
② payload\runtime\ 存在         → 本地复制（离线安装，不联网）
③ 清单 selfHosted.url           → 从作者仓库下载（校验 SHA256）
④ 清单 components[].url（上游）  → 从上游下载（可能触发兼容性检查失败）

download 模式：强制走 ③/④，忽略 ①②
skip 模式    ：什么都不装
```

> 注意：`payload\runtime\` 与 `runtime-manifest.json` 是**两种互斥来源**，同时存在时本地优先。
> 现在本机已铺好 `payload\runtime\`（915MB），所以本机安装全程不联网；发给别人时可以把 `payload\runtime\` 删掉，让他们走下载。

## 无界面安装（测试 / 批量部署）

```powershell
# 只装代理与配置（跳过运行时）
powershell -ExecutionPolicy Bypass -File AMD-CoGPU-Setup.ps1 -InstallTo "E:\test-game" -RuntimeMode skip

# 完整安装（有 payload\runtime 就本地复制，否则下载）
powershell -ExecutionPolicy Bypass -File AMD-CoGPU-Setup.ps1 -InstallTo "E:\test-game" -RuntimeMode auto

# 强制下载（忽略本地副本）
powershell -ExecutionPolicy Bypass -File AMD-CoGPU-Setup.ps1 -InstallTo "E:\test-game" -RuntimeMode download

# 其它参数：-NrDev 7550  -Bform 9|0
```
## 第 1 页直接卸载（2026-10-29 新增）

安装器**第 1 页（用法页）底部**现在带一个卸载入口，不必再单独跑脚本：

```
已经装过了？需要卸载 / 恢复原版 XeSS
  ☑ 同时删除运行时（LmxxfNrRuntime.pak 等，约 915MB）   ← 默认勾选
  [ 选择游戏目录并卸载… ]
```

### 用法

```
1. 点「选择游戏目录并卸载…」→ 选到游戏根目录（含 re2.exe / re4.exe 那层）
2. 确认框会告诉你将删除什么（"删除代理 + 配置 + 运行时" 或 "保留运行时"）
3. 点「是」执行，完成后弹结果框，列出实际删除了哪些项
```

### 会删除的内容

| 类别 | 具体文件 |
|---|---|
| 配置 | `xess_proxy.bform.txt` / `.queuehook.txt` / `.gpu.txt` / `.dumpshot.txt` / `.saved.ini` / `.model.txt` / `.taa.txt` / `.diag5.txt` / `.mirror.txt` / `.resid4k.txt` / `.bwtest.txt` / `.add4k.hlsl` |
| 文档 | `AMD-CoGPU-使用说明.txt`、`AMD-CoGPU-安装报告-*.txt`、`AMD-CoGPU-第三方组件声明.txt` |
| 署名 | `licenses\`（lmxxf MIT / AMDNR RDNA3 MIT / pak 声明照录）|
| 运行时（可保留）| `LmxxfNrRuntime.dll`、`LmxxfNrRuntime.pak`、`MochizukiNrRuntime.dll`、`dlssnr-amd\`、`person-model\` |
| 备份 | `libxess.dll.bak-setup-*` |

### 会恢复的内容

```
libxess_orig.dll  →  改回 libxess.dll（原版 Intel XeSS）
若没有 libxess_orig.dll，则尝试用 libxess.dll.bak-setup-* 里最近的一份恢复
```

### 不会被碰的内容

```
· 游戏本体文件（re2.exe / re4.exe、pak、存档、REFramework 等）
· 日志（xess_proxy.log、mochizuki_nr.log）—— 留作排查证据，启动时会被覆盖
· 显式取消勾选「同时删除运行时」时，915MB 运行时保持原位（下次装更快）
```

### 命令行等价用法（批量/自动化）

```powershell
powershell -ExecutionPolicy Bypass -File 卸载.ps1 -GameDir "E:\...\RESIDENT EVIL 4  BIOHAZARD RE4"
powershell -ExecutionPolicy Bypass -File 卸载.ps1 -GameDir "..." -KeepRuntime      # 保留运行时
powershell -ExecutionPolicy Bypass -File 卸载.ps1 -GameDir "..." -WhatIf          # 演练，只列不删
```

### 已验证

冒烟测试里加了一条**装 → 卸 往返验证**（沙箱目录，不碰真实游戏）：

```
-- 装→卸 往返验证（沙箱）--
   卸载 5 项；残留=无；原版恢复=True
```

运行：`powershell -ExecutionPolicy Bypass -File AMD-CoGPU-Setup.ps1 -UiSmokeTest`

