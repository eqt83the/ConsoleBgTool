# ConsoleBgTool — 交付记录

> **状态：全部完成。** 987 项自动化检查全部通过，已产出可直接运行的自包含版本。
> 2026-10-23 修复轮：解决"cmd 里用几次 cmd 后文字偏移/输出压住输入"的窗口逐次缩短缺陷、apply 退出即回退缺陷，并把帮助输出改为中英双版。

---

## 1. 已构建版本

```
ConsoleBgTool\dist\ConsoleBgTool-win-x64\     77.8 MB，自包含
  ConsoleBgTool.exe           CLI
  cbt_core.dll                C 核心（静态链接 GNU 运行时）
  cbt_platform.dll            C++ 平台层
  README.md  HANDOFF.md  LICENSE
```

**已实测可在无 .NET 运行时、无 MinGW 的 PATH 下运行**：把 `PATH` 缩到仅 `System32` 后，
`ConsoleBgTool.exe doctor` 仍报告两个原生库 `loaded`，宿主识别为 `classic console window (conhost.exe)`。

### 用法

```bat
ConsoleBgTool.exe presets
ConsoleBgTool.exe apply --preset nord --geometry 120x30 --opacity 88
ConsoleBgTool.exe terminal --image C:\pics\bg.png --image-opacity 0.25 --opacity 90
ConsoleBgTool.exe doctor
```

要在 cmd 窗口里长期生效，把工具写进批处理并运行（窗口存活期长于工具进程，样式即留在窗口上）。

