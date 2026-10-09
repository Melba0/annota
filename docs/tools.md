# 使用手册：构建、命令行、环境变量与插件

这篇文档回答"**我该怎么用**"：怎么构建、每个子命令干什么、有哪些开关和环境变量、怎么把
C++ 做成插件、出问题怎么查。语言本身的规则见 [syntax.md](syntax.md)，标准库见
[stdlib.md](stdlib.md)，静态分析见 [reference.md](reference.md)。

## 1. 构建

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1                 # 完整构建
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify         # 构建 + 跑完所有检查
```

| 参数 | 作用 |
| --- | --- |
| （无） | 自动探测 Qt 6（`D:\Qt`、`C:\Qt`、`%USERPROFILE%\Qt`）并做完整构建 |
| `-NoQt` | 不链接 Qt：保留解释器、REPL、分析器、LSP，没有窗口和 IDE |
| `-QtRoot <路径>` | 指定 Qt 目录，例如 `D:\Qt\6.10.1\mingw_64` |
| `-Clean` | 先清理再构建 |
| `-Config <名字>` | 构建配置名（默认 `release`） |
| `-Verify` | 构建后跑示例、分析用例、插件、基准、LSP、高亮、文档链接、IDE 截图 |

产物是 `build/annota.exe`（以及 Qt 需要的 DLL）。随时可以问二进制自己会什么：

```powershell
build\annota.exe --features
# contracts annotations files gui ide      <- 完整构建
# contracts annotations files (+ 说明)     <- 无 Qt 构建
```

要求：C++17 编译器（MinGW-w64 g++ 13 或 MSVC 2022）。**Qt 6 是可选的**，没有它只是没有窗口。

不想自己编译的话，每个 Release 都带预编译包（见 README 的快速开始）：

| 包 | 内容 |
| --- | --- |
| `annota-vX.Y.Z-windows-x64.zip` | `annota.exe` + Qt 运行时 + `platforms/` + `lib/` + `src/` + `native/` + `plugins/` + `docs/` + `examples/` + `libannota.dll.a` |
| `annota-vX.Y.Z-linux-x86_64.tar.gz` | `annota` + `lib/` + `src/` + `native/` + `plugins/` + `docs/` + `examples/` |

`lib/` 与可执行文件同级，所以 `use seq` 之类开箱可用；`src/` 与（Windows 上的）导入库也在包里，
因此解压后可以直接 `annota plugin build`。CI 会在**解压出来的包里**跑一遍
`annota examples/selfcheck.ant`，不通过就不发布。发布流程见 [CONTRIBUTING.md](../CONTRIBUTING.md)
的 Releasing 一节。

## 2. 运行程序

```powershell
build\annota.exe run  examples\algorithms.ant      # 运行
build\annota.exe check examples\algorithms.ant     # 只做语法/编译检查
build\annota.exe -e "print 1 + 2"                  # 直接执行一段代码
"world" | build\annota.exe examples\input.ant      # input 从 stdin 读
build\annota.exe examples\algorithms.ant           # 等价于 run
```

| 开关 | 作用 |
| --- | --- |
| `-e` / `--eval <code>` | 执行一段代码而不是文件 |
| `--contracts` | 运行期检查 `assert` / `require` / `ensure` / `invariant` |
| `--dump-tokens` / `--dump-ast` | 打印词法单元流 / 语法树 |
| `--dump-bc` | 反汇编字节码（能看到融合后的超指令和 `lend`） |
| `--dump-annotations` | 以 JSON 打印标注索引 |
| `--plugin <file>` | 启动时加载一个原生插件（见 §6），可重复 |
| `--gui` | 在窗口里显示程序的 `view`（Qt 构建） |
| `--gui-tree` | 以文本打印组件树（不需要 Qt） |
| `--gui-shot <png>` | 不开窗，把 `view` 渲染成 PNG |
| `--gui-click X,Y` / `--gui-key <键>` | 在截图前派发一次合成点击 / 按键（按键发往焦点控件，和真键盘一致） |
| `--gui-wait <ms>` | 截图前先让事件循环跑 ms 毫秒（这样 `every`/`tick` 定时器和按键处理会被真正执行） |
| `--features` | 报告本二进制的可选能力 |
| `-h` / `--help` | 用法 |

## 3. 子命令一览

| 命令 | 用途 | 主要选项 |
| --- | --- | --- |
| `annota` | 不带参数进入 REPL | `:help` `:load` `:analyze` `:bc` `:reset` … |
| `annota run <file>` / `check <file>` | 运行 / 只检查 | 见 §2 的开关 |
| `annota analyze <file>` | 静态分析 | `--level N`（1/2/3）、`--modules`（也分析 `use` 进来的模块）、`--json` |
| `annota analyze-suite <dir>` | 把目录里的用例（带 `expect:` 标注）跑一遍 | |
| `annota ide <query> <file> …` | 编辑器后端的一次性查询 | `hover` `definition` `rename` `complete` `inline` `coverage` `quickfix` `suppressions` `report` `checks` `annotations` `docs` |
| `annota bench` | 分析器性能基准（L1/L2/L3 预算） | `--funcs=N`、`--repeat=N` |
| `annota lsp` | 语言服务器（stdio，JSON-RPC） | |
| `annota studio [file]` | 图形化 IDE | 见 §4 |
| `annota plugin build <file.cpp>` | 把 C++ 文件编成插件 | `-o <输出>`、`-n <模块名>` |

`annota ide docs --out=docs/reference.md` 会重新生成参考手册（**不要手改 `reference.md`**，
它由标注/检查/原语注册表生成）。

## 4. 图形化 IDE（`annota studio`）

```powershell
build\annota.exe studio examples\algorithms.ant
```

| 键 | 作用 |
| --- | --- |
| `F5` | 运行当前文件，输出进下面的面板（不会开窗） |
| `F7` | 预览程序的 `view`（程序定义了界面时） |
| `Ctrl+F5` | 运行并检查契约 |

无头（CI、远程）用法：

```powershell
$env:QT_QPA_PLATFORM = "offscreen"
build\annota.exe studio examples\perf.ant --run --echo             # 运行并打印输出面板内容
build\annota.exe studio examples\files.ant --shot out.png          # 截图后退出
build\annota.exe studio examples\perf.ant --check-highlight        # 只检查语法高亮
```

| 开关 | 作用 |
| --- | --- |
| `--run` | 加载后立即运行 |
| `--preview` | 加载后立即预览 `view` |
| `--echo` | 把输出面板内容打印到 stdout；没有 `--shot` 时随后自动退出 |
| `--shot <png>` | 布局完成后截图并退出 |
| `--tab=<名字>` | 启动时切到某个页签 |
| `--check-highlight` | 检查文件是否被整段误判为 `-[ ]-` 注释（见 §9） |

## 5. 环境变量

| 变量 | 作用 | 例子 |
| --- | --- | --- |
| `ANNOTA_PLUGIN` | 启动时加载的插件，多个用 `;`（Windows）或 `:` 分隔 | `$env:ANNOTA_PLUGIN="hello.dll"` |
| `ANNOTA_JIT_THRESHOLD` | 自动编译热循环所需的回跳次数（默认 4000，`0` 关闭自动提升） | `$env:ANNOTA_JIT_THRESHOLD="1000"` |
| `ANNOTA_NO_JIT` | 设为任意值即完全关闭机器码后端（保留超指令融合） | `$env:ANNOTA_NO_JIT="1"` |
| `ANNOTA_JIT_DEBUG` | 打印自动编译/不可翻译的原因 | `$env:ANNOTA_JIT_DEBUG="1"` |
| `ANNOTA_INPUT` | 图形界面里预答 `input`（自动化测试用） | `$env:ANNOTA_INPUT="hello"` |
| `QT_QPA_PLATFORM` | Qt 平台插件（无头机器用 `offscreen`） | `$env:QT_QPA_PLATFORM="offscreen"` |
| `CXX` | `annota plugin build` 使用的编译器 | `$env:CXX="D:\Qt\Tools\mingw1310_64\bin\g++.exe"` |

## 6. C++ 插件：热插拔，不重编译解释器

一条命令把单个 C++ 文件编成插件，程序里 `use` 它即可：

```powershell
annota plugin build plugins\hello.cpp     # -> build\plugins\hello.dll
annota run examples\plugin.ant            # 程序里写 use hello
```

* 插件默认写到 **`<解释器目录>/plugins/`**，这个目录会被自动搜索，所以 `use hello` 直接可用；
* 放在别处也可以：`annota run x.ant --plugin .\hello.dll`，或设 `ANNOTA_PLUGIN`；
* 构建器自己找编译器（`CXX` → 解释器构建时记录的编译器 → `g++`/`clang++`），自动带上
  `src/ffi.hpp` 的包含路径并链接解释器的导入库；
* 静态分析器加载同一份插件，所以 IDE 里 `use hello` 不会报"未定义"；
* 写法（注册模块/函数、回调 Annota、参数与返回值转换）见 [ffi.md](ffi.md)；
  现成例子：`plugins/hello.cpp` + `examples/plugin.ant`。

构建器实际执行的就是下面这条命令（想看/想自己拼参数时用得上）：

```powershell
# Windows / MinGW
g++ -std=c++17 -O2 -shared -I src plugins\hello.cpp -o build\plugins\hello.dll `
    -static-libgcc -static-libstdc++ build\libannota.dll.a

# Linux / macOS
g++ -std=c++17 -O2 -shared -fPIC -I src plugins/hello.cpp -o build/plugins/libhello.so
```

