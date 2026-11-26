# AegisBlackout (黑幕神盾) v4.1.0

**Windows 11 24×7 GPU 硬件 DRM (`HW_PROTECTED | DISPLAY_ONLY`) + 全局无白名单 Special-APC 单窗隐身 (`WDA_EXCLUDEFROMCAPTURE`) + `GhostSwap v2.1` Fileless PIC / 0%频闪防拍黑幕 + `AegisVault` 隔离浏览 + Ring-0 UIPI (`High-IL`) & `SYSTEM` 自愈守护**

> **Zero Custom `.sys` Driver | VBS / HVCI & ACG / CFG Compatible | Global No-Whitelist Zero-Snow | Zero Hotkey Interference (`Ctrl+Alt+Q` Physical HID) | Cooperative Shield Mode**

AegisBlackout 是一个面向 Windows 11 的零内核驱动隐私防窥、防截屏与防录屏框架。**v4.1.0 在 v4.0 零注入 `NtQueueApcThreadEx2` (`QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC`) + `win32u!NtUserSetWindowDisplayAffinity` 的基础上完成三项核心进化**：
1. **全局无白名单零雪花单窗隐身（`AegisBlackout v4.1.0`）**：移除微信/QQ/终端等宿主限制，引入 `EVENT_OBJECT_CREATE/SHOW/LOCATIONCHANGE` 瞬时唤醒、`50ms` 快巡检与 `3` 次瞬态重试容错，新打开的 Chrome / Edge / Electron / Weixin / QQ 等全部 x64 程序毫秒自动隐身（`V11/V18 -> 0.00%` 且零雪花），并将 `Ctrl+Alt+Q` 截图前摘幕校验加固至 `150ms` 轮询 + 3× `DwmFlush()`，保证**全软件 0.00% 外部泄漏的同时，物理键 `Ctrl+Alt+Q` 仍可完整清晰截图**。
2. **`GhostSwap v2.1` 无文件 PIC DXGI 升维引擎与一键防偷拍黑幕**：内嵌纯 `.gpic` 位置无关代码（0 重定位、16 字节对齐，并通过 `SetProcessValidCallTargets` 注册 CFG 合法跳转目标），实证定位 AMD RX 9070 下 `0x420` 对 Chrome (`BGRA8 + Usage=0x30 SRV`) 产生雪花屏、而对微信 (`RGBA8`) 无雪花的底层根因，提出通用的零雪花 `0x20 DISPLAY_ONLY` 方案；默认以**协同护盾模式**（0% 进程注入、0 冲突，提供 `Ctrl+Alt+F9/F10/F11` **0% 频闪、0% 空闲占用的 100%/85%/65% 一键防手机偷拍黑幕**）无感驻留后台，也支持 `--lock-vram` 一键切换为连 `Ctrl+Alt+Q` 也截黑的极端显存焊死模式。
3. **`AegisVault` 独立硬件加密浏览沙箱**：内置基于 D3D11/D3D12 `0x420` 硬件保护表面的离线密保图/文档查看器。

---

## 三分钟小白科普：我们是怎么“白嫖”微软和 AMD 版权保护的？

如果你之前用过 **Netflix、Apple TV+ 或播放 4K 蓝光电影**，可能注意过一个现象：电脑屏幕上明明放着高清电影，但只要一按微信截屏、OBS 录屏或者远控，**画面立刻变成一块纯黑**。

这背后是微软（Windows DWM / DXGI 显示合成器）和显卡厂商（比如 AMD 的 **TMZ** Trusted Memory Zone 硬件信任内存区）联手修好的**“影视级硬件防盗版专用通道”（DRM / 受保护媒体路径）**：
- 视频帧在显存里勾选 `HW_PROTECTED | DISPLAY_ONLY`（标志位 `0x420`）后，**像素在显卡显存里就已经处于硬件密文状态**——实测拿另一个独立 D3D11 设备通过 `OpenSharedResource1 -> CopyResource` 硬抠显存，读出来的只有 4096+ 种高熵加密乱码，`0/16384` 明文匹配；
- 只有顺着显卡直连物理显示器的输出线时才在硬件管线末端解码亮在屏幕上，Windows 的一切系统截屏 API（`BitBlt`、`DXGI`、`WGC`、`PrintWindow`）会在内核/DWM/GPU 层直接被掐断返回纯黑。

