# 电量托盘

![BatteryTray](docs/BatteryTray.png)

一个干净、便携、开源的 Windows 电池百分比托盘显示程序。

## 特性

- 托盘图标实时显示电量数字，满电时显示 `FL`
- 图标按当前 DPI 现画，100% / 150% / 200% 缩放下都不糊
- 文字颜色跟随系统主题：浅色主题黑字，深色主题白字
- 事件驱动刷新，由系统推送电量与充电状态变化，**不轮询，空闲时几乎不占 CPU**
- 可选的电量日志，把每次电量与充电状态变化追加到程序目录下的文本文件
- 一键开机自启，只写注册表 `HKCU`，**无需管理员权限**
- 资源管理器重启后自动恢复托盘图标；同一时间只运行一个实例
- 完全便携：不写配置文件、不留注册表垃圾，删除 exe 即完整卸载
- 零第三方依赖，**单个可执行文件，无需安装任何运行时**

## 系统要求

- Windows 10 及以上，64 位

## 安装与使用

1. 从 [Releases](../../releases) 下载 `BatteryTray.zip` 并解压，得到一个 `BatteryTray` 文件夹
2. 把这个文件夹放到一个**有写入权限**的位置，例如 `D:\Software\`
3. 启动文件夹里的 `BatteryTray.exe`，托盘出现电量数字后即开始工作

## 技术说明

<details>
<summary>实现细节与取舍，一般使用者不必阅读（点击展开）</summary>

- 原生 C++（C++23）+ Win32 API，无第三方库、无框架；一个隐藏窗口收事件，`Shell_NotifyIcon` 显示图标
- 电量变化由 `RegisterPowerSettingNotification` + `WM_POWERBROADCAST` 推送，没有定时器轮询；
  托盘图标只在数字或充电状态真的变了时才重画
- 托盘图标是运行时把数字**渲染成带 alpha 的位图**再转 `HICON`，不是现成的图标资源，
  边长取 `GetSystemMetrics(SM_CXSMICON)`，字号按该尺寸拟合，因此天然随 DPI 缩放
- 字体按名字固定用 `Segoe UI`，不读 `lfMessageFont`：中文 Windows 的 `lfMessageFont` 是
  Microsoft YaHei UI，它带内嵌点阵字形且覆盖范围正是托盘这种小 ppem，GDI 命中点阵就绕过矢量轮廓，
  `ANTIALIASED_QUALITY` 在这条路径上失效，字形直接变成硬边锯齿
- 数字先在 2 倍边长的位图上栅格化再平均压回，等效于把笔画定位精度提到半像素——
  hinting 只能把横竖笔画对齐像素网格，对不齐 `7` 那条贯穿字高的斜笔，直接画会摊成一串硬台阶
- 平均后的覆盖率再过一条 gamma 2.2 的查表：线性覆盖率直接当 alpha 用会比系统自己画的文字细而灰，
  而人眼判断笔画「实不实」看的是峰值不是总量
- 字号只取超采样倍数的整数倍，并且只用图标边长的 92% 作为拟合预算：顶满方框的数字比系统自带托盘图标
  显眼一大截，非整数倍的字号则会把竖笔摊到两个半亮像素上
- 抗锯齿用灰度而非 ClearType：图标背景透明，而 ClearType 是亚像素渲染、假设背景不透明，
  用在透明位图上会在边缘产生彩色毛边。这也是托盘文字**无法**和任务栏时钟完全一致的根本原因——
  ClearType 每像素带三个子像素的覆盖率，alpha 通道每像素只有一个数，信息装不下
- exe 的文件图标（资源管理器、任务管理器、快捷方式里显示的那个）与托盘图标无关，
  由 `tools/make_icon.ps1` 从 Segoe Fluent Icons 的充电电池字形（`U+E861`）在设计期渲染一次，
  产物 `src/BatteryTray.ico` 直接进仓库：CI 的 Windows Server 镜像没装这个字体，构建期渲染会
  让 CI 与本地产出不同的东西
- **CRT 混合链接**：`/MT` 把 vcruntime 和 C++ 标准库静态链进去，`/NODEFAULTLIB:libucrt.lib ucrt.lib`
  则让 UCRT 走系统的 `ucrtbase.dll`。产物仍是单文件、不需要 VC++ 可再发行包，体积比全静态小得多，
  UCRT 的安全补丁也由 Windows Update 负责；代价是要求 Windows 10 及以上
- `/GL` + `/LTCG` + `/OPT:REF,ICF` 做全程序优化与冗余消除，`/Gw` 让未引用的全局数据也能被删掉；
  不传 `/Zi` / `/DEBUG`，Release 不生成 PDB。再小就得绕开 CRT（`/NODEFAULTLIB` + 自定义入口），
  代价是自己接管全局初始化，本项目没这么做
- manifest（DPI 感知、`asInvoker`）与图标都通过 `src/BatteryTray.rc` 嵌入；`.rc` 存成 UTF-8 并写了
  `#pragma code_page(65001)`，否则 rc.exe 按系统 ANSI 代码页解析，在非中文 Windows 上编出乱码
- 主题配色在启动时读一次 `AppsUseLightTheme`，不监听后续切换

| 文件 | 职责 |
| --- | --- |
| `src/main.cpp` | 隐藏窗口、电源事件、托盘图标与右键菜单 |
| `src/tray_icon.cpp` | 把电量文字渲染成带 alpha 的 `HICON` |
| `src/battery_log.cpp` | 电量日志的写入与滚动 |
| `src/autostart.cpp` | 注册表 `Run` 项的读写 |
| `src/win32_raii.h` | GDI / 内核句柄的 RAII 包装 |
| `tools/make_icon.ps1` | 设计期生成 exe 的文件图标 `src/BatteryTray.ico` |

完整的功能与实现约束见 [docs/specification.md](docs/specification.md)。

</details>

## 许可证

[GPL-3.0-or-later](LICENSE)
