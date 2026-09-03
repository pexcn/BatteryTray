# TODO

两轮代码审查（`/code-review` 最后一次提交 + 全项目）合并去重后的待修清单，按严重度排序。
勾掉一条就删掉或标记，修完的改动记得同步 `docs/specification.md`。

已修完并删去的两节：「正确性」6 条（同步了规格 2.2 / 2.3 / 2.8 / 2.10.1 / 2.10.4）、
「图标工具与光栅器」6 条（只动 `tools/svg2ico.pl` 与随之重新生成的 `src/BatteryTray.ico`；
规格 4.7 那段仍待定夺，见下）。

## 规格与代码不一致（按 AGENTS.md：需要先定夺改哪边）

- [ ] **规格 4.7 节仍描述已删除的图标管线** — `docs/specification.md:467`、`:482`、`:484-486`
      以及 `src/BatteryTray.rc:22`
      都还指向 56eaf67 已删除的 `tools/make_icon.ps1` 和 Segoe Fluent Icons 的 `BatteryCharging7`，
      `.rc` 注释还留着「CI 镜像没有那个符号字体」这个已不存在的理由。
      实际是 Perl 的 `svg2ico.pl` + fluentui-system-icons 的 Battery Saver SVG。

- [ ] **图标尺寸集与规格冲突** — `docs/specification.md:488-490` vs `src/BatteryTray.ico`
      规格明确定为 16/24/32/48/256 并专门论证过「20 / 64 / 128 略去」，
      实际 `.ico` 有 6 帧、含 20×20（`icon_20.svg` 就是喂它的，文件 23090 字节）。
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
- `.ico` 六帧全部可复现：`icon_20.svg` 喂 16 / 20 两帧，`icon_24.svg` 喂 24 / 32 / 48 / 256 四帧，
  完整命令记在 `tools/svg2ico.pl` 头部，跑出来与提交的文件逐字节相同。仓库里从来只有这两个设计，
  「缺失 `icon_16.svg` / `icon_32.svg`」是脚本头部抄来的假线索，不是真缺件。
- `build.bat` 的 UCRT 混合链接、`/OPT:REF,ICF`、无 PDB，以及 CI 的版本推导与打包层级
  都符合规格 1.2 / 7.x。