以前这套机制专供好莱坞大片和付费流媒体防盗版。**AegisBlackout 的核心思路非常简单粗暴：不写高危未签名 `.sys` 驱动，也不去买昂贵的 PlayReady 商业版权证书——直接借壳“白嫖”微软和 AMD 现成的硬件防盗版保险箱，拿来保护你自己的隐私屏幕与私人文件！**

1. **第一招：把私密文件「直接塞进加密显存保险箱」（`AegisVault`）**
   想看敏感截图、证件或隐私文档？别放在普通窗口里裸奔。`AegisVault` 把内容直接传进 `0x420` 硬件加密显存交换链里渲染——你的肉眼通过显示器线缆正常浏览，但任何后台程序想读显存、抓句柄、截屏，拿到的都是加密乱码或纯黑；关闭窗口瞬间用 `SecureZeroMemory` 将主机内存里的明文像素与路径彻底抹零。
2. **第二招：「偷梁换柱」，把整个桌面和全部软件拉进防盗版专车道（`AegisBlackout` + `GhostSwap`）**
   - **借壳全屏数字版权幕布（`AegisBlackout`）**：好莱坞播放器只遮自己一个小窗口，我们直接把一张开着 `0x420` 硬件加密的 4K DirectComposition 透明幕布（透明度仅 `1/255`，鼠标键盘完全穿透）铺满整个桌面。你的眼睛完全感觉不到它，但它只要亮着，整个桌面在系统截屏和录屏眼里就等同于“正在播放加密 4K 蓝光”，直接触发全黑拦截；配合零注入 `Special-APC` 直接让目标进程自己调用微软签名 `win32u` 接口给自己挂上 `WDA_EXCLUDEFROMCAPTURE (0x11)`，连定向单窗抓拍也直接黑透且 **0% 雪花**。
   - **DXGI 交换链「偷梁换柱」（`GhostSwap` 可选极端模式）**：把本来只会出现在普通窗口里的 DirectX 渲染流程，在目标 GPU 进程创建与缩放交换链的瞬间用纯内存 `.gpic` 位置无关代码截胡，悄悄替换塞进 `0x20 DISPLAY_ONLY`“仅限物理显示器可见、禁止一切桌面回读”的版权专车道（不改动硬盘上的 `.exe` / `.dll`）。
3. **一句话看懂你的日常体验**：
   你平时看 4K 144Hz 桌面 **100% 原生清晰、零雪花、后台无黑框**；微信、QQ、OBS、系统 `Win+Shift+S`、远控木马截出来 **0.00% 全黑**；你自己按物理键盘 **`Ctrl + Alt + Q`**，护盾核验真人硬件按键后毫秒放行拍一张清晰全屏；身边真有人举手机偷拍，按 **`Ctrl + Alt + F9`** 瞬间降下 **0 频闪、0 占用的纯黑砖幕布**！

---

## English Overview

**AegisBlackout v4.1.0** is a driverless Windows 11 anti-screen-capture, anti-recording, and privacy curtain framework designed for VBS/HVCI, ACG/CIG/CFG, and competitive gaming/anti-cheat environments:

