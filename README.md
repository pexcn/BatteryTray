# 电量托盘

![](BatteryTray.png)

在 Windows 通知区域（托盘）显示当前电池百分比。原生 C++ / Win32 实现，单文件
`BatteryTray.exe`，无第三方依赖、无需安装、不写配置文件。需要 Windows 10 及以上。

## 功能

- 托盘图标实时显示电量数字，满电（> 99%）显示 `FL`；图标按当前 DPI 生成，高分屏不糊。
- 文字颜色跟随系统主题（浅色主题黑字，深色主题白字），启动时读取一次。
- 事件驱动刷新：通过 `RegisterPowerSettingNotification` 与 `WM_POWERBROADCAST`
  由系统推送电量与充电状态变化，不轮询，空闲时几乎不占 CPU。
- 鼠标悬停显示 `正在充电：<n>%` / `使用电池：<n>%`。
- 右键菜单：
  - **电量日志**：把每次电量/充电状态变化追加到 exe 同目录的 `BatteryTray.log`
    （UTF-8，行格式 `[yyyy-MM-dd HH:mm:ss]: 使用电池 -> 87%`）。超过 512 KB 时滚动为
    `BatteryTray.log.old`，最多保留两个文件。开关不持久化，每次启动默认关闭；exe 所在
    目录不可写时静默关闭，不会改写到其它目录。
  - **开机启动**：写入/删除 `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Run`
    下的 `BatteryTray` 值，无需管理员权限。每次弹出菜单时按注册表实际状态回填勾选。
  - **退出**。
- explorer.exe 重启后自动恢复托盘图标；同一时间只运行一个实例。

## 构建

需要 Visual Studio 2022（或 Build Tools）并勾选「使用 C++ 的桌面开发」。

```cmd
build.bat
```

脚本会自己用 vswhere 找到并调用 `vcvars64.bat`，产物在 `build\BatteryTray.exe`。
在 Developer Command Prompt 里直接手写命令也可以：

```cmd
rc /nologo /I src /fo build\BatteryTray.res src\BatteryTray.rc

cl /nologo /std:c++latest /utf-8 /W4 /permissive- /EHsc /GR- /O2 /GL /Gw /DNDEBUG /MT ^
   /Fobuild\ /Febuild\BatteryTray.exe ^
   src\*.cpp build\BatteryTray.res ^
   /link /LTCG /OPT:REF,ICF /SUBSYSTEM:WINDOWS /RELEASE /NODEFAULTLIB:libucrt.lib ^
   ucrt.lib user32.lib gdi32.lib shell32.lib advapi32.lib
```

说明：

- **CRT 混合链接**：`/MT` 把 vcruntime 和 C++ 标准库静态链进去，
  `/NODEFAULTLIB:libucrt.lib ucrt.lib` 则让 UCRT 走系统的 `ucrtbase.dll`。
  这样产物仍是单文件、不需要 VC++ 可再发行包（`vcruntime140.dll` / `msvcp140.dll`
  不随 Windows 分发，而 `ucrtbase.dll` 从 Windows 10 起是系统自带组件），体积比全静态
  小得多，UCRT 的安全补丁也由 Windows Update 负责。**代价是要求 Windows 10 及以上**；
  想支持更老的系统就去掉这两个链接选项，退回 `/MT` 全静态。
- 不传 `/Zi`、不传 `/DEBUG`，因此 Release 不生成 PDB，可执行文件里没有调试信息。
- `/GL` + `/LTCG` + `/OPT:REF,ICF` 做全程序优化与冗余消除，`/Gw` 让未引用的全局数据也能
  被删掉；如果还要更小，可以进一步考虑绕开 CRT（`/NODEFAULTLIB` + 自定义入口），代价是
  要自己接管全局初始化，本项目没这么做。
- `/utf-8` 必须带上：源码里的中文 UI 文案是 UTF-8，缺少它 MSVC 会按 ANSI 代码页解析。
- manifest（DPI 感知、`asInvoker`）与 exe 的文件图标都通过 `src/BatteryTray.rc` 嵌入
  （`1 RT_MANIFEST` 与 `1 ICON`），随 `rc.exe` 一起编进 `.res`。图标 `src/BatteryTray.ico`
  由 `tools/make_icon.ps1` 从 Segoe Fluent Icons 的充电电池字形（`U+E861`，约七成电量）渲染，产物直接进仓库，
  构建不依赖那个字体；想换字形、配色或尺寸，改参数重跑脚本并把 `.ico` 一起提交：

  ```cmd
  powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
  ```
- **版本号**：CI 从 git tag 算出版本，通过 `VERSION` 环境变量传给 `build.bat`，再写进
  `build\version.h` 供 `rc` 嵌入 `VERSIONINFO`：tag 上是 `1.2.3`，之后的提交带上距 tag 的
  提交数（`1.2.3-5`）。本地构建没有这个变量，版本就是 `0.0.0`；想要真版本，
  `set VERSION=1.2.3` 之后再跑 `build.bat`。

## 使用

双击 `BatteryTray.exe` 即可，托盘出现电量数字。想开机自启，用右键菜单里的
「开机启动」，或者把 exe 放进启动文件夹（Win+R 输入 `shell:startup`）。

## 源码

| 文件 | 职责 |
| --- | --- |
| `src/main.cpp` | 隐藏窗口、电源事件、托盘图标与右键菜单 |
| `src/tray_icon.cpp` | 把电量文字渲染成带 alpha 的 `HICON` |
| `src/battery_log.cpp` | 电量日志的写入与滚动 |
| `src/autostart.cpp` | 注册表 `Run` 项的读写 |
| `src/win32_raii.h` | GDI / 内核句柄的 RAII 包装 |
| `tools/make_icon.ps1` | 设计期生成 exe 的文件图标 `src/BatteryTray.ico` |

## 许可证

[GPL-3.0-or-later](LICENSE)
