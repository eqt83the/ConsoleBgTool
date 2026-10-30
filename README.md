# ConsoleBgTool

修改 **cmd.exe** 与 **Windows PowerShell** 控制台背景的 Windows 工具。参考 [Maplespe/explorerTool](https://github.com/Maplespe/explorerTool) 修改资源管理器背景的思路，但控制台的实现方式与资源管理器**完全不同** —— 本节第 2 部分记录了在真实 Windows 11 上实测得到的关键结论。

用 **C / C++ / C#** 三种语言实现，987 项自动化检查全部通过。

---

## 1. 快速开始

### 已构建版本

```
ConsoleBgTool/dist/ConsoleBgTool-win-x64/     ← 自包含，免安装 .NET 运行时
  ConsoleBgTool.exe
  cbt_core.dll  cbt_platform.dll              ← 原生层（静态链接 GNU 运行时）
```

### 用法

```bat
:: 列出内置配色
ConsoleBgTool.exe presets

:: 给"当前这个"控制台窗口改背景（在 cmd 或 PowerShell 窗口里直接运行）
ConsoleBgTool.exe apply --preset nord --geometry 120x30 --opacity 88

:: 只看当前控制台状态
ConsoleBgTool.exe show --palette

:: 把当前控制台恢复成 Windows 11 默认调色板与默认文字属性（不改变窗口尺寸）
ConsoleBgTool.exe restore

:: 让 Windows Terminal 里的 cmd / PowerShell 持久生效（含背景图）
ConsoleBgTool.exe terminal --image C:\pics\bg.png --image-opacity 0.25 --opacity 90

:: 环境自检：这台机器上什么能用、什么不能用
ConsoleBgTool.exe doctor
```

`--help`（以及所有子命令的 `--help`）会同时输出**英文和中文**两版说明。

**典型用法是在 cmd 窗口里建一个批处理并运行**，这样窗口的存活期长于工具进程，样式就会留在那个窗口上：

```bat
@echo off
"C:\path\ConsoleBgTool.exe" apply --preset solarized-dark
```

> 注意：每次在 PowerShell 里单独执行一条命令，都会得到一个**新的**控制台；新控制台被销毁时样式也随之消失。所以请把工具**放在你希望变色的那个控制台里**运行。

### 从源码构建

```powershell
# 需要 MinGW-w64（gcc/g++）与 .NET SDK 10
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean -Test -Publish
```

MinGW 默认在 `C:\mingw64`；若在别处，用 `-MingwRoot 'D:\mingw64'` 指定，或设置环境变量 `CBT_MINGW_ROOT`。

`-Test` 会依次跑 **C 核心 498 项**、**C++ 平台层 128 项**、**托管层 361 项**检查（合计 987 项）。
（本机执行策略为 `Restricted`，故用一次性的 `-ExecutionPolicy Bypass`，不必改机器策略。）

---

## 2. 关键结论：为什么不能照搬资源管理器的做法

这一节是本次工作的核心产出。以下每一条都在 Windows 11 build 26100 上实测过，而不是从文档推断。

| # | 结论 | 实测方式 |
|---|---|---|
| 1 | **`HKCU\Console` 的 `ColorTable00..15` 对新建控制台完全无效** | 让子进程用 `GetConsoleScreenBufferInfoEx` 自报调色板；试遍 `Defaults`、`%SystemRoot%_…`、展开式、斜杠式、裸文件名 5 种键名形态，`ct0` 始终是默认的 `0x000C0C0C` |
| 2 | `ScreenBufferSize` / `WindowSize` 注册表值同样无效 | 同上；写入 180x42 后新控制台仍是 120x30 |
| 3 | **文档化的活体 API 有效** | `SetConsoleScreenBufferInfoEx` 写入后，屏幕像素主色 `#562401` → `#FF00FF`（占 94%） |
| 4 | 正确句柄是 `CONOUT$` + **`GENERIC_READ`** | 四种组合逐一测试：`STD_OUT`（管道）err=6、`CONOUT$`+`GENERIC_WRITE` err=5、`CONOUT$`+RW 与 +R 均成功 |
| 5 | 注册表 `WindowAlpha` **不产生透明** | A/B 像素采样 255 vs 120，平均亮度差仅 1.5 |
| 6 | 透明来自 `WS_EX_LAYERED` + `SetLayeredWindowAttributes` | 同一 conhost 窗口 A/B 采样，16.3% 像素改变 |
| 7 | **跨进程美化不可行** | `AttachConsole` 指向真实 conhost 返回 err 5/6 —— 因此本工具**不做注入、不做 Hook** |
| 8 | 命令执行完全不受影响 | 嵌套 `cmd /c` 输出完整，`exit /b 7` 透传为 7 |
| 9 | 默认宿主是 **conhost**，不是 Windows Terminal | `HKCU\Console\%%Startup` 为空；cmd 的父进程是 conhost.exe |
| 10 | **`SetConsoleScreenBufferInfoEx` 对 `srWindow.Bottom` 按独占语义生效** | 把刚读到的窗口矩形原样传回，窗口末行会变成 `Bottom-1`：每次调用少一行。cmd 的 AutoRun 每次启动都 apply，于是**每用一次 `cmd` 就缩一行**，文字偏移量 = `cmd` 使用次数；ConPTY 下每次 apply 还会向终端发 `ESC[8;rows;cols t`，终端重排文字后表现为"输出压在输入上" |
| 11 | 修复：**补偿一行 + 回读校验** | 传入 `Bottom+1`（窗口贴缓冲区底边时改用 `Top-1`），调用后读回几何；不一致再用 `SetConsoleWindowInfo` 精确恢复。实测重复 5 次 apply 窗口恒为 120x50，ConPTY 无 resize 序列 |

**2026-10-23 修复轮**：除上述 off-by-one 之外，还修掉了"apply 在进程退出时被自动回滚"的问题（`ConsoleStateGuard` 析构会把调色板恢复原状，导致 AutoRun 的样式从未真正生效）。现在成功 apply 后样式保留到窗口结束；`restore` 改为恢复 Windows 11 默认调色板与默认属性（因为 CLI 一次命令一个进程，旧的跨进程 restore 实际是空操作）。帮助输出改为中英双版。

### 由此确定的两栖架构

```
活体路径（改当前控制台）      文档化的控制台 API，唯一在 Windows 11 上可靠的 conhost 手段
持久化路径（跨重启生效）      Windows Terminal 的 settings.json，唯一官方支持背景图的途径
```

**这正是本工具不写 `HKCU\Console` 的原因**：写了也不会改变任何控制台的外观，只会让控制台自带的"属性"对话框与现实不一致。工具只读该键（`registry` 命令仅用于备份参考）。

### 一个 MinGW 陷阱（已写进代码注释）

`RegQueryValueExW` **不能**用 `lpData = NULL` 探测值大小：对 `REG_DWORD` 会直接返回 `ERROR_FILE_NOT_FOUND`，让一个正常存在的值看起来"不存在"。正确做法是先分配缓冲区，再按 `ERROR_MORE_DATA` 增长重试。

---

## 3. 官方资料

| 主题 | 文档 |
|---|---|
| 控制台屏幕缓冲区（含"修改前须保存并恢复状态"的要求） | https://learn.microsoft.com/windows/console/console-screen-buffers |
| `CONSOLE_SCREEN_BUFFER_INFOEX`（`ColorTable[16]` 为 `COLORREF`） | https://learn.microsoft.com/windows/console/console-screen-buffer-infoex |
| `SetConsoleScreenBufferInfoEx` | https://learn.microsoft.com/windows/console/setconsolescreenbufferinfoex |
| `COLORREF`（`0x00BBGGRR`，注意与 `#RRGGBB` 相反） | https://learn.microsoft.com/windows/win32/gdi/colorref |
| 分层窗口 `SetLayeredWindowAttributes` | https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setlayeredwindowattributes |
| `WS_EX_LAYERED` 扩展窗口样式 | https://learn.microsoft.com/windows/win32/winmsg/extended-window-styles |
| Windows Terminal 外观设置（`backgroundImage` / `opacity` / `useAcrylic`） | https://learn.microsoft.com/windows/terminal/customize-settings/profile-appearance |
| Windows Terminal 配色方案 | https://learn.microsoft.com/windows/terminal/customize-settings/color-schemes |
| WCAG 相对亮度与对比度（可读性告警依据） | https://www.w3.org/TR/WCAG21/#dfn-relative-luminance |

---

## 4. 架构

```
ConsoleBgTool/
  native/include/  cbt_console.h     纯模型：颜色、属性位、注册表编码（无 Win32 依赖）
                   cbt_registry.h    可替换的注册表后端接口
                   cbt_platform.hpp  活体控制台 / 窗口透明 / 宿主识别
  native/src/      cbt_console.c          C11
                   cbt_registry.c         值所有权与快照容器
                   cbt_registry_memory.c  内存后端（供测试注入失败，验证事务回滚）
                   cbt_registry_win32.c   真实后端，严格限制在 HKCU\Console
                   cbt_platform.cpp       C++17，RAII + 作用域守卫
  native/tests/    test_core.c  test_platform.cpp
  managed/         ConsoleBgTool.Interop   P/Invoke 与结构体契约
                   ConsoleBgTool.Cli       CLI、配色预设、Windows Terminal 写入
                   ConsoleBgTool.Tests     托管测试（含跨语言结构体布局断言）
  scripts/         build.ps1  build-native.ps1
```

### 分层职责

- **C 核心**：不依赖 Win32，因此可完全离线单测。调色板、属性位、`COLORREF` 转换、注册表 DWORD 编码全部在此
- **C++ 平台层**：所有会泄漏的句柄、所有会失败的 Win32 调用。用 RAII 与作用域守卫写成，而不是靠成对的 close
- **C# 层**：命令行、配置解析、Windows Terminal 的 JSON 读写。**不含**任何外观逻辑

### 跨语言结构体契约

`cbt_config` 与 `LiveConsoleState` 通过 C ABI 跨越托管边界，字段顺序/打包/字符宽度一旦不一致不会编译失败，而是**静默错位**。因此 C++ 侧有 `test_structure_layout()`、C# 侧有逐字段 `Marshal.OffsetOf` 断言，两侧对同一组偏移量做检查：

```
cbt_config        palette[16] @0 | fg @64 | bg @68 | canvas @72 |
                  buffer/window @76,78,80,82 | font_size @84 | font_weight @86 |
                  font_family @88 | face_name @92 (32B) | opacity @124 | sizeof 128
LiveConsoleState  4×bool @0..3 | attributes @4 | popup @6 | buffer/window @8,10,12,14 |
                  font w/h @16,18 | pixel w/h @20,24 | palette @28 | sizeof 92
```

---

## 5. 安全性

工具带 `danger-full-access` 权限运行过全部测试，且刻意把攻击面压到最小。

| 项 | 做法 |
|---|---|
| **不注入、不 Hook** | 不写 `AppInit_DLLs`，不 `AttachConsole` 到他人的控制台，不修改任何系统进程。所有控制台操作只针对**本进程自己的**控制台 |
| **注册表路径约束** | 逐段校验：拒绝 `..`、`.`、空段、盘符、`:`、`%`、控制字符、hive 前缀（`HKLM`/`HKCU`/`HKEY_` …）。根恒为 `HKEY_CURRENT_USER`。拼装后的绝对路径必须确实以 `Console` 开头并后接 `\` 或结束 |
| **拒绝畸形配置** | 每个值在写入前都经范围校验；窗口大于缓冲区等组合直接拒绝而非静默截断 |
| **共享控制台先保存、失败即回滚** | 改屏幕缓冲前先 `capture()`；任何一步失败立即 `restore()`。成功后按产品语义保留样式（窗口关闭即消失），显式 `restore` 可恢复默认外观；本轮起 `apply` 不再改变几何，避免影响同一窗口里的其他程序 |
| **透明可逆** | `WS_EX_LAYERED` 基线恒为"未分层"窗口，因此设过透明后仍能恢复到 100% 不透明 |
| **DLL 加载** | `DllImport` 解析器**只**从程序集同目录加载，不使用工作目录、环境变量或 `PATH`，避免 DLL 劫持 |
| **诊断输出** | 所有回显的用户输入与控制字符都经转义，防止注入转义序列 |
| **settings.json** | 先解析后写入；保留注释与未知字段；写前时间戳备份；写时临时文件同卷原子替换 |
| **无命令执行** | 配置是纯 `key=value`，无脚本、无 include、无表达式求值；任何路径都不 shell out |

---

## 6. 测试

```
C 核心      (GCC C11)    498 项检查 / 0 失败
C++ 平台层  (GCC C++17)  128 项检查 / 0 失败 / 0 跳过
托管层      (.NET 10)    361 项检查 / 0 失败
```
（合计 987 项。若测试程序本身位于被沙箱标记为低完整性的工作区内，注册表写/窗口样式类检查会因系统权限被拒绝而失败；把测试 exe 复制到普通目录运行即可，见 §7 第 7 条。）

原生层编译开启 `-Werror -Wpedantic -Wconversion -Wsign-conversion`（C++ 另加 `-Wold-style-cast`、`-Wnon-virtual-dtor`），托管层开启 .NET 分析器 `latest-recommended` 并把警告视为错误。

### 测试实际抓到的缺陷

测试不是形式主义 —— 它抓到了 7 个真实缺陷，其中两个是**严重缺陷**：

| # | 缺陷 | 后果 | 修复 |
|---|---|---|---|
| 1 | **路径重复前缀**：根已是 `Console`，传入 `"Console"` 却拼成 `HKCU\Console\Console` | **读写完全错误的注册表键**。快照会记下空键、恢复会删掉真实配置 —— 静默数据损坏 | 改为相对 `HKEY_CURRENT_USER` 的路径语义，并断言拼装结果必须以 `Console` 开头 |
| 2 | 路径约束放过 `HKLM\Software`、`Console\%%Startup` 等 | 安全边界语义错误 | 新增 hive 前缀拒绝；`%` 列入禁用字符 |
| 3 | **WCAG 指数写反**：`pow(x, 1/2.4)` 应为 `pow(x, 2.4)` | 近黑背景 `#0C0C0C` 亮度算成 **0.378**（正确 0.0037），可读性告警对最常用的深色配色完全失效 | 改用 `k_gamma = 2.4`，并加独立计算的锚定值断言 |
| 4 | 窗口已分层时捕获的基线保留了 `WS_EX_LAYERED` | 设过透明后再设 100% 不透明无效，窗口永久半透明 | 捕获时掩掉该位 |
| 5 | 透明度应用前未捕获基线（`has_window()` 误用为"已打开"） | `apply --opacity` 必然失败 | 增加 `opened()`，以"是否已捕获"为条件 |
| 6 | **`SetConsoleScreenBufferInfoEx` 少一行**：把读到的窗口矩形原样传回，每次调用窗口缩短一行 | cmd AutoRun 每次启动都 apply → **每用一次 `cmd` 缩一行**，输出与输入文字重叠；ConPTY 下还触发终端重排 | 补偿一行 + 回读校验 + `SetConsoleWindowInfo` 兜底；新增"连续 5 次 apply 窗口尺寸不变"回归测试 |
| 7 | `ConsoleStateGuard` 析构时回滚调色板 | `apply` 进程一退出样式就消失，cmd AutoRun 等于没生效 | 成功 apply 后 `disarm()`；显式 `restore` 仍可还原（CLI 的 `restore` 改为恢复系统默认调色板） |

同时修正了**测试自身**的三处错误，它们本会让测试虚假通过：
- `NativeConfig` 是值类型，`Action<NativeConfig>` 传的是**副本**，lambda 里的修改被丢弃 → 全部改用 `ref` 委托
- 结构体赋值复制的是调色板**数组引用** → 加 `DeepCopy()` 辅助
- `paper` 预设前景/背景槽位写反（控制台用**默认属性的背景位**选背景色，不是槽 0）→ 修正后在测试中加断言锁住

---

## 7. 限制

1. **活体样式只作用于运行它的那个控制台窗口。** 工具进程退出后，窗口仍保留样式（窗口属于 conhost，其寿命长于工具进程）；但若窗口本身关闭，样式随之消失。跨重启需用 `terminal` 命令或批处理自启。
2. **`terminal` 命令只影响 Windows Terminal 承载的配置。** 经典 conhost 窗口不支持背景图与透明度，这是操作系统的设计而非本工具的限制。
3. **窗口透明度不是官方支持的控制台设置。** conhost 没有对应的文档化接口；本工具将其作为**显式可选、完全可逆**的窗口样式修改提供，并在 `doctor` 中说明。需要官方途径的透明度请用 Windows Terminal 的 `opacity`。
4. **几何尺寸受控制台与屏幕约束。** 无法精确达到请求的缓冲区/窗口尺寸时，`apply` 会报错并回滚，而不是静默钳制。
5. **仅限 Windows 10/11。** 依赖 `CONSOLE_SCREEN_BUFFER_INFOEX`（Vista+）与 `GetCurrentConsoleFontEx`。
6. 若 PowerShell 报"禁止运行脚本"，用 `-ExecutionPolicy Bypass` 跑构建脚本即可，无需修改机器策略。
7. **完整性级别会影响部分功能。** Windows 会按可执行文件的强制完整性标签降低进程完整性：若工具所在目录被沙箱标为 Low，工具会以 Low IL 运行，此时 `terminal` 写 `settings.json`、`--opacity` 设置窗口样式会被系统（MIL/UIPI）拒绝；从普通目录运行，或把 exe/dll 的标签显式设为 Medium（`icacls <file> /setintegritylevel Medium`）即可恢复。另外在**管理员**控制台里，Medium 工具无法修改高完整性 conhost 窗口，`--opacity` 仍会被 UIPI 拒绝——这是操作系统的隔离规则，不是本工具的缺陷。

---

## 8. 许可

见 [LICENSE](LICENSE)。