- **24×7 GPU Hardware-Protected DRM Overlay**: Creates a fullscreen `DirectComposition` (`IDCompositionDevice`) swap chain with `DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED | DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY` (`0x420`) backed by hardware-protected VRAM (`D3D11_RESOURCE_MISC_HW_PROTECTED`), gracefully falling back to `WS_EX_LAYERED` when unsupported.
- **Global No-Whitelist Zero-Injection Special-APC Cloak (Upgraded in v4.1.0)**: Uses `NtQueueApcThreadEx2(..., QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC, win32u!NtUserSetWindowDisplayAffinity, hwnd, WDA_EXCLUDEFROMCAPTURE, NULL)` directly on Microsoft-signed `win32u` syscall stubs — zero remote DLLs, zero remote allocations, zero `WriteProcessMemory`. Expanded globally across Chrome, Edge, Electron, WeChat/Weixin, QQ, terminals, and newly launched apps via `50ms` sweeps, `EVENT_OBJECT_CREATE` + `SHOW` + `LOCATIONCHANGE` hooks, and 3-attempt transient retry tolerance. Hardened `Ctrl+Alt+Q` capture unmasking (`150ms` affinity poll + 3× `DwmFlush()`) preserves clean privileged self-capture while keeping external captures at **0.00%**.
- **GhostSwap v2.1 (Fileless `.gpic` PIC Engine + 0%-Flicker Privacy Veil)**: Includes a relocation-free, 16-byte-aligned `.gpic` DXGI swap-chain vtable upgrader compatible with ACG/CIG (`NoRemoteImages`, `DynamicCode` policies permitting) and CFG (`SetProcessValidCallTargets`). Resolves AMD RX 9070 `BGRA8` vs `RGBA8` `HW_PROTECTED` snow artifacts using universal zero-snow `0x20 DISPLAY_ONLY`. Runs by default in **Cooperative Shield Mode** alongside `AegisBlackout v4.1.0` providing an event-driven 0%-flicker / 0%-idle-GPU solid blackout/dimmer veil (`Ctrl+Alt+F9/F10/F11`), with `--lock-vram` available for extreme lockout.
- **AegisVault (`src/AegisVault.cpp`)**: Standalone `HW_PROTECTED` GPU-encrypted media viewer.
- **Ring-0 `win32k.sys` UIPI + `NT AUTHORITY\SYSTEM` Watchdog**: Hardened LocalSystem service (`AegisBlackoutGuard`) launches `AegisBlackout.exe` at **High Integrity Level** in `winsta0\default` with zero UAC prompts at boot, strict process DACL, `ProcessExtensionPointDisablePolicy`, `ProcessImageLoadPolicy`, and NTFS read-only ACLs.
- **Zero-Silent Hardware-Only Raw Input**: Passive `RegisterRawInputDevices` (`RIDEV_INPUTSINK`) verifies physical HID/ACPI/USB device handles — blocking synthetic `SendInput` / `keybd_event` / RDP spoofing while registering zero hotkeys in `AegisBlackout.exe`.

---

## 五层防御体系架构

| 防御维度 | 底层机制 | 安全效果 |
| :--- | :--- | :--- |
| **第一层：24×7 GPU 硬件 DRM** | `D3D11` + `DirectComposition` + `DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED \| DISPLAY_ONLY` | 显存开启硬件内容加密；DXGI 桌面复制触发 `ProtectedContentMaskedOut`，`DwmGetDxSharedSurface` 拒绝导出句柄 |
| **第二层：DWM 全屏双锁幕布** | 全屏置顶 `WDA_MONITOR` + `WS_EX_NOREDIRECTIONBITMAP` + `WS_EX_TRANSPARENT` + 100ms 看门狗 | 完全穿透点击不影响操作；GDI `BitBlt` / `GetPixel` / `PrintWindow` / WGC Monitor 均捕获纯黑；自动压回抢置顶弹窗 |
| **第三层：零注入单窗隐身 (v4 新增)** | `NtQueueApcThreadEx2` (`SPECIAL_USER_APC`) -> `win32u!NtUserSetWindowDisplayAffinity(0x11)` | 零 DLL/Shellcode/远线程/远程内存分配，兼容 Chromium ACG/CFG；宿主机 `PW_RENDERFULLCONTENT` 降至 `0.00%`，`WGC CreateForWindow` 降至 `0.00%–0.42%`；`Ctrl+Alt+Q` 时原子临时摘除并在退出时干净还原 |
| **第四层：Ring-0 UIPI + 内核自卫** | `SYSTEM` 服务托管拉起 `High-IL` + `D:P` 进程 DACL + `ExtPt`/`ImgLoad` 缓解 + NTFS 只读锁 | 宿主机普通（Medium-IL）软件 `OpenProcess(TERMINATE/VM_READ/VM_WRITE)` 全部 `ERROR_ACCESS_DENIED (5)`；阻断跨权限窗口控制 |
| **第五层：零静默物理门禁截屏** | `RegisterRawInputDevices` 校验物理 HID 句柄 + `>=150ms` 可视 HUD 门禁 + `SecureZeroMemory` 擦除 | 彻底杜绝静默/后台代码触发截屏；必须人类物理按 `Ctrl + Alt + Q` 并双击或按 `Enter` 确认，关闭瞬间安全清零 4K 内存缓冲 |

---

## 全向量攻防与硬件 DRM 实测结果 (Capture & Red-Team Audit)

使用本仓库自带的 [`CaptureAuditSuite.cpp`](src/CaptureAuditSuite.cpp) 在 **Windows 11 25H2 (3840×2160) + AMD Radeon RX 9070** 实机实测数据如下：

### Group A & B：宿主机桌面截屏 / 现代 GPU 连续录屏测试