### 复现构建

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ConsoleBgTool\scripts\build.ps1 -Clean -Test -Publish
```

---

## 2. 验证结果

| 层 | 语言 | 检查数 | 结果 |
|---|---|---|---|
| C 核心 | C11 | 498 | ✅ 0 失败 |
| C++ 平台层 | C++17 | 128 | ✅ 0 失败 / 0 跳过 |
| 托管层 | C# / .NET 10 | 361 | ✅ 0 失败 |

编译严格度：原生层 `-Werror -Wpedantic -Wconversion -Wsign-conversion`（C++ 另加
`-Wold-style-cast`、`-Wnon-virtual-dtor`、`-Woverloaded-virtual`）；托管层 .NET 分析器
`latest-recommended` 且警告视为错误。**零告警。**

### 端到端实测（真实 conhost 窗口内）

在一个 cmd 控制台里链式执行两次 `apply`，用 `--report-state` 把状态写到文件外部读取：

```
第一次（控制台原始状态）   canvasColor=#2E3440  palette00=#2E3440  palette07=#D8DEE9  buffer=120x30
第二次（已被第一次改色）   同上报，且 palette08=#4C566A
嵌套 cmd /c exit /b 7      退出码透传为 7
```

即：改色生效、几何生效、命令执行与退出码完全不受影响、且可重复应用。

### 2026-10-23 修复轮（用户报告：cmd 里用几次 cmd 后文字偏移）

- **根因 1**：`SetConsoleScreenBufferInfoEx` 在本机 conhost/ConPTY 上把 `srWindow.Bottom` 当**独占**处理（实际生效末行是 `Bottom-1`）。工具把刚读到的矩形原样传回，于是**每次 apply 窗口少一行**；cmd 的 AutoRun 每次 cmd 启动都 apply，所以每用一次 `cmd` 就缩一行，偏移量 = 使用次数。ConPTY 下每次 apply 还会向终端发 `ESC[8;rows;cols t`，终端重排后表现为"命令输出压住用户输入"。
- **根因 2**：`ConsoleStateGuard` 析构时执行 `restore()`，把调色板恢复原状 —— `apply` 进程一退出样式就没了，AutoRun 实际从未生效。
- **修复**：新增 `set_screen_buffer_info_keeping_geometry()`（`Bottom+1` 补偿、贴底边时 `Top-1`；调用后回读校验，不符再用 `SetConsoleWindowInfo` 精确恢复），`apply` 与 `restore` 都走它；成功 apply 后 `disarm()`；CLI 的 `restore` 改为恢复 Windows 11 默认调色板/默认属性（不动几何）。帮助输出改为中英双版。
- **验证**：三层测试 498/128/361 全绿；conhost 中连续 5 次 `cmd /c` 与连续 5 次 apply 窗口恒为 120x50（修复前 50→45）；apply 后新进程读回 `#231617`/attr 0x0007；ConPTY 120x30 修复前 5 次 resize 序列、windowRows=26，修复后 **0 次、windowRows=30**；`exit /b 11`、`exit /b 5` 均正确透传；`--help`/`apply --help` 退出码 0 且同时含中英文。

---

## 3. 测试实际抓到的缺陷（7 个，2 个严重）

| # | 缺陷 | 后果 | 修复 |
|---|---|---|---|
| 1 | **路径重复前缀**：根已是 `Console`，传入 `"Console"` 却拼成 `HKCU\Console\Console` | **读写完全错误的注册表键**。快照记下空键、恢复会删掉真实配置 —— 静默数据损坏 | 改为相对 `HKEY_CURRENT_USER` 语义，并断言拼装结果必须以 `Console` 开头且后接 `\` 或结束 |
| 2 | 路径约束放过 `HKLM\Software`、`HKEY_LOCAL_MACHINE`、`Console\%%Startup` | 安全边界语义错误 | 新增 `cbt_path_names_a_hive()`；`%` 列入禁用字符 |
| 3 | **WCAG 指数写反**：`pow(x, 1/2.4)` 应为 `pow(x, 2.4)` | 近黑 `#0C0C0C` 亮度算成 **0.378**（正确 0.0037），可读性告警对最常用深色配色完全失效 | 改用 `k_gamma = 2.4` + 独立锚定断言 |
| 4 | 窗口已分层时捕获的基线保留 `WS_EX_LAYERED` | 设过透明后无法恢复 100% 不透明 | 捕获时掩掉该位 |
| 5 | 透明应用前未捕获基线（把 `has_window()` 误当"已打开"） | `apply --opacity` 必然失败 | 增加 `opened()`，以"是否已捕获"为条件 |
| 6 | **`SetConsoleScreenBufferInfoEx` 窗口矩形少一行**：原样回传读到的 `srWindow`，每次调用窗口缩一行 | cmd AutoRun 每启动一次就缩一行 → 偏移量等于 `cmd` 使用次数；ConPTY 下触发终端重排，输出压住输入 | 补偿 + 回读校验 + `SetConsoleWindowInfo` 兜底；新增连续 5 次 apply 尺寸不变回归测试 |
| 7 | `ConsoleStateGuard` 析构回滚调色板 | `apply` 退出即失效，AutoRun 从未真正生效 | 成功 apply 后 `disarm()`；显式 restore 仍可用 |

同时修正**测试自身**的三处错误（它们本会让测试虚假通过）：
`NativeConfig` 是值类型导致 `Action<NativeConfig>` 传副本（改用 `ref` 委托）；结构体赋值复制调色板**数组引用**（加 `DeepCopy`）；`paper` 预设前景/背景槽位写反。

另外修正一个最初写错的常量：内置调色板原为近似 Campbell 值，已替换为从本机注册表反解出的 16 个真值。

---

## 4. 已实测确认的 Windows 11 行为（build 26100）

| # | 结论 | 验证方式 |
|---|---|---|
| 1 | `HKCU\Console` 的 `ColorTable*` 对**新建**控制台无效 | 子进程自报 `GetConsoleScreenBufferInfoEx`；5 种键名形态全部保持默认 |
| 2 | `ScreenBufferSize` / `WindowSize` 注册表值同样无效 | 同上 |
| 3 | **活体 API 有效** | 改色后屏幕主色 `#562401` → `#FF00FF`（94%） |
| 4 | 正确句柄是 `CONOUT$` + **`GENERIC_READ`** | 四种句柄/权限组合逐一测试 |
| 5 | 注册表 `WindowAlpha` **不产生透明** | A/B 像素采样 255 vs 120，差 1.5 luma |
| 6 | 透明来自 `WS_EX_LAYERED` + `SetLayeredWindowAttributes` | 同窗口 A/B，16.3% 像素改变 |
| 7 | 跨进程美化**不可行** | `AttachConsole` 对真实 conhost 返回 err 5/6 |
| 8 | 命令执行不受影响 | 嵌套 `cmd /c` 输出完整、`exit /b 7` 透传 |
| 9 | 默认宿主是 conhost（非 Windows Terminal） | `%%Startup` 为空；cmd 父进程为 conhost.exe |
| 10 | 几何编码 `LOWORD=X, HIWORD=Y` | 真实值 `ScreenBufferSize=0x00070878`、`WindowSize=0x001E0078` |

### MinGW 陷阱（已写进代码注释）
`RegQueryValueExW` **不能**用 `lpData = NULL` 探测大小：对 `REG_DWORD` 会直接返回
`ERROR_FILE_NOT_FOUND`，让正常值看起来"不存在"。须先分配缓冲区再按 `ERROR_MORE_DATA` 增长重试。

---

## 5. 安全边界（实现中已落实）

1. **不注入、不 Hook、不写 `AppInit_DLLs`、不 `AttachConsole` 到他人控制台** —— 只操作本进程自己的控制台
2. **注册表逐段校验**：拒绝 `..`、`.`、空段、盘符、`:`、`%`、控制字符、hive 前缀；根恒为 `HKEY_CURRENT_USER`
3. **改共享控制台前必须 `capture()`，任一步失败立即回滚**；成功时按产品语义保留样式（窗口关闭即结束，显式 `restore` 可还原默认外观），且 `apply` 不再改变窗口几何
4. **透明基线恒为未分层窗口**，完全可逆
5. **DLL 解析器不使用工作目录 / 环境变量 / PATH**，仅从程序集同目录加载
6. **`settings.json` 先解析后写入**，保留注释，写前时间戳备份，写时同卷原子替换
7. **无命令执行**：配置是纯 `key=value`，无脚本 / include / 表达式求值

---

## 6. 交付物清单

```
ConsoleBgTool/
  README.md                     用法、官方引用、实测结论、限制
  HANDOFF.md                    本文件
  native/include/               cbt_console.h  cbt_registry.h  cbt_platform.hpp
  native/src/                   cbt_console.c  cbt_registry.c  cbt_registry_memory.c
                                cbt_registry_win32.c  cbt_platform.cpp
  native/tests/                 test_core.c  test_platform.cpp
  managed/ConsoleBgTool.Interop/    NativeMethods.cs（含结构体契约与防劫持加载器）
  managed/ConsoleBgTool.Cli/        Program.cs  Appearance.cs  WindowsTerminalSettings.cs
                                    RegistryReport.cs
  managed/ConsoleBgTool.Tests/      Program.cs（含跨语言结构体布局断言）
  scripts/                      build.ps1  build-native.ps1
  dist/ConsoleBgTool-win-x64/   已构建的自包含版本
```

`HKCU\Console` 已核对还原：3 个子键、48 个值、`ColorTable00=0x000C0C0C`、`WindowAlpha=255`。
所有临时探针工程与实验目录均已删除。
