# TODO

两轮代码审查（`/code-review` 最后一次提交 + 全项目）合并去重后的待修清单，按严重度排序。
勾掉一条就删掉或标记，修完的改动记得同步 `docs/specification.md`。

## 正确性

- [ ] **日志写失败后的状态残留** — `src/battery_log.cpp:181`
      写入失败时只置 `enabled_ = false`，没清 `last_percent_` / `pending_` / `marked_ms_`。
      重新勾选日志后 `start() → observe()` 会写出一条横跨整个禁用期的假电量行并附上时长，
      违反规格 2.8.1「开启日志后的第一格不写时长」。`stop()` 有清理，失败路径漏了。

- [ ] **功率滑动平均不在电流反向时重置** — `src/info_panel.cpp:182`
      窗口只在设备掉线（`device_.close()`）时重置。面板开着插上充电器，会把 -11 W 和 +30 W
      平均成「充电功率 2.7 W」这种从未出现过的读数，持续约 6 秒；正负恰好抵消时
      整个功率区连分隔线一起消失又出现。

- [ ] **`GetSystemPowerStatus` 失败伪装成满电** — `src/battery_power.cpp:56`
      失败返回 `{}`，即 100% + Discharging，和真实读数无法区分。一次瞬时失败会让托盘跳 `FL`、
      清空「最近实测」环，并往日志里永久写下 `62% -> 100%` 和一条「使用电池」状态行。
      需要一个「读数无效」的表示，让上层跳过这一拍而不是当作真值。

- [ ] **字号拟合失败时位图与字体状态不一致** — `src/tray_icon.cpp:112`
      `ensure_font()` 先更新 `icon_width_` / `width_` / `height_`，拟合失败却不更新
      `font_` / `baseline_`。`render()` 只判 `!font_`，于是会用旧 DPI 的字号和越界基线
      画进新尺寸位图，输出错位/裁切，而不是预期的「不出图标」。

- [ ] **出厂日期在设备掉线后不清** — `src/info_panel.cpp:165`
      `date_` 在 `device_.close()`、`hide()` 和采样失败路径上都不清。电池热拔后
      `facts_` / `has_sample_` 都正确归零，唯独「出厂日期」一行留着旧值，且跨多次开关面板持续存在。

- [ ] **窗口类重复注册导致面板永久失效** — `src/info_panel.cpp:78`
      `ensure_window()` 每次都 `RegisterClassExW`。首次 `CreateWindowExW` 偶发失败后，
      后续重试卡在 `ERROR_CLASS_ALREADY_EXISTS`，面板在本进程内再也起不来。
      注册应只做一次（静态标志），或容忍 `ERROR_CLASS_ALREADY_EXISTS`。

## 图标工具与光栅器

已验证：用仓库里的两个 SVG 跑 `svg2ico.pl`，20 / 24 两帧与提交的 `.ico` 逐字节相同，
ICO 目录、BITMAPINFOHEADER、AND mask stride、PNG 的 IHDR/IDAT/CRC32 与 zlib 流都校验通过。
编码器本身没问题，以下是光栅化精度与路径解析的边界。

- [ ] **贝塞尔展平容差不随输出尺寸缩放** — `tools/svg2ico.pl:45`
      容差是 viewBox 单位的固定值（约 0.141 单位），而缩放发生在展平之后，误差随输出尺寸放大。
      实测 `icon_24` 渲染到 256 时单像素 alpha 最大偏差 153/255（渲染到 24 时只有 9.8/255）——
      正是「48/64/256 由 32 设计放大」这条路径，256 帧的圆角会被切成折线。
      容差需要乘 `$size / $vw`。

- [ ] **`Z` 后接绘制命令丢失子路径首顶点** — `tools/svg2ico.pl:77`
      `@cur = ()` 恢复了坐标却没把 `($sx, $sy)` 压回去。用
      `M2 2 L14 2 L14 6 L2 6 Z L2 14 ...` 验证：第二段回来只有 4 点、起点 (2,14)，
      而非 5 点起点 (2,2)。当前两个 SVG 的 `Z` 后面都是 `M` 或结尾，属潜伏 bug，
      但它是静默出错而非 `die`。

