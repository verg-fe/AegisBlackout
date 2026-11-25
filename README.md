# AegisBlackout (黑幕神盾)

**Windows 11 24×7 GPU 硬件 DRM (`HW_PROTECTED | DISPLAY_ONLY`) + `WDA_MONITOR` + Ring-0 UIPI (`High-IL`) + `SYSTEM` 自愈守护反截屏与反录屏护盾**

> **Zero Custom `.sys` Driver | VBS / HVCI 100% Compatible | Zero Hotkey Occupation (`RegisterHotKey = 0`) | Zero-Silent Physical Hardware Snipping**

AegisBlackout 是一个面向 Windows 11 的零内核驱动、零入侵式隐私防窥与防录屏保护框架。它通过将 GPU 硬件受保护显示内容（Hardware-Protected DRM Overlay）、系统 DWM 显示亲和力锁（`WDA_MONITOR`）、`win32k.sys` 内核 UIPI 特权隔离（`SYSTEM` 服务托管拉起 `High-IL`）以及纯物理硬件原始输入（Raw Input）四层机制正交叠加，在无需加载任何未签名 `.sys` 内核驱动的前提下，全天候阻断后台静默截屏、GPU 桌面帧采集、系统录屏及进程/窗口篡改攻击，同时保留绝对零静默的人类交互式特权截屏通道。

---

## English Overview

**AegisBlackout** is a driverless Windows 11 anti-screen-capture and anti-recording shield engineered for strict compatibility with VBS/HVCI and anti-cheat environments:

- **24×7 GPU Hardware-Protected DRM Overlay**: Creates a fullscreen `DirectComposition` (`IDCompositionDevice`) swap chain with `DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED | DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY` (`0x420`) backed by hardware-protected VRAM (`D3D11_RESOURCE_MISC_HW_PROTECTED`), automatically falling back to `WS_EX_LAYERED` if unavailable.
- **Dual-Lock Click-Through Curtain**: Combines `WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE` (`WM_NCHITTEST -> HTTRANSPARENT`) with `SetWindowDisplayAffinity(WDA_MONITOR)`, an active `100ms` Z-order watchdog, and `SetWinEventHook`.
- **Ring-0 `win32k.sys` UIPI + `NT AUTHORITY\SYSTEM` Watchdog**: A hardened LocalSystem service (`AegisBlackoutGuard`) acquires the active console user's elevated `TokenLinkedToken` to spawn `AegisBlackout.exe` at **High Integrity Level** in `winsta0\default` with zero UAC prompts at boot, strict Protected Process DACL, `ProcessExtensionPointDisablePolicy`, `ProcessImageLoadPolicy`, and NTFS read-only ACLs.
- **Zero-Silent Hardware-Only Raw Input**: Uses passive `RegisterRawInputDevices` (`RIDEV_INPUTSINK`) verifying `hDevice != NULL` and physical HID/ACPI/USB device paths — blocking synthetic `SendInput` / `keybd_event` / RDP spoofing while registering **zero** system hotkeys (`RegisterHotKey` free).

---

## 为什么选择“零 `.sys` 驱动”架构？

在启用了 **VBS / HVCI（内核完整性保护）** 与 **Kernel DMA Protection** 的现代 Windows 11（如 24H2 / 25H2）环境中，自定义未签名 `.sys` 内核驱动无法加载，而激进的内核 Hook 也极易与商业驱动安全机制及主流游戏反作弊系统产生冲突。

AegisBlackout 依托操作系统原生受保护的组件实现同等级别防线：

