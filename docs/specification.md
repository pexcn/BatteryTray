# BatteryTray 技术规格

`BatteryTray` 是一个 Windows 系统托盘小程序：在任务栏通知区用一个动态生成的图标显示当前电池百分比。
本文档描述它的功能规格与实现约束。

程序最初用 C#/.NET Framework 4.8.1 + WinForms 实现，现已重写为**原生 C++（C++23）+ Win32 API**。
下文描述的是**功能需求与用户可见效果**，不是对某一版实现的逐行记录；凡是原生 Win32 有更优、更省资源、
更健壮的做法（用电源事件通知替代轮询、用纯 GDI 替代 GDI+、更高效的图标缓存策略等），实现上都应优先采用。
目标是一个**零第三方依赖、极低 CPU/内存占用、不含调试信息**的原生实现。

## 1. 硬性约束（最高优先级）

1. **性能优先**：低 CPU、低内存占用。空闲时几乎不占 CPU；常驻内存尽量小。
   - 用消息/事件驱动，**不轮询**：通过 `RegisterPowerSettingNotification` + `WM_POWERBROADCAST` 由系统推送电量/充电变化，收到事件才刷新（详见 [2.2 刷新机制](#22-刷新机制事件驱动禁止轮询)）。
   - 仅在电量百分比或充电状态**发生变化**时才重建托盘图标（用缓存判断去重）。
   - 复用 GDI 资源（字体、DC 等），避免热路径上重复创建/销毁。
   - 每次生成图标后必须 `DestroyIcon` 释放句柄，杜绝 GDI/USER 句柄泄漏。
2. **编译与体积**：产物要经过优化并去除调试信息（相当于 GCC 的 `-O3` + `strip`）。
   - **工具链：MSVC（Visual Studio / Build Tools）。** 在「零第三方依赖 + 小体积」目标下，MSVC 配静态 CRT 能做到真正无 DLL 依赖且体积最小。
     - 优化 + 去调试信息：`cl /std:c++latest /O2 /GL /DNDEBUG /MT ...`（`/std:c++latest` 启用 C++23 特性），链接 `/LTCG /OPT:REF,ICF`，**Release 不生成 PDB**（等价于 strip）。
     - `/MT` 静态链接 CRT，使 `.exe` 不依赖任何 VC++ 运行时可再发行包。
     - manifest 与图标资源用 `rc.exe` 编译 `.rc` 嵌入（也可用 `/MANIFEST` / `mt.exe`）。
   - `/OPT:REF,ICF` 已做冗余消除；如仍需进一步压缩需说明取舍。
   - 不链接系统库以外的任何东西；不引入 vcpkg/conan 等包管理器。
3. **零第三方依赖**：只允许使用 Windows 自带的系统库（user32、gdi32、shell32、advapi32、gdiplus 等）与 C++ 标准库。**禁止**引入任何外部开源库、框架或 NuGet/vcpkg 包。
   - 文字绘制优先用纯 GDI（`CreateFont` + `TextOut` + `GetTextExtentPoint32`）以避免依赖 GDI+；若为字形质量选用 GDI+，需说明理由并确保 `gdiplus.dll` 是系统自带、无需分发。
4. **单文件可执行**：产物是一个独立的 `BatteryTray.exe`，双击即用，无需安装、无需额外 DLL 分发。
5. **绿色便携**：程序自身不产生配置文件，不向用户目录/系统目录写入任何东西。唯一会写的文件是「电量日志」，且**只写在 exe 所在目录**（见 [2.8](#28-电量日志)）。唯一的持久化状态是「开机启动」项写在注册表 `Run` 键（读写注册表不破坏便携性，是 Windows 开机启动的标准做法）；日志开关等其它状态不持久化。

## 2. 功能规格

### 2.1 进程形态

无主窗口的托盘常驻程序。创建一个**隐藏的普通顶层窗口**（正常 `CreateWindowEx` 创建，但从不 `ShowWindow`、不带 `WS_VISIBLE`）来接收托盘回调与系统消息，进程通过消息循环常驻。

- **这个窗口对用户完全不可见**：不显示、不在任务栏、不在 Alt+Tab、不抢焦点，仅作为 `Shell_NotifyIcon` 回调与系统消息的接收端（`NOTIFYICONDATA.hWnd` 必须指向一个真实窗口，托盘图标的所有交互——点击、右键菜单、`TaskbarCreated` 恢复——都通过 `WM_` 消息发到该 hwnd）。托盘图标依赖窗口是 Win32 的硬性要求，无法省略；用户只会看到一个纯托盘图标。
- **不用 message-only 窗口（`HWND_MESSAGE`）**：它收不到广播消息，而本程序依赖两个广播——`PBT_APMPOWERSTATUSCHANGE`（充电插拔）与 `TaskbarCreated`（explorer 重启后恢复图标）。隐藏顶层窗口才能同时收到定向消息与广播消息。

### 2.2 刷新机制（事件驱动，禁止轮询）

**不用定时器周期性轮询**。向系统注册电源事件通知，由系统主动推送变化：

- 调用 `RegisterPowerSettingNotification(hwnd, &GUID_BATTERY_PERCENTAGE_REMAINING, DEVICE_NOTIFY_WINDOW_HANDLE)`，在**电量百分比变化**时收到 `WM_POWERBROADCAST` 的 `PBT_POWERSETTINGCHANGE`。
- 同时处理 `WM_POWERBROADCAST` 的 `PBT_APMPOWERSTATUSCHANGE`，覆盖**插拔电源/充电状态变化**。
- 收到上述任一事件时调用 `GetSystemPowerStatus` 读取最新状态并刷新托盘图标 —— 做到**立即刷新**，最坏延迟由系统事件决定，而非固定 5s。
- 启动时先主动刷新一次作为初始状态。
- 退出前 `UnregisterPowerSettingNotification`。
- 空闲态不得有任何轮询循环或周期性定时器占用 CPU。

### 2.3 托盘图标内容

把电量数字**渲染成文字位图**再转成 `HICON` 设为托盘图标（不是用现成图标资源）。

- 字体：默认用系统 UI 字体（`SystemParametersInfo(SPI_GETNONCLIENTMETRICS)` 取 `lfMessageFont`，回退到 `Segoe UI`）。内容只有数字与 `FL`，不用 CJK 字体（如 Microsoft YaHei）——对纯 ASCII 无收益且非核心系统字体。
- **字号不写死**：托盘图标实际边长是 `GetSystemMetrics(SM_CXSMICON)`，随 DPI 缩放（100%→16px、150%→24px、200%→32px）。按当前 DPI 求出图标尺寸后，把字号**适配到该尺寸**（让最宽内容如两位数/`FL` 刚好填满、不被裁切），与「DPI 感知」要求一致。
- 电量 = `round(BatteryLifePercent * 100)`。**当电量 > 99 时显示为 `FL`**（表示满电 Full），否则显示整数百分比。
- 文字在图标位图内**居中**（不写死 2px 偏移那种按固定尺寸调出来的经验值，随字号/DPI 动态居中）。
- **抗锯齿用灰度而非 ClearType**：图标背景透明（alpha 通道），而 ClearType 是亚像素渲染、假设背景不透明，用在透明位图上会在字符边缘产生彩色毛边。用 `ANTIALIASED_QUALITY`（灰度抗锯齿），边缘干净且与任意任务栏背景兼容。
- 背景透明（图标 alpha 通道透明）。

### 2.4 主题自适应配色

读取注册表 `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize` 的 `SystemUsesLightTheme`：
值为 1（浅色主题）用**黑色**文字，否则（深色/缺失）用**白色**文字。

- 与原程序保持一致：**仅在启动时读取一次配色，运行期不跟随主题切换。**

### 2.5 变化去重

电源事件可能短时间内重复触发。仅当 `(percentage, isCharging)` 相对上次**确有变化**时才重建图标与写日志（沿用缓存判断作为一层防抖），避免无谓的图标重建。

### 2.6 充电状态与 Tooltip

- `isCharging = (电源线在线)`，通过 `GetSystemPowerStatus` 的 `ACLineStatus == 1` 判断。
- Tooltip 文案（`NOTIFYICONDATA.szTip`，中文）：
  - 充电中：`正在充电：<percentage>%`
  - 用电池：`使用电池：<percentage>%`

### 2.7 右键上下文菜单

用 `TrackPopupMenu` 弹出，含三项：

1. `电量日志`（可勾选，见 [2.8](#28-电量日志)）
2. `开机启动`（可勾选，见 [2.9](#29-开机启动)）
3. `退出`：隐藏并移除托盘图标（`Shell_NotifyIcon` + `NIM_DELETE`），退出消息循环。

### 2.8 电量日志

勾选式开关：

- 勾选时开始记录，取消勾选时停止；用一个 bool 状态标记即可。日志开关状态**不持久化**，每次启动默认关闭（保持零配置文件的便携特性）。
- **日志文件放在 exe 所在目录**（`GetModuleFileName` 取 exe 全路径 → 取其目录），文件名 `BatteryTray.log`。**不得写入任何其它目录**（便携/绿色版要求）。
- **写入方式：同步 open-append-close，不用后台线程/队列。** 由于刷新已是事件驱动，日志写入是稀有事件（电量变化通常数分钟一次），每次事件里同步「打开→追加一行→关闭」即可，耗时微秒级、不卡 UI。这样无跨线程共享，从根本上消除「写已关闭句柄」的竞态（比线程+队列更简单也更健壮），不独占文件，用户可随时查看，退出时也无需特殊 flush/关闭逻辑。
- **大小上限 512KB，单文件滚动**：每次追加前检查 `BatteryTray.log` 大小，若写入后将超过 512KB，则先把它重命名覆盖为 `BatteryTray.log.old`（`MoveFileEx` + `MOVEFILE_REPLACE_EXISTING`），再新建 `BatteryTray.log` 继续写。最多保留两个文件（当前 + 一个 `.old`），总量约 1MB，且都在 exe 目录内。
- **写失败要容错**：若 exe 目录不可写（如放在 `Program Files`），打开/写入会失败——此时**静默禁用日志、取消菜单勾选**，绝不崩溃，也**不得**改写到别的目录作为兜底。
- 开启时写一行 `[<时间戳>]: 开启电量日志`，关闭时写 `[<时间戳>]: 关闭电量日志`。
- 每次电量/充电状态变化时追加：`[<时间戳>]: <正在充电|使用电池> -> <percentage>%`。
- **时间戳用固定格式** `yyyy-MM-dd HH:mm:ss`（本地时间），不用区域相关的本地化默认格式——固定格式稳定、可排序、跨机器一致。

### 2.9 开机启动

勾选式开关，用勾选状态（`MF_CHECKED`/`MF_UNCHECKED`）表示当前是否已启用。

- 实现方式：写入/删除注册表运行项 `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Run`，值名用程序名（如 `BatteryTray`），值数据为当前可执行文件的完整路径（用 `GetModuleFileName` 获取，路径含空格时加引号）。
- 用 HKCU（当前用户）而非 HKLM，避免需要管理员权限。
- 每次弹出菜单前（`WM_INITMENUPOPUP` 或构建菜单时）读取注册表，据实回填勾选状态，保证与外部改动同步。
- 点击切换：已启用则删除该值，未启用则写入；操作失败要有容错（记录/忽略，不崩溃）。

## 3. 资源管理与健壮性要求

- 所有 GDI 对象（`HFONT`、`HBITMAP`、`HDC`、`HICON`）与内核句柄（文件、注册表 key）都要成对释放；用 RAII 包装（自定义 `unique_handle` 类模板或 `std::unique_ptr` 配合自定义 deleter），禁止裸露的手动释放散落各处。
- 注册表读取要做空值/类型/长度校验，键或值缺失时走安全默认分支，不得崩溃。
- 单例：用命名 mutex 防止重复启动。
- 正确处理 DPI（至少声明 per-monitor DPI aware 或 system DPI aware，通过 manifest 或 `SetProcessDpiAwarenessContext`），避免高分屏下图标模糊或尺寸错误。
- 托盘图标在 explorer.exe 重启后应能恢复：注册并处理 `TaskbarCreated` 消息，重新 `NIM_ADD`。

## 4. 交付物

1. 完整可编译的 C++ 源码（可单文件 `main.cpp`，也可按职责合理拆分为少量文件，但保持简单）。
2. 一份应用程序 manifest（DPI 感知、`requestedExecutionLevel` 为 `asInvoker`）及其嵌入方式说明。
3. 构建说明：MSVC 命令（`/O2 /GL /MT /DNDEBUG` + `/LTCG /OPT:REF,ICF`、Release 无 PDB）；说明如何嵌入 manifest 与图标资源（`.rc`）。
4. 简短 README 段落：功能、构建、使用（放进启动文件夹或用菜单「开机启动」）。
5. 代码注释用英文；只在解释「为什么」（平台怪癖、非显然的取舍）时写注释，不复述代码。

## 5. 风格与工程约束

- C++23，遵循 RAII 与 const-correctness；避免过度设计与不必要的抽象。
- 不用异常做正常控制流；Win32 错误用返回值/`GetLastError` 处理。
- 字符串统一用宽字符（`wchar_t` / `wWinMain` / `UNICODE` 宏），正确处理中文 UI 文案的编码（源码保存为 UTF-8，字面量用 `L"..."`）。
- 不打印任何调试输出到控制台（无控制台子系统 `/SUBSYSTEM:WINDOWS`）。

## 6. 验收标准

- 空闲态 CPU 占用接近 0，常驻内存尽量低。
- 托盘图标随电量/充电状态变化正确刷新，颜色随系统主题正确。
- 三个菜单项（电量日志、开机启动、退出）行为正确，勾选状态与实际状态一致。
- 反复开关日志、切换开机启动、explorer 重启后均不泄漏句柄、不崩溃。
- 用给出的 MSVC 命令能一次编译出去除了调试信息、无 DLL 依赖的单文件 `BatteryTray.exe`。

## 7. 持续集成

GitHub Actions（`.github/workflows/build.yml`）在 `windows-latest` 上直接调用仓库根的 `build.bat`，
把 `build\BatteryTray.exe` 作为构建产物上传。

- **构建脚本是唯一事实来源**：CI 不重复一遍 `cl` / `rc` 的参数，也不用第三方的 MSVC 环境配置 action
  （`build.bat` 自己用 vswhere 找到并调用 `vcvars64.bat`）。这样 CI 与本地构建出的是同一份东西，
  改编译选项只需要改一个地方。
- 找不到产物要让 CI 失败（`if-no-files-found: error`），不能悄悄上传一个空 artifact。
