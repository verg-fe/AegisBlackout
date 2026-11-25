# AegisBlackout (黑幕神盾) v4.0

**Windows 11 24×7 GPU 硬件 DRM (`HW_PROTECTED | DISPLAY_ONLY`) + 零注入 Special-APC 单窗隐身 (`WDA_EXCLUDEFROMCAPTURE`) + `WDA_MONITOR` + Ring-0 UIPI (`High-IL`) + `SYSTEM` 自愈守护反截屏与反录屏护盾**

> **Zero Custom `.sys` Driver | Zero DLL Injection | VBS / HVCI & ACG / CFG Compatible | Zero Hotkey Occupation (`RegisterHotKey = 0`) | Zero-Silent Physical Hardware Snipping**

AegisBlackout 是一个面向 Windows 11 的零内核驱动、零 DLL 注入隐私防窥与防录屏框架。**v4.0 在 24×7 GPU 硬件受保护显示叠加层、全屏 DWM 遮罩与 `High-IL` UIPI 隔离的基础上，新增第五道防线——基于 `NtQueueApcThreadEx2` (`QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC`) + 原生 `win32u!NtUserSetWindowDisplayAffinity` 的“零注入单窗隐身引擎”**：无需任何 `VirtualAllocEx`、`WriteProcessMemory`、`CreateRemoteThread` 或 DLL 注入，即可在 16ms 内为宿主机运行中的原生 x64 应用（含启用了 ACG/CIG/CFG 的 Chromium 系浏览器与桌面应用）动态施加 `WDA_EXCLUDEFROMCAPTURE` 保护，从而在**不依赖沙箱的纯宿主机环境下**将定向单窗抓拍（`PrintWindow(PW_RENDERFULLCONTENT)` 与 `WGC CreateForWindow`）的像素泄漏压制至 `0.00%`（仅 DWM 非客户区 1px 阴影边框 `~0.4%`，正文 0 像素）。

---

## English Overview

**AegisBlackout v4.0** is a driverless, injectionless Windows 11 anti-screen-capture and anti-recording shield engineered for strict compatibility with VBS/HVCI, ACG/CIG/CFG, and anti-cheat environments:

- **24×7 GPU Hardware-Protected DRM Overlay**: Creates a fullscreen `DirectComposition` (`IDCompositionDevice`) swap chain with `DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED | DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY` (`0x420`) backed by hardware-protected VRAM (`D3D11_RESOURCE_MISC_HW_PROTECTED`), automatically falling back to `WS_EX_LAYERED` if unavailable.
- **Zero-Injection Special-APC Per-Window Cloak (New in v4.0)**: Uses `NtQueueApcThreadEx2(..., QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC, win32u!NtUserSetWindowDisplayAffinity, hwnd, WDA_EXCLUDEFROMCAPTURE, NULL)` directly on Microsoft-signed `win32u` syscall stubs — zero remote allocations, zero `WriteProcessMemory`, zero `CreateRemoteThread`. Seals bare-metal per-window `PrintWindow(PW_RENDERFULLCONTENT)` and `WGC CreateForWindow`, atomically uncloaks/recloaks during `Ctrl+Alt+Q`, and cleanly restores `WDA_NONE` on exit.
- **Dual-Lock Click-Through Curtain**: Combines `WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE` (`WM_NCHITTEST -> HTTRANSPARENT`) with `SetWindowDisplayAffinity(WDA_MONITOR)`, an active `100ms` Z-order watchdog, and `SetWinEventHook`.
- **Ring-0 `win32k.sys` UIPI + `NT AUTHORITY\SYSTEM` Watchdog**: A hardened LocalSystem service (`AegisBlackoutGuard`) acquires the active console user's elevated `TokenLinkedToken` to spawn `AegisBlackout.exe` at **High Integrity Level** in `winsta0\default` with zero UAC prompts at boot, Protected Process DACL, `ProcessExtensionPointDisablePolicy`, `ProcessImageLoadPolicy`, and NTFS read-only ACLs.
- **Zero-Silent Hardware-Only Raw Input**: Uses passive `RegisterRawInputDevices` (`RIDEV_INPUTSINK`) verifying `hDevice != NULL` and physical HID/ACPI/USB device paths — blocking synthetic `SendInput` / `keybd_event` / RDP spoofing while registering **zero** system hotkeys.

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
   为保障系统稳定性与反作弊兼容性，v4 零注入单窗隐身引擎默认**跳过** WOW64 32 位进程、OS 核心桌面壳（`Progman`/`WorkerW`/`Shell_TrayWnd` 等）、`Sandbox:` 沙箱窗口、保护/系统进程以及主流游戏/反作弊进程，且仅对当前 `affinity == 0` 的宿主原生 x64 顶层窗口生效（不覆盖软件自带的 DRM 标志）。