| 防御维度 | 底层机制 | 安全效果 |
| :--- | :--- | :--- |
| **第一层：24×7 GPU 硬件 DRM** | `D3D11` + `DirectComposition` + `DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED \| DISPLAY_ONLY` | 显存开启硬件内容加密；DXGI 桌面复制触发 `ProtectedContentMaskedOut`，`DwmGetDxSharedSurface` 拒绝导出句柄 |
| **第二层：DWM 双锁隐身幕布** | 全屏置顶 `WDA_MONITOR` + `WS_EX_NOREDIRECTIONBITMAP` + `WS_EX_TRANSPARENT` + 100ms 看门狗 + `WinEventHook` | 完全穿透点击不影响正常操作；GDI `BitBlt` / `GetPixel` / `PrintWindow` / WGC Monitor 均捕获纯黑；自动压回抢置顶弹窗 |
| **第三层：Ring-0 UIPI + 内核自卫** | `SYSTEM` 服务托管拉起 `High-IL` + `D:P` 进程 DACL + `ExtPt`/`ImgLoad` 缓解 + NTFS 只读锁 | 宿主机普通（Medium-IL）软件 `OpenProcess(TERMINATE/VM_READ/VM_WRITE)` 全部 `ERROR_ACCESS_DENIED (5)`；禁止向高权限窗口注入消息 |
| **第四层：零静默物理门禁截屏** | `RegisterRawInputDevices` 校验物理 HID 句柄 + `>=150ms` 可视绿色 HUD 门禁 + `SecureZeroMemory` 内存擦除 | 彻底杜绝静默/后台代码触发截屏；必须人类物理按 `Ctrl + Alt + Q` 并双击或按 `Enter` 确认，关闭瞬间安全清零 4K 显存/内存快照 |

---

## 29 项全向量攻防实测结果 (Capture & Red-Team Audit)

使用本仓库自带的 [`CaptureAuditSuite.cpp`](src/CaptureAuditSuite.cpp) 在 **Windows 11 25H2 (3840×2160) + AMD Radeon RX 9070** 实机（对运行中的 High-IL + `HW_PROTECTED|DISPLAY_ONLY` 实例）实测数据如下：

### Group A & B：宿主机桌面截屏 / 现代 GPU 连续录屏测试

| 编号 | 攻击 / 采集方式 | 实测非黑像素率 | 结果 |
| :--- | :--- | :--- | :--- |
| `V01–V05` | GDI `BitBlt(SRCCOPY / CAPTUREBLT)` / `StretchBlt` / `CreateDC("DISPLAY" / "\\\\.\\DISPLAY1")` | **0.00%** | 拦截（全黑） |
| `V06` | `GetPixel(GetDC(NULL))` 20×20 全屏采样网格 | **0.00%** | 拦截（全黑） |
| `V07–V10` | 目标窗口 `GetDC` / `GetWindowDC` / `PrintWindow(0)` / `PrintWindow(PW_CLIENTONLY)` | **0.00%** | 拦截（全黑） |
| `V12` | DXGI 1.2 `DuplicateOutput`（30 帧连续桌面流） | **0.00%**（`maskedOut=25`） | 拦截（全黑） |
| `V13` | DXGI 1.5 `DuplicateOutput1` | **0.00%**（`maskedOut=4`） | 拦截（全黑） |
| `V14` | Direct3D 9 `GetFrontBufferData` | **0.00%** | 拦截（全黑） |
| `V15–V16` | Magnification API（常规 & `MW_FILTERMODE_EXCLUDE` 主动排除保护窗） | **0.00%** | 拦截（全黑） |
| `V17` | Windows Graphics Capture `CreateForMonitor`（10 帧连续录屏） | **0.00%**（仅鼠标光标微点） | 拦截（全黑） |
| `V19` | `user32!DwmGetDxSharedSurface` 共享表面读取 | **0.00%** | 拦截（全黑） |
| `V20` | 20 帧连续 GDI `gdigrab` 录屏循环 | **0.00%** | 拦截（全黑） |

### Group C & E：红队对抗与 GPU 硬件受保护资源独立设备验证