Linux/macOS 上解释器需要**导出自己的符号**，插件才能解析宿主函数：`build.ps1`、`CMakeLists.txt`
（`ENABLE_EXPORTS`）以及 CI 的 `g++` 命令都带了 `-rdynamic`；自己手写编译命令时请一并带上。

## 7. 性能：自动 JIT 与基准

解释器会自己数循环，热到阈值就把函数编译成机器码，并在循环头**当场接管**（不必等下一次调用）。
`[[jit]]` 只表示"加载阶段就编译"，两者语义完全一致。

```powershell
build\annota.exe examples\perf.ant                  # 自带计时与正确性断言的基准
build\annota.exe bench                              # 分析器性能 + VM 微基准
build\annota.exe check --dump-bc examples\jit.ant   # 看融合后的超指令
$env:ANNOTA_JIT_DEBUG="1"; build\annota.exe examples\jit.ant    # 看谁被自动编译
```

`tools/cpp_baseline.cpp` 是同一批负载的 C++ 版本，用 `g++ -O2` 编译后可以对照：

```powershell
g++ -std=c++17 -O2 tools\cpp_baseline.cpp -o build\cpp_baseline.exe
build\cpp_baseline.exe
```

优化手段的取舍与实测表格见 [jit.md](jit.md) 与 [ffi.md](ffi.md)。