---

## 快速上手 (Quick Start)

### 方式 A：直接下载预编译 Release（推荐）
1. 前往 [**GitHub Releases**](../../releases/latest) 下载 `AegisBlackout-v4.0.0-win64.zip` 并解压到独立目录（例如 `C:\Program Files\AegisBlackout\`，建议目录本身仅限管理员写入）。
2. 验证校验和（可选）：核对 `SHA256SUMS.txt`。
3. 右键管理员运行（或直接双击触发 UAC 提权）**`install_service.bat`**：
   - 自动锁定 `AegisBlackout.exe` / `AegisBlackoutSvc.exe` 为 NTFS 只读权限；
   - 注册并加固 `AegisBlackoutGuard`（LocalSystem 开机自启 + 断线 1 秒拉起 + DACL 防普通进程停服）；
   - 拉起 **High-IL UIPI + 24×7 GPU HW-DRM + 零注入单窗隐身** 护盾并回显 `aegis_init.log`。
4. 查看运行状态运行 `check_status.bat`；如需完整卸载运行 `uninstall_service.bat`。

### 方式 B：从源码本地编译
环境要求：**Windows 10/11 x64 SDK + MSVC Build Tools（VS 2019 / 2022 / 18+）**
```bat
git clone https://github.com/verg-fe/AegisBlackout.git
cd AegisBlackout
build.bat
```
编译产物将自动输出至 `build\`：
- `build\AegisBlackout.exe` —— 核心护盾主程序（24×7 HW-DRM + Dual-Lock + 零注入 Special-APC 单窗隐身 + 物理级防静默截图）
- `build\AegisBlackoutSvc.exe` —— Session 0 `SYSTEM` 级防死守护服务
- `build\CaptureAuditSuite.exe` —— 29 项全向量截屏、红队自卫与 GPU DRM 硬件实证审计工具

---

## 物理硬件快捷键说明

> 全部基于 `RegisterRawInputDevices` 硬件原始输入被动观测，**不调用 `RegisterHotKey`**，不会与任何第三方软件或游戏的组合键产生占位冲突。

| 快捷键 | 功能 | 说明 |
| :--- | :--- | :--- |
| **`Ctrl + Alt + Q`** | **零静默特权截屏** | 仅响应真实物理键盘；原子临时还原单窗 WDA 并隐藏全屏幕布抓取干净桌面后瞬间恢复全部防护，开启受 `WDA_MONITOR` 保护的 4K 暗化选区画布，满足 `>= 150ms` 可视停留并经物理 **双击** 或 **`Enter`** 确认后才写入剪贴板 `CF_DIB`，关闭瞬间 `SecureZeroMemory` 擦除缓冲 |
| **`Ctrl + Alt + Shift + End`** | **紧急干净退出（免 UAC）** | 仅响应真实物理键盘；自动还原所有受保护窗口的 `WDA_NONE` 状态，并返回专属退出码 `0x5A5A` 通知 `AegisBlackoutGuard` 服务优雅停止 |

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