| 编号 | 红队对抗 / 硬件验证项目 | 实测结果 |
| :--- | :--- | :--- |
| `V21` | 旧版全局 IPC 后门事件探测 | **PASS**（零全局 IPC 句柄） |
| `V22` | `OpenProcess(TERMINATE / VM_READ / VM_WRITE \| CREATE_THREAD)` 强杀/转储/注入 | **PASS**（全部返回 `ERROR_ACCESS_DENIED (5)`） |
| `V23` | 特征窗口类名 / 标题指纹扫描 | **PASS**（`0/5` 命中，每次启动随机类名 + 空标题） |
| `V24` | 窗体隐藏 / 移出屏幕 / 降 Z 序 / `WM_CLOSE` 篡改攻击 | **PASS**（维持 `3840x2160` + `WDA_MONITOR` + `0.00%`） |
| `V25` | 合成按键 `SendInput` 伪造截屏或紧急退出热键 | **PASS**（直接拒收，漏光 `0.00%`，护盾存活） |
| `V26` | 全局热键占用扫描（`Alt+A` / `Ctrl+Alt+A` / `Ctrl+Alt+S` / `Ctrl+Alt+X`） | **PASS**（`4/4 FREE(0)`，零冲突零指纹） |
| `V27` | 600×600 高亮红色 `WS_EX_TOPMOST` 抢置顶劫持 | **PASS**（瞬间压回底层，GDI/DXGI 漏光均 `0.00%`） |
| `V29` | **独立 D3D11 设备 `CopyResource` 与硬件 DRM 密文实证** | **PASS**：<br>• `HW_PROTECTED\|DISPLAY_ONLY` 交换链：`GetSharedHandle`/`CreateSharedHandle` 均拒绝 (`0x80070057`)，对运行中护盾窗调用 `DwmGetDxSharedSurface` 返回 `ok=0, hSurf=NULL`；同设备强制 `CopyResource -> Staging -> Map` 读回纯高熵硬件密文（`0/16384` 明文匹配，`4096+` 杂乱色值，对照组未保护交换链为 `16384/16384` 纯明文）<br>• 即便开启跨设备 `SHARED_NTHANDLE \| SHARED_KEYEDMUTEX` 后通过**第二个独立 `ID3D11Device`** `OpenSharedResource1` + `CopyResource` 读回，仍保留 `MiscFlags=0x80900` 且全部为高熵密文（`0/16384` 明文匹配） |

### 配合隔离沙箱后的点对点窗口与磁盘审计 (`V11` / `V18` / `V28`)

在主机桌面隔离基础上，按 [`configs/Sandboxie-Hardening-Template.ini`](configs/Sandboxie-Hardening-Template.ini) 将高风险桌面端 / IM / Electron 系应用放入强化沙箱后实测：
- **`V11` `PrintWindow(PW_RENDERFULLCONTENT=2)` & `WM_PRINT`**：对宿主机窗口句柄全部阻断（`0.00%` 像素）。
- **`V18` WGC `CreateForWindow(target_hwnd)`**：`blocked=1, hr=0x80070005`（直接拒绝访问）。
- **`V28` 宿主机敏感目录扫盘 & 系统捕获 DLL 加载**：桌面/图片/下载/文档及护盾目录全部 `BLOCKED`；`GraphicsCapture.dll`、`Magnification.dll`、`CaptureService.dll` 加载失败。

---

## 安全边界与坦诚说明 (Threat Model)

1. **单窗口句柄定向抓取 (`PW_RENDERFULLCONTENT` / `WGC CreateForWindow`)**：
   Windows 全屏置顶遮罩主要拦截桌面合成层、全屏 GDI/DXGI/WGC-Monitor 采集及未知句柄遍历。如果同处于宿主机且同权限的恶意程序明确持有某个受信任窗口的 `HWND`，操作系统原生允许它直接从 DWM 视觉树抓取该窗口表面——因此对存在恶意扫盘或静默抓屏风险的**主流桌面即时通讯软件 / IM 客户端**，务必配合 **Sandboxie-Plus 沙箱模板** 使用，使沙箱内所有进程的 `V11`/`V18` 全部跌落到 `0.00% / E_ACCESSDENIED`。
2. **同桌面窗口几何矩形元数据 (`GetWindowRect`)**：
   在共享交互桌面 (`WinSta0\Default`) 下，即便跨沙箱屏蔽了像素、标题与 `EnumWindows`，基础 Win32 链式遍历 / UIA 仍可读取其他窗口的外框宽高坐标（不含像素与标题）。若需彻底隔离窗口几何尺寸元数据，需使用独立 Win32 Desktop 或虚拟机。
3. **显卡驱动差异与自动降级**：
   若当前硬件/虚拟显卡不支持 `DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED | DISPLAY_ONLY`，AegisBlackout 会自动记录日志并无缝降级至标准 `WS_EX_LAYERED + WDA_MONITOR` 双锁模式继续运行。

---

## 快速上手 (Quick Start)