## 8. 文档一览

| 文档 | 内容 |
| --- | --- |
| [README.md](../README.md) / [README_zh.md](../README_zh.md) | 项目介绍、快速上手、性能对比 |
| [syntax.md](syntax.md) | 语法规范（含值与引用语义、`lend`） |
| [stdlib.md](stdlib.md) | 标准库 API |
| [reference.md](reference.md) | 标注、检查码、原语（**生成物**） |
| [style.md](style.md) | 代码规范 |
| [jit.md](jit.md) | 自动 JIT 与 `[[jit]]` |
| [ffi.md](ffi.md) | 链接 C++ / 插件 |
| [tools.md](tools.md) | 本文：构建、命令行、环境变量、插件 |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | 参与开发 |

## 9. 故障排查

<details>
<summary><b><code>cannot find module 'test'</code>（或任何标准库模块）</b></summary>

模块按顺序在下面这些目录里找：程序所在目录、它的 `lib/`、`../lib/`、解释器所在目录及其
`lib/`、`../lib`、当前目录的 `lib/`、当前目录，最后还有插件目录。所以从任意工作目录启动
都能找到标准库；如果仍然报错，用错误提示里的 `searched ...` 列表核对实际路径，或用
`annota analyze <file> --modules` 看它是否把 `use` 进来的模块也算进去了。
</details>

<details>
<summary><b>图形 IDE / 双击启动时程序跑不起来，但命令行没问题</b></summary>

先确认不是工作目录问题（§9 上一条已经覆盖）；再看 IDE 输出的完整报错。无头环境请设
`QT_QPA_PLATFORM=offscreen`，并用 `--run --echo` 把输出打到终端。
</details>

<details>
<summary><b><code>annota plugin build</code> 说找不到编译器</b></summary>

设置 `CXX` 指向编译器（完整路径最稳），例如：

```powershell
$env:CXX = "D:\Qt\Tools\mingw1310_64\bin\g++.exe"
annota plugin build plugins\hello.cpp
```

如果它在受限环境（某些沙箱/CI）里拒绝启动外部程序，直接用 §6 里的手工命令编译即可。
</details>

<details>
<summary><b>编辑器里从某一行开始整段变成注释色，但程序能跑</b></summary>

这是语法高亮的显示问题：`-[ ... ]-` 块注释的结束标记被漏掉了。用下面这条命令定位：

```powershell
build\annota.exe studio 你的文件.ant --check-highlight
```

它会在文档最后一块仍处于注释状态时报错并退出；修好 `]-` 即可。`build.ps1 -Verify` 会对
`examples/` 下的全部文件做这项检查。
</details>

<details>
<summary><b>热循环为什么没有变快</b></summary>

自动提升只作用于"能整体翻译成机器码"的函数（整数运算、比较、分支、返回；用到闭包、调用
容器方法、字符串等就不行）。用 `ANNOTA_JIT_DEBUG=1` 看原因：

```
[jit] 自动编译 sum_auto（机器码）          <- 已提升
[jit] 自动编译 f（不可翻译，继续解释）      <- 该函数不在整数子集里
```

调低阈值可以更早提升：`$env:ANNOTA_JIT_THRESHOLD="500"`；想完全关掉用 `ANNOTA_NO_JIT=1`。
</details>

---

相关： [syntax.md](syntax.md) · [stdlib.md](stdlib.md) · [jit.md](jit.md) · [ffi.md](ffi.md) ·
[reference.md](reference.md) · [style.md](style.md)