- [ ] **首尾重合的三次曲线被静默丢弃** — `tools/svg2ico.pl:45`
      环形曲线必然通过平坦性判定（`0 <= 0.02 * 0`），整段丢掉。实测
      `flatten_cubic(0,0, 10,0, 10,10, 0,0)` 只吐出一个点。

- [ ] **SVG 解析静默忽略不支持的特性** — `tools/svg2ico.pl:134`
      只抓 `<path>` 的 `d`，静默忽略 `transform`、`fill-rule="evenodd"`（光栅器只实现 nonzero）
      和外层 `<g>`。脚本对不支持的路径命令会 `die`，这里却静默接受，行为不一致。
      当前两个 SVG 不涉及，但应改成 `die` 或至少告警。

- [ ] **缺失 `icon_16.svg` / `icon_32.svg`** — `tools/svg2ico.pl:17`
      两个源文件从未提交，六帧里有四帧无法复现，脚本自称的「让下一个人能重新生成」目的落空。

- [ ] **脚本头部注释是从别的项目抄来的** — `tools/svg2ico.pl:3`
      输出路径写 `ui/StatMeter.ico`、颜色写 `0078D4`（实测提交的 ico 全部是 `#107C10`），
      给出的复现命令还多一个不存在的 64 帧、引用不存在的 `icon_16.svg` / `icon_32.svg`——
      照抄执行必然失败，真正生成当前 `.ico` 的命令没有任何地方记录。
      26-32 行整段配色论证在为一个错误的颜色辩护，且与规格 491 行的 `#107C10` 理由直接冲突。

## 规格与代码不一致（按 AGENTS.md：需要先定夺改哪边）

- [ ] **规格 4.7 节仍描述已删除的图标管线** — `docs/specification.md:467`、`:482`、`:484-486`
      以及 `src/BatteryTray.rc:22`
      都还指向 56eaf67 已删除的 `tools/make_icon.ps1` 和 Segoe Fluent Icons 的 `BatteryCharging7`，
      `.rc` 注释还留着「CI 镜像没有那个符号字体」这个已不存在的理由。
      实际是 Perl 的 `svg2ico.pl` + fluentui-system-icons 的 Battery Saver SVG。

- [ ] **图标尺寸集与规格冲突** — `docs/specification.md:488-490` vs `src/BatteryTray.ico`
      规格明确定为 16/24/32/48/256 并专门论证过「20 / 64 / 128 略去」，
      实际 `.ico` 有 6 帧、含 20×20（`icon_20.svg` 就是喂它的，文件 20232 → 23132 字节）。
      要么帧错要么规格错，需要定夺。

## 次要

- [ ] `src/battery_log.cpp:78` — `open_error` 取了从不使用（/W4 下 C4189），注释描述的用途在代码里不存在。
- [ ] `src/battery_log.cpp:84` — `std::string payload = bytes;` 纯多余整块拷贝，直接用 `bytes` 即可。
- [ ] `src/battery_log.cpp:21` — 每写一行跑三次 `GetModuleFileNameW` 加约六次堆分配重算日志路径；
      exe 路径进程内不变，可缓存。这条和上一条都撞 AGENTS.md 的性能硬约束。
- [ ] `src/info_panel.h:14` — 类注释写「double click」，实际是单击（规格 2.10.4 明确要求单击）。

## 已确认没问题（留档，别重复查）

- 无句柄 / GDI 泄漏：RAII 包装完整，`render()` 中位图与字体的选入/还原顺序正确，
  `SetWindowRgn` 与 `CreateIconIndirect` 的所有权移交也对。
- `build.bat` 的 UCRT 混合链接、`/OPT:REF,ICF`、无 PDB，以及 CI 的版本推导与打包层级
  都符合规格 1.2 / 7.x。