### 方式 A：直接下载预编译 Release（推荐）
1. 前往 [**GitHub Releases**](../../releases/latest) 下载 `AegisBlackout-v3.0.0-win64.zip` 并解压到独立目录（例如 `C:\Program Files\AegisBlackout\`，建议目录本身仅限管理员写入）。
2. 验证校验和（可选）：核对 `SHA256SUMS.txt`。
3. 右键管理员运行（或直接双击触发 UAC 提权）**`install_service.bat`**：
   - 自动锁定 `AegisBlackout.exe` / `AegisBlackoutSvc.exe` 为 NTFS 只读权限；
   - 注册并加固 `AegisBlackoutGuard`（LocalSystem 开机自启 + 断线 1 秒拉起 + DACL 防普通进程停服）；
   - 拉起 **High-IL UIPI + 24×7 GPU HW-DRM** 护盾并回显 `aegis_init.log`。
4. 查看运行状态运行 `check_status.bat`；如需完整卸载运行 `uninstall_service.bat`。

### 方式 B：从源码本地编译
环境要求：**Windows 10/11 x64 SDK + MSVC Build Tools（VS 2019 / 2022 / 18+）**
```bat
git clone https://github.com/verg-fe/AegisBlackout.git
cd AegisBlackout
build.bat
```
编译产物将自动输出至 `build\`：
- `build\AegisBlackout.exe` —— 核心护盾主程序（24×7 HW-DRM + Dual-Lock + 物理级防静默截图）
- `build\AegisBlackoutSvc.exe` —— Session 0 `SYSTEM` 级防死守护服务（负责将 `AegisBlackout.exe` 以 High-IL 注入交互桌面）
- `build\CaptureAuditSuite.exe` —— 29 项全向量截屏、红队自卫与 GPU DRM 硬件实证审计工具

---

## 物理硬件快捷键说明

> 全部基于 `RegisterRawInputDevices` 硬件原始输入被动观测，**不调用 `RegisterHotKey`**，不会与任何第三方软件或游戏的组合键产生占位冲突。

| 快捷键 | 功能 | 说明 |
| :--- | :--- | :--- |
| **`Ctrl + Alt + Q`** | **零静默特权截屏** | 仅响应真实物理键盘；开启受 `WDA_MONITOR` 保护的 4K 暗化选区画布与顶部绿色 HUD 门禁，满足 `>= 150ms` 可视停留并经物理 **双击** 或 **`Enter`** 确认后才写入剪贴板 `CF_DIB`，关闭瞬间使用 `SecureZeroMemory` 擦除缓冲 |
| **`Ctrl + Alt + Shift + End`** | **紧急干净退出（免 UAC）** | 仅响应真实物理键盘；返回专属退出码 `0x5A5A` 通知 `AegisBlackoutGuard` 服务优雅停止而不触发自愈重启 |

---

## 审计工具 CLI 用法

```bat
:: 在宿主机运行全部 GDI / GPU / 红队 / 硬件DRM 审计 (V01-V29)
build\CaptureAuditSuite.exe --mode host

:: 指定在隔离沙箱内测试特定目标窗口句柄（十六进制）与扫盘封堵 (V01-V20, V28)
build\CaptureAuditSuite.exe --mode sandbox --target-hwnd 0x001204A0 --out C:\Users\Public\Documents\audit_box.txt
```

---

## 配合 Sandboxie-Plus 封堵高频扫盘 IM 客户端

1. 打开 [`configs/Sandboxie-Hardening-Template.ini`](configs/Sandboxie-Hardening-Template.ini)。
2. 将 `<YOUR_USER>`、`<AEGIS_INSTALL_DIR>` 替换为本机真实用户名与 AegisBlackout 部署路径。
3. 如需针对特定常见聊天软件禁止其内置截屏/录屏插件，将模板底部的 `ClosedFilePath` 示例路径对应到该客户端的插件目录（多版本目录下务必使用通配符 `*` 结尾以拦截子文件）。

---

## 免责声明 (Disclaimer)

本项目仅用于个人隐私防护、桌面安全架构研究及授权环境下的防御有效性评估。请勿将本工具用于未授权的系统控制或对抗合规审计策略；使用本软件即表示您理解并同意自行承担配置与使用产生的全部责任。

## License

[MIT License](LICENSE) © 2026 verg-fe