| 编号 | 攻击 / 采集方式 | 实测非黑像素率 | 结果 |
| :--- | :--- | :--- | :--- |
| `V01–V05` | GDI `BitBlt(SRCCOPY / CAPTUREBLT)` / `StretchBlt` / `CreateDC("DISPLAY" / "\\\\.\\DISPLAY1")` | **0.00%** | 拦截（全黑） |
| `V06` | `GetPixel(GetDC(NULL))` 20×20 全屏采样网格 | **0.00%** | 拦截（全黑） |
| `V07–V10` | 目标窗口 `GetDC` / `GetWindowDC` / `PrintWindow(0)` / `PrintWindow(PW_CLIENTONLY)` | **0.00%** | 拦截（全黑） |
| `V11` | **宿主机单窗定向抓取 `PrintWindow(PW_RENDERFULLCONTENT=2)` (v4 新增实证)** | **100.00% -> 0.00%** | Win32 与 Chrome 实测均降至 **0.00%**（无需沙箱） |
| `V12` | DXGI 1.2 `DuplicateOutput`（30 帧连续桌面流） | **0.00%**（`maskedOut=26`） | 拦截（全黑） |
| `V13` | DXGI 1.5 `DuplicateOutput1` | **0.00%**（`maskedOut=1`） | 拦截（全黑） |
| `V14` | Direct3D 9 `GetFrontBufferData` | **0.00%** | 拦截（全黑） |
| `V15–V16` | Magnification API（常规 & `MW_FILTERMODE_EXCLUDE` 主动排除保护窗） | **0.00%** | 拦截（全黑） |
| `V17` | Windows Graphics Capture `CreateForMonitor`（10 帧连续录屏） | **0.00%**（仅鼠标光标微点） | 拦截（全黑） |
| `V18` | **宿主机单窗 WGC `CreateForWindow(target_hwnd)` (v4 新增实证)** | Win32 **99.94% -> 0.00%**<br>Chrome **99.99% -> 0.40%–0.42%** | Win32 为 0 帧全黑；Chrome（含 ACG/CFG）仅剩 DWM 非客户区 1px 投影边，正文 **0 像素** |
| `V19` | `user32!DwmGetDxSharedSurface` 共享表面读取 | **0.00%**（对 DComp/Chrome/护盾） | `ok=0, hSurf=NULL` 拒绝导出（传统 GDI 窗口见下方边界说明） |
| `V20` | 20 帧连续 GDI `gdigrab` 录屏循环 | **0.00%** | 拦截（全黑） |

### Group C & E：红队对抗与独立 GPU 设备硬件 DRM 实证

| 编号 | 红队对抗 / 硬件验证项目 | 实测结果 |
| :--- | :--- | :--- |
| `V21` | 旧版全局 IPC 后门事件探测 | **PASS**（零全局 IPC 句柄） |
| `V22` | `OpenProcess(TERMINATE / VM_READ / VM_WRITE \| CREATE_THREAD)` 强杀/转储/注入 | **PASS**（全部返回 `ERROR_ACCESS_DENIED (5)`） |
| `V23` | 特征窗口类名 / 标题指纹扫描 | **PASS**（`0/5` 命中，每次启动随机类名 + 空标题） |
| `V24` | 窗体隐藏 / 移出屏幕 / 降 Z 序 / `WM_CLOSE` 篡改攻击 | **PASS**（维持 `3840x2160` + `WDA_MONITOR` + `0.00%`） |
| `V25` | 合成按键 `SendInput` 伪造截屏或紧急退出热键 | **PASS**（直接拒收，漏光 `0.00%`，护盾存活） |
| `V26` | 全局热键占用扫描（`Alt+A` / `Ctrl+Alt+A` / `Ctrl+Alt+S` / `Ctrl+Alt+X`） | **PASS**（`4/4 FREE(0)`，零冲突零指纹） |
| `V27` | 600×600 高亮红色 `WS_EX_TOPMOST` 抢置顶劫持 | **PASS**（瞬间压回底层，GDI/DXGI 漏光均 `0.00%`） |
| `V29` | **独立 D3D11 设备 `CopyResource` 与硬件 DRM 密文实证** | **PASS**：`0x420` 交换链无法导出共享句柄 (`0x80070057`)，同设备及跨独立 Device B (`OpenSharedResource1`) 强制 `CopyResource -> Map` 均只读出高熵硬件密文（`0/16384` 明文匹配，`4096+` 杂乱像素；未保护对照组为 `16384/16384` 纯明文） |

---

## 安全边界与坦诚说明 (Threat Model)

1. **传统 GDI 重定向表面 (`DwmGetDxSharedSurface` / `V19`)**：
   Modern Chromium / Electron / DirectComposition 渲染窗口的 `DwmGetDxSharedSurface` 本身返回 `ok=0 (0.00%)`，叠加 v4 零注入 `WDA_EXCLUDEFROMCAPTURE` 后已全面封堵 `V11/V18/V19`；但若目标是一个老旧的纯传统 GDI 绘制窗口（非 DirectComposition），Windows 在赋予 `WDA_MONITOR`/`WDA_EXCLUDEFROMCAPTURE` 后，未公开接口 `DwmGetDxSharedSurface` 仍可能访问其旧式 GDI 重定向表面。
2. **高风险扫盘客户端的磁盘隔离**：
   单窗隐身层负责防像素抓取，**防扫盘与彻底隔离高风险桌面客户端**仍建议按 [`configs/Sandboxie-Hardening-Template.ini`](configs/Sandboxie-Hardening-Template.ini) 配合 Sandboxie-Plus 使用，在 Ring-0 拦截敏感目录遍历与系统捕获 DLL 加载。
3. **同桌面窗口几何矩形元数据 (`GetWindowRect`)**：
   在共享交互桌面 (`WinSta0\Default`) 下，即便像素与标题全被阻断，Win32 链表遍历 / UIA 仍可读取窗口的外框坐标和宽高。若需隐藏窗口矩形尺寸，需使用独立 Win32 Desktop 或虚拟机。
4. **目标保护豁免策略**：
   为保障系统稳定性与反作弊兼容性，v4.1 零注入单窗隐身引擎默认**跳过** WOW64 32 位进程、OS 核心桌面壳（`Progman`/`WorkerW`/`Shell_TrayWnd` 等）、`Sandbox:` 沙箱窗口、保护/系统进程、GhostSwap 自身遮罩以及主流游戏/反作弊进程，且仅对当前 `affinity == 0` 的宿主原生 x64 顶层窗口生效（不覆盖软件自带的 DRM 标志）。
5. **物理键盘特权截图 (`Ctrl + Alt + Q`) 的两个残余攻击面与对抗实证**：
   - **Windows 全局剪贴板无隔离泄密**：用户敲下回车将截图写入系统剪贴板（`CF_DIB`）的一刻，同桌面任意 Medium-IL 后台进程可通过 `AddClipboardFormatListener` / `GetClipboardData` 实时读走；
   - **~25ms 摘幕竞态空窗**：虽无法被远程或 `SendInput` 触发，但若后台潜伏 60–144fps 死循环录屏木马，存在在用户亲手触发 `Ctrl + Alt + Q` 的数十毫秒摘幕瞬间蹭拍到 1–2 帧的概率（极高敏感场景可通过 `GhostSwap.exe --lock-vram` 焊死 `0x20` 彻底杜绝）；
   - **物理防手机偷拍的光学极限**：室内手机自动拍照快门（约 `1/30s` + 多帧 HDR）积分慢于人眼暂留，任何不闪瞎人眼的软件 144Hz 频闪均会被手机自动平均还原；因此 `GhostSwap v2.1` 弃用高频频闪，采用 **0% 频闪、0% 空闲占用的瞬间全黑/极暗黑幕 (`Ctrl+Alt+F9/F10/F11`)**。

---

## 下期开发计划 (Roadmap)

- **无驱动「键盘防记录 + 剪贴板防窃取」防护体系**：
  - 针对 `WH_KEYBOARD_LL` 全局钩子、`GetAsyncKeyState` 轮询、`RegisterRawInputDevices(RIDEV_INPUTSINK)` 后台监听的键盘防记录对抗；
  - 针对 `AddClipboardFormatListener` / `GetClipboardData` 的剪贴板隐私锁、特权截图安全落地与延时自毁机制。

---

## 快速上手 (Quick Start)

### 方式 A：直接下载预编译 Release（推荐）
1. 前往 [**GitHub Releases**](../../releases/latest) 下载 `AegisBlackout-v4.1.0-win64.zip` 并解压到独立目录（例如 `C:\Program Files\AegisBlackout\`，建议目录本身仅限管理员写入）。
2. 验证校验和（可选）：核对 `SHA256SUMS.txt`。
3. 右键管理员运行（或直接双击触发 UAC 提权）**`scripts\install_service.bat`**：
   - 自动锁定 `AegisBlackout.exe` / `AegisBlackoutSvc.exe` 为 NTFS 只读权限；
   - 注册并加固 `AegisBlackoutGuard`（LocalSystem 开机自启 + 断线 1 秒拉起 + DACL 防普通进程停服）；
   - 拉起 **High-IL UIPI + 24×7 GPU HW-DRM + 全局无白名单单窗隐身** 护盾并回显 `aegis_init.log`。
4. **（可选伴侣部署）** 运行 **`scripts\install_ghostswap.bat`** 部署静默无框的 `GhostSwap v2.1` 开机自启（默认协同护盾模式，提供 `Ctrl+Alt+F9/F10/F11` 防偷拍黑幕；可用 **`scripts\manage_ghostswap.bat`** 一键查看状态或切换 `--lock-vram` 极端焊死模式）。
5. 查看运行状态运行 `scripts\check_status.bat`；如需完整卸载运行 `scripts\uninstall_service.bat`。

### 方式 B：从源码本地编译
环境要求：**Windows 10/11 x64 SDK + MSVC Build Tools（VS 2019 / 2022 / 18+）**
```bat
git clone https://github.com/verg-fe/AegisBlackout.git
cd AegisBlackout
build.bat
```
编译产物将自动输出至 `build\`：
- `build\AegisBlackout.exe` —— 核心护盾主程序（24×7 HW-DRM + Dual-Lock + 全局无白名单 Special-APC 单窗隐身 + 物理级防静默截图）
- `build\AegisBlackoutSvc.exe` —— Session 0 `SYSTEM` 级防死守护服务
- `build\CaptureAuditSuite.exe` —— 29 项全向量截屏、红队自卫与 GPU DRM 硬件实证审计工具
- `build\GhostSwap.exe` —— `GhostSwap v2.1` Fileless `.gpic` DXGI 升维引擎 & 0% 频闪防偷拍黑幕后台
- `build\AegisVault.exe` —— 基于 `0x420` HW-DRM 交换链的安全防窥看图隔离舱

---

## 物理硬件与防拍快捷键说明

> `AegisBlackout.exe` 全部基于 `RegisterRawInputDevices` 硬件原始输入被动观测，**不调用 `RegisterHotKey`**，不会与任何第三方软件或游戏的组合键产生占位冲突。

| 快捷键 | 所属模块 | 功能 | 说明 |
| :--- | :--- | :--- | :--- |
| **`Ctrl + Alt + Q`** | `AegisBlackout` | **零静默特权截屏** | 仅响应真实物理键盘；原子临时还原单窗 WDA 并隐藏全屏幕布（`150ms` 校验 + 3× `DwmFlush()`）抓取干净桌面后瞬间恢复全部防护，开启受 `WDA_MONITOR` 保护的 4K 暗化选区画布，经物理双击或 `Enter` 确认写入剪贴板，关闭瞬间 `SecureZeroMemory` 擦除缓冲 |
| **`Ctrl + Alt + F9`** | `GhostSwap` | **瞬间防偷拍黑幕开关** | 0% 频闪、0% 空闲 GPU 占用的顶层黑幕，开启瞬间防手机直拍 |
| **`Ctrl + Alt + F10 / F11`** | `GhostSwap` | **黑幕遮罩浓度调节** | `F10` 快速切换 `100% 纯黑防拍` / `85% 极暗防窥`；`F11` 循环 `100% / 85% / 65% 低亮护眼` |
| **`Ctrl + Alt + Shift + End`** | `AegisBlackout` | **紧急干净退出（免 UAC）** | 仅响应真实物理键盘；自动还原所有受保护窗口的 `WDA_NONE` 状态，并返回专属退出码 `0x5A5A` 通知 `AegisBlackoutGuard` 服务优雅停止 |

---

## 审计工具 CLI 用法

```bat
:: 在宿主机运行全部 GDI / GPU / 红队 / 硬件DRM 审计 (V01-V29)
build\CaptureAuditSuite.exe --mode host

:: 单独对着某个宿主或隔离窗体实测 PW_RENDERFULLCONTENT / WGC 单窗泄漏情况
build\CaptureAuditSuite.exe --mode sandbox --target-hwnd 0x001204A0
```

---

## 免责声明 (Disclaimer)

本项目仅用于个人隐私防护、桌面安全架构研究及授权环境下的防御有效性评估。请勿将本工具用于未授权的系统控制或对抗合规审计策略；使用本软件即表示您理解并同意自行承担配置与使用产生的全部责任。

## License

[MIT License](LICENSE) © 2026 verg-fe
