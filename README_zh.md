<div align="center">

# Annota

**一门可静态验证的语言：C++17 手写词法/语法/字节码虚拟机，外加一整套工具链——REPL、三层静态分析器、图形化 IDE。**

[English](README.md) | [简体中文](README_zh.md)

[![build](https://github.com/Melba0/annota/actions/workflows/build.yml/badge.svg)](https://github.com/Melba0/annota/actions/workflows/build.yml)
[![Language](https://img.shields.io/badge/language-C%2B%2B17-blue.svg)](#构建)
[![GUI](https://img.shields.io/badge/GUI-Qt%206-green.svg)](#图形化-ide)
[![Tests](https://img.shields.io/badge/verification-build.ps1%20--Verify-success.svg)](#验证)
[![Analyzer](https://img.shields.io/badge/analyzer-32%20checks-orange.svg)](docs/reference.md)
[![License](https://img.shields.io/badge/license-see%20LICENSE-lightgrey.svg)](#许可证)

<img src="docs/images/ide-problems.png" alt="Annota Studio" width="820">

</div>

---

Annota 是一门"标注即规范"的小语言：`[[require]]`、`[[ensure]]`、`[[invariant]]`、`[[decrease]]`、
`[[taint]]` 这些标注不是装饰，而是**规格**——分析器会把它们变成证明义务或具体的缺陷报告。
解释器与分析器读的是同一份源码、同一棵语法树，所以"工具告诉你的事"和"程序实际做的事"不会脱节。

**特色**

* 🧩 **一套工具链，四种界面**——命令行、REPL、图形化 IDE（`annota studio`）、LSP 语言服务器。
  它们共享同一个前端与分析层，不存在"两份实现需要同步"的问题。
* 🔍 **三层静态分析**——击键（<50 ms）、保存（<500 ms）、后台（<5 s）三个预算，32 个诊断码，
  CFG 上的抽象解释，调用点契约检查。
* 🖥️ **真正的 Qt 6 界面**——既用于 Annota 程序自己的 `view` 组件，也用于写代码的 IDE；
  没有 Qt 时优雅降级为 `--gui-tree` + REPL + 完整分析器。
* 🔢 **C 风格数值宽度**——`int8..int64` / `uint8..uint64` / `float32` / `float64`，外加盒式的
  `longlong`（128 位）与 `longdouble`（80 位）；声明宽度按 C 语义回绕/舍入，整数除法向零截断。
* 🧱 **定长类型化数组**——`int[5]`、`int[3][4]`、`int[]`：连续存储、自动补 0、O(1) 行视图、
  静态越界证明，以及用 `[[unsafe]]` 关掉运行时检查的开关。
* ⚡ **`[[jit]]` 标记**——唯一的优化标记，让某函数在加载期做常量折叠与超指令融合，基准可量化。
* 🧭 **分类讨论与 `elif`**——`if` / `elif` / `else` 链；每条路径各自学习事实
  （`if i < 1 ( i = 1 )` 之后可证 `i >= 1`），会返回的分支不污染并集，
  并且每条诊断都会注明来自哪个分支。
* 🧠 **IDE 认得类型**——悬停 `int` / `long` / `longlong` / `double` 会给出类型信息
  （宽度、有无符号、怎么声明与转换），补全把它们作为 `type` 提供，编辑器也会高亮；
  这些名字来自 parser / 分析器 / 高亮器共用的同一份注册表。
* 📁 **开箱可用的标准库**——`Seq` / `Str` / `Dict` / `Mathx` / `File` / `Dir` / `Path` / `Test` / 切片，
  以及正确处理 UTF-8 路径的文件层与 JSON 模块。
* ✅ **自带验证**——`build.ps1 -Verify` 一条命令跑完 8 个示例、12 个分析用例、性能基准、
  LSP 端到端和 IDE 无头截图。

---

## 目录

- [快速开始](#快速开始)
- [30 行看懂这门语言](#30-行看懂这门语言)
- [命令行](#命令行)
- [REPL](#repl)
- [图形化 IDE](#图形化-ide)
- [静态分析](#静态分析)
- [编辑器集成（LSP）](#编辑器集成lsp)
- [标准库](#标准库)
- [原语](#原语)
- [文件与路径](#文件与路径)
- [目录结构](#目录结构)
- [验证](#验证)
- [性能](#性能)
- [常见问题](#常见问题)
- [参与贡献](#参与贡献)
- [许可证](#许可证)
- [实现要点](#实现要点)
- [与文档的取舍](#与文档的取舍)

---

## 快速开始

```powershell
git clone https://github.com/Melba0/annota.git
cd annota

# 构建（自动探测 D:\Qt、C:\Qt、%USERPROFILE%\Qt 下的 Qt 6）
powershell -ExecutionPolicy Bypass -File build.ps1

# 构建 + 跑完所有检查（示例、分析用例、基准、LSP、IDE）
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify

build\annota.exe examples\selfcheck.ant    # 72 条断言覆盖语言全部特性
build\annota.exe studio                    # 图形化 IDE
```

需要 C++17 编译器（MinGW-w64 g++ 13 或 MSVC 2022）。**Qt 6 Widgets 是可选的**：
没有它只是没有窗口和 IDE，其它功能照常。

| 构建方式 | 命令 | 得到 |
|---|---|---|
| 完整 | `build.ps1` | 解释器 + `--gui` + `annota studio` + LSP |
| 无 Qt | `build.ps1 -NoQt` | 解释器 + REPL + 分析器 + LSP（无窗口） |
| CMake | `cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt>/<版本>/<套件>` | 同"完整" |

随时可以确认某个二进制具备哪些能力：

```powershell
build\annota.exe --features
# contracts annotations files gui ide <- 完整构建
# contracts annotations files (+ 中文说明) <- 无 Qt 构建，会直接告诉你怎么办
```

## 30 行看懂这门语言

**没有 `main` 函数**：一个文件就是一个脚本，顶层语句按顺序执行；声明要写在使用之前。

```annota
-[ 标注就是规范 ]-
[[module: demo]]
[[require: divisor != 0]]
[[ensure: result * divisor == dividend]]
divide(dividend, divisor) = dividend / divisor

[[invariant: i >= 0]]
[[decrease: limit - i]]
count_to(limit)(
    new i = 0
    while i < limit( i = i + 1 )
    =i
)

-- 顶层代码就是入口
new xs = [3, 1, 4, 1, 5]
for x in sorted(xs)( print x )

new text = "hello"
print text.upper(), len(text)

new d = Dict()
d.set("k", 42)
print d.get("k")

print divide(10, 2)        -- 5
print count_to(4)          -- 4

try_it()                   -- 会抛异常，下面接住
except e( print "caught: " + e )
```

把代码包进 `main()` **不是必需的**，只会改变执行时机（函数体在调用前不执行）——
见 [docs/style.md](docs/style.md)。

语言特性：深拷贝值语义、闭包共享捕获、类与继承、魔法函数（`__len__` / `__str__` / `__call__` /
`__iter__` …）、声明式 `view` 组件与响应式 `state`、编译期宏、`Ok`/`Err`、切片（`xs[1:4]`）、
可选参数 / 具名参数 / 可变参数、分段表达式、`except` 错误处理。

### 语法细节

**构造函数。** 类名后的括号是构造参数；`__init__` **括号为空**，在构造时执行，并且能直接读到那些
构造参数（它们是局部变量）。字段默认值先应用，因此 `__init__` 可以覆盖它们。

```annota
Point(x, y)=(
    X:int              -- 声明字段，默认 0
    Y:int
    __init__()(
        X = x          -- x、y 就是类名括号里的构造参数
        Y = y
    )
    norm2() = X * X + Y * Y
)
new p = Point(3, 4)
print p.norm2()        -- 25
```

**一行多条语句。** 声明、赋值、删除都接受逗号列表；一行里的逗号也充当语句分隔符：

```annota
new a, b                   -- 一次声明两个
new x = 1, y = 2, z = 3    -- 各自带初值
a = 1, b = 2               -- 同行两条赋值
del a, b                   -- 一次删除多个
d + "y"                    -- 裸表达式语句也可以
```

**续行。** 行尾是二元运算符时自动续到下一行，也可以用 `\` 显式续行，长算式因此可以自然排版：

```annota
new total = 1 +
            2 +
            3                      -- 6
new joined = "a" + \
             "b"                   -- "ab"
```

**可变参数。** `*args`（Python 风格）与 `...args` 等价，收集到的是普通 `List`。
`**kwargs` **不支持**，请直接传 `Dict`。

```annota
total(*nums)(
    new s = 0
    for n in nums( s = s + n )
    =s
)
print total(), total(1), total(1, 2, 3)     -- 0 1 6

join_with(sep, *parts)( ... )               -- 定长参数在前，变长参数收尾
```

**切片**（`use slice`）保持类型：列表/元组切片是只读的 `SliceView`（可用 `.to_list()`、`len`、
遍历），而**字符串切片返回字符串**。

```annota
new arr = [10, 20, 30, 40, 50]
print arr[1:3].to_list()   -- [20, 30]
print arr[2:]              -- SliceView(30, 40, 50)
new text = "abcdefg"
print text[2:5]            -- "cde"（是字符串，不是视图）
```

**两条容易踩的格式规则。** `else` 必须与 `if` 体结尾的 `)` **同一行**——解析器靠这一点区分语句
与表达式：

```annota
if n > 0( print "positive" ) else ( print "not positive" )     -- ✓
if n > 0(
    print "positive"
) else (                                                        -- ✓ `) else (` 要在一行
    print "not positive"
)
```

语句不能跨行断开，除非行尾是运算符、括号内的逗号，或显式写了 `\`：

```annota
new s = "a" +
        "b"          -- ✓ 续行
new t = "a"
        + "b"        -- ✗ `+ "b"` 会被当成新语句
```

📘 **[语法规范](docs/syntax.md)**——词法、语句、表达式、运算符优先级，以及续行与 `else` 的确切规则。
📘 **[代码规范](docs/style.md)**——文件怎么写、怎么命名、标注怎么用、提交前检查清单。
📘 **[`[[jit]]`](docs/jit.md)**——标记到底做了什么：超指令融合，以及整数子集的 x86-64 机器码后端，
翻译不了就自动回退解释执行。
📘 **[链接 C++（FFI）](docs/ffi.md)**——从 C++ 注册原生函数与模块（可静态链接，也可做成插件加载），
让标准库变快而不必改语法核心。

标准库就是这么分层的：语言核心只有词法/语法/编译/虚拟机，`lib/*.mod` 是可读的脚本层，
热内核放在通过 FFI 注册的 C++ 里——`Seq.sort`、`kth`、`median`、`dedup`、`sort_by`、
`lower_bound`、`upper_bound` 调用的就是原生模块 `seqnative`（`native/seq_native.cpp`），
它完全不属于核心。

## 命令行

```
annota                          REPL（无参数时的默认行为）
annota studio [file.ant]        图形化 IDE（Qt 构建）
annota run|check <file.ant>     执行 / 只做语法检查
annota analyze <file> [选项]    静态分析（见 docs/reference.md）
annota analyze-suite <dir>      跑分析用例集
annota ide <query> <file> ...   悬停 / 跳转 / 重命名 / 补全 / 内联状态 / 覆盖率 /
                                快速修复 / 抑制清单 / 报告 / 检查表 / 标注表 / 文档
annota bench                    分析器性能基准
annota lsp                      语言服务器（stdio，JSON-RPC）

  -e <code>            执行一段代码
  --dump-tokens        打印词法单元流
  --dump-ast           打印语法树
  --dump-bc            反汇编字节码
  --dump-annotations   以 JSON 打印标注索引
  --contracts          运行期检查 assert/require/ensure/invariant
  --gui                在窗口中显示程序自己的 view（Qt 构建）
  --gui-tree           以文本打印组件树（无需 Qt）
  --gui-shot <png>     不开窗，把 view 渲染成 PNG
  --features           报告本二进制的可选能力
```

## REPL

`annota` 不带参数进入交互式解释器，整个会话共用一个 VM，定义可以累积：

```
annota> new x = 10
annota> x * 3
30
annota> add(a, b)( =a + b )
annota> add(1, 2)
3
annota> :analyze
<repl>: 0 errors, 0 warnings, 0 infos (1.0 ms, 6 lines)
```

| 命令 | 作用 |
|---|---|
| `:help` / `:quit` | 帮助 / 退出 |
| `:globals` / `:macros` | 列出全局与已注册宏 |
| `:load <file>` | 在会话中执行文件 |
| `:analyze [片段]` | 对会话历史（或一段代码）做完整分析 |
| `:bc <code>` / `:tokens <code>` | 反汇编 / 打印词法单元 |
| `:reset` | 清空全局、类与宏 |

括号未闭合时提示符变成 `...>`，函数、类、列表都可以跨行输入。

## 图形化 IDE

```powershell
build\annota.exe studio examples\algorithms.ant
```

| 问题面板 | 结构面板 | 覆盖率面板 |
|---|---|---|
| ![问题](docs/images/ide-problems.png) | ![结构](docs/images/ide-structure.png) | ![覆盖率](docs/images/ide-coverage.png) |

* **编辑器**——行号、Annota 语法高亮、诊断波浪线、当前行高亮、自动缩进、括号配对、`Tab` 四空格。
* **问题**——级别 / 行号 / 检查码 / 说明，**双击跳转到该行**；悬停显示抽象值与修复建议。
* **结构**——每个函数及其 **✓ / ? / ✗** 契约状态，双击跳转。
* **覆盖率**——契约与循环不变量覆盖率、尚未写契约的函数清单、`[[ignore]]` 抑制清单
  （含**已经失效的抑制**），以及全部可用的快速修复。
* **输出**——`F5` 运行当前缓冲区（`Ctrl+F5` 带契约检查），失败时自动跳到出错行；
  程序里定义了 `view` 时，输出面板会提示你按 `F7`。
* **预览界面（`F7`）**——运行后把程序自己的 `view` 作为真正的 Qt 窗口打开，并接上它的 `state`，
  按钮、计数器都能用；`Ctrl+F7` 则打印组件树。**只按 `F5` 不会开窗**——这就是"写了图形界面却
  什么都没出现"的原因：`F5` 是控制台运行，图形界面要用 `F7` 预览（命令行等价物是 `--gui`）。
* **悬停面板与状态栏**——标注文档、光标处变量的抽象状态、本行诊断，以及 `✓ ? ✗` 函数汇总。
* **`input` 在 IDE 里也能用**——在 IDE（以及 `--gui` 运行的程序）里，`input name` 会弹出以变量名
  为标题的输入对话框，而不是去等一个 GUI 程序通常没有的控制台；取消对话框得到空字符串。
  自动化运行可用 `ANNOTA_INPUT=<文本>` 预填答案（UTF-8，非 UTF-8 控制台下建议用 ASCII），
  CI 就是这么测的。例子见 [examples/input.ant](examples/input.ant)。
* **帮助菜单**——标注手册（`F1`）、检查手册（`F2`）、文件 API，全部**从分析器注册表生成**。

快捷键：`F5`/`Ctrl+F5` 运行、`F7`/`Ctrl+F7` 预览程序界面（或打印组件树）、`F6` 分析、
`Ctrl+S` 保存、`Ctrl+/` 注释、`Ctrl+1` 插入选中修复、`Ctrl+Shift+1` 插入全部修复。

IDE 也能无头运行（CI 就是这么验证的）：

```powershell
build\annota.exe studio examples/files.ant --run --shot ide.png            # 截图后退出
build\annota.exe studio examples/gui_counter.ant --preview --shot p.png    # 同时打开 view 窗口
```

用 Annota 写的程序也能开自己的窗口：

![Annota view 窗口](docs/images/gui-counter.png)

```powershell
build\annota.exe examples\gui_counter.ant --gui
```

## 静态分析

```powershell
build\annota.exe analyze examples/analysis/01_basics.ant
build\annota.exe analyze file.ant --json > report.json
build\annota.exe analyze file.ant --level=1 --min-severity=warning
build\annota.exe analyze file.ant --int-bits=64 --modules
```

三层结构与延迟预算：

| 层 | 触发 | 内容 | 预算 |
|---|---|---|---|
| L1 | 击键 | 词法/语法错误、括号不匹配、标注注册表/作用域/冲突 | 50 ms |
| L2 | 保存 | 过程内数据流：除零、溢出、越界、空引用、未初始化、死存储、不可达、类型/作用域/迭代器、调用点契约、`ensure`、`invariant` | 500 ms |
| L3 | 后台 | 循环不变量保持、终止性、污点、纯函数、宏副作用、冗余条件 | 5 s |

**32 个诊断码**都是稳定标识（`severity`：`error` 可证伪、`warning` 可能、`info` 提示）。
报告也可输出为 JSON：

```json
{
  "file": "examples/analysis/01_basics.ant",
  "errors": 5, "warnings": 0, "infos": 3, "elapsedMs": 2,
  "diagnostics": [
    { "line": 12, "column": 1, "severity": "error", "code": "division-by-zero",
      "message": "除零", "detail": "除数恒为 0",
      "fix": "加 [[assert: b != 0]] 或 [[require: b != 0]]" }
  ]
}
```

> 诊断**消息是中文**，而诊断码、严重度与 JSON 结构是英文——工具应以 `code` 为键。
> 完整清单见 [docs/reference.md](docs/reference.md)（由 `annota ide docs` 从注册表生成，
> 不会与实现脱节）。

**内置一个很小的 CAS**：条件会被规范化成整数线性式（`sum(k_i * v_i) + c`），于是
`assert(i + 1 > i)`、`assert(2 * k == k + k)`、`assert(n - 1 < n)` 是**被证明**而不是报
“无法证明”；`[[assume: x == 5]]` 作为代入使 `assert(x * 2 == 10)` 可证；
`if a + 1 > a` 会被指出是冗余条件。

**标注即规范**：`[[assume]]` 缩小抽象状态；`[[assert]]` 必须被证明；`[[require]]` 在函数入口被假设、
并**在每个调用点被检查**；`[[ensure]]` 在每个返回点代入 `result` 检查；`[[invariant]]` 在循环入口与
每次迭代末尾检查；`[[decrease]]` 必须为正且严格递减；`[[taint]]` 标记来源、`[[unsafe]]` 标记边界；
`[[ignore: 码]]` 抑制一条检查（失效时会被报告为"未使用"）。

## 编辑器集成（LSP）

`annota lsp` 用 stdio 上的标准 LSP 帧（`Content-Length`）通信：

* `didOpen` / `didChange` → 立刻推送 L1 诊断（击键预算）
* `didSave` → 同步推送 L2，然后**在可取消的后台线程**跑 L3
* `hover`、`definition`、`rename`（含标注名）、`completion`

VS Code 风格的注册方式：

```json
{ "command": "D:\\path\\to\\build\\annota.exe", "args": ["lsp"], "filePattern": "*.ant" }
```

```powershell
powershell -ExecutionPolicy Bypass -File tools/lsp_smoke.ps1    # 端到端自检
```

## 标准库

运行时刻意做得很少：C++ 只暴露 `_` 前缀的**原语**（见 [原语](#原语)），其余全部是 `lib/` 下
用 Annota 写的模块。因此"加一个能力"通常意味着加一个 `.mod` 文件，而不是重新编译解释器。

原生模块（运行时注册，始终可用）：`math`、`io`、`json`，加上很小的时钟模块 `time`
和原语门面 `system`。

`lib/` 下的脚本模块，用 `use <名字>` 载入，或一次 `use std`：

`lib/` 下的脚本模块，用 `use <名字>` 载入（子目录写法 `use sub/名字`），或一次 `use std`。
模块找不到会直接报错，并列出搜索路径、可用模块与最接近的名字。完整 API 见
**[docs/stdlib.md](docs/stdlib.md)**。

| 模块 | 对象 | 内容 |
|---|---|---|
| `seq` | `Seq` `Heap` | `map` `filter` `reduce` `scan` `zip_with` `chunk` `window` `rotate` `flatten` `transpose`；**算法**：`merge_sort` `sort_by` `heap_sort` `insertion_sort` `kth` `median` `quantile` `lower_bound` `upper_bound` `bsearch` `merge_sorted` `insert_sorted`；**集合**：`dedup` `union` `intersect` `difference` `is_subset`；**统计**：`mean` `variance` `stddev` `frequencies` `mode` `min_max`；`Heap.from_list(...).drain()` |
| `dict` | `Dict` `Set` `Counter` | 开放寻址哈希表（FNV-1a，负载 0.75 扩容 / 0.125 收缩）：`set` `get` `get_or` `has` `remove` `items_sorted` `map_values` `filter` `invert` `merge` `copy` `d[k]` `len(d)`；`Set` 交并差；`Counter` `most_common` `total` |
| `text` | `Text` | KMP `find_all` `find_kmp` `split_by` `replace_all` `words` `wrap` `snake_case` `camel_case` `levenshtein` `similarity` `lcs` `is_palindrome` `is_anagram` `caesar` `rot13` `csv_parse` `csv_format` `base64_encode` `hex_dump` `ngrams` `char_freq` `word_freq` |
| `numeric` | `Num` | `sieve` `is_prime` `nth_prime` `prime_sum` `prime_factors` `divisors` `divisor_count` `divisor_sum` `totient` `goldbach` `mod_pow` `mod_inverse` `gcd_ext` `fib`（快速倍增）`collatz_*` `binomial` `pascal_row` `catalan` `integer_sqrt` `digits` `base_str` `parse_base` `binary_search_monotone` `hanoi` |
| `matrix` | `Matrix` | 行优先一维存储，`mul`（i-k-j）`pow`（快速幂）`det`（高斯消元）`transpose` `trace` `add` `scale` `mul_list` `from_lists` `to_lists` `identity` |
| `stats` | `Stats` | `mean` `weighted_mean` `median` `mode` `variance` `stddev` `percentile` `iqr` `skewness` `kurtosis` `covariance` `correlation` `rank` `zscores` `normalize` `histogram` `moving_average` `summary` |
| `graph` | `Graph` `DSU` | `bfs` `dfs` `distances` `shortest_path`（Dijkstra + 二叉堆）`all_pairs`（Floyd–Warshall）`topological_sort`（Kahn）`has_cycle` `connected_components` `is_bipartite` `kruskal`；`DSU` 路径压缩 + 按秩合并 |
| `geometry` | `Geo` | `dist` `dist2` `cross` `orientation` `segments_intersect` `polygon_area`（鞋带）`point_in_polygon`（射线法）`convex_hull`（Andrew 单调链 O(n log n)）`closest_pair`（分治）`bounding_box` `centroid` |
| `slice` | `Slice` `SliceView` | `arr[1:4]`、`arr[2:]`、`arr[:3]`、`arr[:]` |
| `str` | `Str` | `repeat` `pad_left` `pad_right` `center` `reverse` `capitalize` `title` `count` `blank` `is_digit` `lines` `join` `hex` `format` |
| `mathx` | `Mathx` | `gcd` `lcm` `factorial` `fib` `fib_iter` `is_prime` `primes` `clamp` `lerp` `round_to` `mean` `median` `variance` `stddev` `is_even` `is_odd` `digit_sum` |
| `file` | `File` `Dir` `Path` `FileText` `Result` | 读写、复制、遍历、路径拼接 |
| `time` | `Time` `Stopwatch` | `millis` `clock` `parts` `make` `format`（strftime 风格，脚本实现）`iso` `date` `stamp_text` `leap` `days_in_month` `diff_ms` `describe_ms` |
| `os` | `Os` | `platform` `arch` `cpus` `pid` `home` `temp` `cwd` `env` `set_env` `env_all` `exec` `run` `ok` `which` |
| `thread` | `Thread` `Task` `Pool` | `hardware` `spawn` `run` `parallel_map` `parallel_each` `pool` |
| `net` | `Net` `Tcp` `Url` `Response` | `tcp` `server` `get` `post` `request` `parse_http` `resolve` |
| `test` | `Test` | `suite` `ok` `eq` `ne` `near` `raises` `report` `check` |

```powershell
build\annota.exe examples\stdlib.ant       # 标准库导览（69 条断言）
build\annota.exe examples\algorithms.ant   # 排序 / 动态规划 / 图 / 数论 / 几何（82 条）
build\annota.exe examples\perf.ant         # 性能基准：12 个工作负载 + 计时表
build\annota.exe examples\collections.ant  # 哈希容器，哈希 vs 线性扫描
build\annota.exe examples\strings.ant      # 字符串算法
build\annota.exe examples\graphs.ant       # BFS / Dijkstra / 拓扑 / MST / 迷宫寻路
build\annota.exe examples\numerics.ant     # 数论练习册
build\annota.exe examples\geometry.ant     # 凸包 / 面积 / 最近点对
build\annota.exe examples\files.ant        # 文件 API，39 条断言
build\annota.exe examples\system.ant       # 时钟、线程、套接字、HTTP 解析（53 条断言）
```


## 原语

C++ 只是操作系统之上的一层薄壳。下面是 `.mod` 无法自己表达的东西；`docs/reference.md`
会列出完整签名。

| 原语 | 用途 |
|---|---|
| `_sys_clock()` `_sys_time()` `_sys_sleep(ms)` | 单调毫秒、墙上毫秒、休眠 |
| `_sys_local_time([ms])` `_sys_make_time(y,mo,d,…)` | 时间分量 ⇄ 时间戳 |
| `_sys_info(key)` | `platform` `arch` `cpus` `pid` `home` `temp` `cwd` |
| `_sys_env` `_sys_env_set` `_sys_env_all` `_sys_exec` | 环境变量与外部命令 |
| `_sys_spawn(fn[, args])` `_sys_task_done` `_sys_join` | 工作线程 |
| `_sys_socket(op, …)` | `resolve` `connect` `listen` `accept` `send` `recv` `close` `shutdown` `timeout` |
| `_file_*` `_dir_*` `_path_*` `_cwd` `_chdir` `_stdin_*` | 文件、目录、路径、标准输入 |

每一个都在 `system` 门面里按名字镜像了一份，库代码因此可以动态分发：

```annota
print system.clock()                    -- 等价于 _sys_clock()
print system.call("info", "platform")   -- 动态调用 _sys_info("platform")
for p in system.primitives( print p )   -- 这个构建提供了哪些原语
```

`time.millis()` / `time.now()` / `time.clock()` / `time.sleep(ms)` 不需要 `use time` 就能用，
因为"量时间"到处都要。

线程：工作线程有独立的 VM，全局变量是调用方的**深拷贝**（代码、类、原生函数是共享的，
因为它们不可变）。闭包捕获的单元仍然共享，所以工作函数要把捕获到的状态当作只读——
分析器的 `[[pure]]` 就是用来盯这件事的。

## 文件与路径

分两层。**下划线前缀的原语**是运行时唯一接触操作系统的入口，出错即抛出，任何地方都能用：

```annota
print _file_read("notes.txt")           -- 出错抛出，用 except 捕获
print _file_write("out.txt", "hello")   -- 返回写入字节数
print _dir_list(".")                    -- 已排序的名字列表
print _path_join("build", "x.txt")      -- 使用当前平台分隔符
```

**上层 API**（`use file`）把它们包成三种错误处理风格：

```annota
use file

new text  = File.read("a.txt")                  -- 1) 抛异常，配合 except
new probe = File.try_read("a.txt")              -- 2) Result：is_ok / unwrap / unwrap_or
new safe  = File.read_or("a.txt", "默认值")      -- 3) 带默认值

new lines = File.lines("a.txt")
new obj   = File.json("d.json")
new rows  = File.csv("t.csv")

File.write("a.txt", "内容")
File.append_line("a.txt", "再追加一行")
File.write_lines("a.txt", ["x", "y"])
File.write_json("d.json", {"n": 1})

File.exists(p)  File.is_file(p)  File.is_dir(p)  File.size(p)  File.describe(p)
File.copy(from, to)  File.move(from, to)  File.remove(p)  File.touch(p)

Dir.list(p)  Dir.files(p)  Dir.dirs(p)  Dir.walk(p)  Dir.find(p, ".ant")  Dir.size_of(p)
Dir.make(p)  Dir.remove(p, recursive)  Dir.clear(p)

Path.join(a, b[, c])  Path.dirname(p)  Path.basename(p)  Path.ext(p)  Path.stem(p)
Path.with_ext(p, ".md")  Path.abs(p)  Path.normalize(p)  Path.parts(p)
```

路径中的中文等非 ASCII 字符会转换成 Windows 宽字符 API，因此 `文件/笔记.txt` 可用；
读取时换行统一为 `\n`。原语完整清单见
[docs/reference.md](docs/reference.md#3-file-and-path-primitives)。

## 目录结构

```
src/                    约 13k 行 C++17
  lexer, parser          词法、宏引擎、标注解析、模块加载
  ast, compiler, vm      字节码编译器与栈式虚拟机（深拷贝语义）
  builtins               内置函数、伪方法、GUI 组件、原生模块
  gui_qt / gui           Qt 视图渲染 / 文本组件树与无 Qt 桩函数
  repl                   交互式解释器（持久 VM）
  analyzer, analysis_flow 标注注册表、CFG、抽象解释、检查实现
  ide_cmd, ide_gui, lsp  CLI 查询、图形化 IDE、语言服务器
lib/                    标准库模块（*.mod）
examples/               示例程序，含 examples/analysis/ 分析用例
docs/                   参考手册（生成）与截图
tools/lsp_smoke.ps1     LSP 端到端测试
build.ps1               一键构建 + 验证
```

## 验证

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify
```

| 步骤 | 证明了什么 |
|---|---|
| 8 个示例程序 | 语言语义没有回归（仅 `selfcheck.ant` 就有 72 条断言） |
| 12 个分析用例 | 每条检查该报的报、不该报的不报 |
| 对示例跑分析器 | 真实且正确的代码上**零误报** |
| `annota bench` | 三层延迟预算达标 |
| LSP 冒烟测试 | 完整编辑器会话（打开 → 悬停 → 重命名 → 保存 → 诊断） |
| `studio --shot` | IDE 能建窗并完成无头渲染 |

## 性能

`annota bench` 生成约 2000 行、80 个带契约函数的程序，分别测量三层
（MinGW-w64 g++ 13.1，`-O2`，单核）：

| 层 | 预算 | 实测 |
|---|---|---|
| L1 击键 | 50 ms | **8 ms** |
| L2 保存 | 500 ms | **23 ms** |
| L3 后台 | 5 s | **23 ms** |

## 常见问题

<details>
<summary><b><code>--gui</code> 或 <code>annota studio</code> 提示"这个二进制没有 Qt 支持"</b></summary>

说明这个 exe 是无 Qt 构建。检查并重建：

```powershell
build\annota.exe --features        # 需要输出里有 gui ide
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean -QtRoot "D:\Qt\6.10.1\mingw_64"
```

`build.ps1` 会自动探测 `D:\Qt`、`C:\Qt`、`%USERPROFILE%\Qt` 下的
`6.x/{mingw_64,msvc2022_64,msvc2019_64}`；找不到时会**黄字提醒**并构建无 Qt 版本。
用 CMake 时记得传 `-DCMAKE_PREFIX_PATH=<Qt>/<版本>/<套件>`。
如果 `build/` 里缺少 `Qt6Widgets.dll` 或 `platforms/qwindows.dll`，
报错会是 `no Qt platform plugin could be initialized`。
</details>

<details>
<summary><b><code>--gui</code> 没反应 / 窗口一闪而过</b></summary>

`--gui` 显示的是**程序自己的 `view`**，程序里得先有 `view`；
而 `annota studio` 是 IDE，用 IDE **不需要**任何 `view`。

```powershell
build\annota.exe examples\gui_counter.ant --gui        # 有窗口
build\annota.exe examples\algorithms.ant --gui-tree    # 没有 view：什么都不会打印
```

在 IDE 里 **`F5` 只运行程序并显示控制台输出，不会打开 `view` 窗口**；
要开窗请按 **`F7`**（运行菜单 → 预览界面）。程序里定义了 `view` 时，输出面板会主动提示你。
完整例子见 [examples/gui_counter.ant](examples/gui_counter.ant)。
</details>

<details>
<summary><b><code>input</code> 读不到东西 / 程序卡住</b></summary>

控制台运行时 `input` 从 **stdin** 读；没有输入就会立刻读到空字符串（EOF）：

```powershell
"world" | build\annota.exe examples\input.ant      # 控制台：从管道读
build\annota.exe examples\input.ant                # 交互式控制台：手输一行
```

窗口里没有控制台可读，所以 Qt 构建改为弹出**输入对话框**：用 `--gui` 运行程序，
或在 `annota studio` 里按 `F5` / `F7`。取消对话框会得到空字符串，程序应当能处理 `""`。
`_stdin_line()` 遵循同样的规则。

自动化跑 GUI 时可以用环境变量预填答案：

```powershell
$env:ANNOTA_INPUT = "hello"
build\annota.exe studio examples\input.ant --run --echo --shot out.png
```
</details>

<details>
<summary><b>分析器报"使用了未声明的变量"</b></summary>

先看是不是拼写错误，或者变量只存在于 `use` 进来的模块里。分析默认只报告主文件；
`annota analyze --modules` 会连内联模块一起分析，并在每条诊断前标出它来自哪个 `.mod`。
</details>

<details>
<summary><b>Windows 上中文路径读写失败</b></summary>

用 `use file`（`File` / `Dir` / `Path`）或 `_file_*` 原语，二者都做了 UTF-8 → 宽字符转换。
`io.read_file` 在非 UTF-8 代码页下可能失败。
</details>

更多内容见 [docs/reference.md](docs/reference.md)。

## 参与贡献

欢迎提 Issue 与 PR——构建、测试与代码风格约定见 [CONTRIBUTING.md](CONTRIBUTING.md)，
版本历史见 [CHANGELOG.md](CHANGELOG.md)。

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify   # 提 PR 前必须通过
```

## 许可证

遵循本仓库 [LICENSE](LICENSE) 文件的条款发布。

第三方组件：仓库不内嵌任何第三方源码。Qt 构建会链接 Qt（LGPL-3.0）——
`build.ps1` 复制到 `build/` 的 DLL 需按 Qt 的条款分发。

## 实现要点

<details>
<summary><b>值表示与深拷贝</b></summary>

`Value` 是带标签的结构体（`VT` 枚举 + 标量 + `shared_ptr<Obj>`）。`Obj` 是"胖对象"，
按种类复用字段：字符串、字节、列表项、成员表、函数块、迭代器状态、类信息、实例字段。
所有堆对象由 `shared_ptr` 管理，闭包捕获的变量是 `shared_ptr<Value>` 单元，
因此**捕获即共享**，不需要 open/close upvalue。

文档要求"所有赋值都是深拷贝"，因此 `OP_SET_LOCAL/SET_GLOBAL/SET_FIELD/SET_INDEX/SET_UPVAL`、
实参绑定、返回值都调用 `deepCopy()`；自引用结构用 `Obj* → Value` 记忆表处理。
唯一例外是方法接收者 `this`（按单元绑定），否则 `counter.inc()` 与 GUI 字段读写无法工作。
</details>

<details>
<summary><b>作用域与闭包</b></summary>

函数帧的局部变量槽在编译期确定（`Chunk::numLocals` 取槽位高水位，块退出后可复用）。
顶层作用域深度为 0 的 `new` 生成全局变量，更深的块（裸作用域 `( ... )`）生成帧内局部变量——
于是"块内 `new` 的变量出块即失效"自然成立。
</details>

<details>
<summary><b>类与组件</b></summary>

类体在编译期拆成三部分：构造体（字段初始化 + `:父类(...)`）、方法、组件构建体
（`buildFn`，类体里出现视图语句时自动成为组件）。`this` 是方法帧的 0 号槽位；
方法内**裸名字**按 `局部 → 上值 → 本类字段 → 全局` 解析。
</details>

<details>
<summary><b>GUI：Qt 窗口</b></summary>

`gui_qt.cpp` 用一个自绘 `QWidget` 承载整棵组件树，而不是为每个节点创建子控件——
这样每次 `state` 变化重建视图时不会反复创建/销毁窗口部件。

* **布局**：先自底向上 `measure()`（文本用 `QFontMetrics`，容器累加 `pad`/`gap`），
  再自顶向下 `layoutNode()` 分配矩形；`Column`/`Row` 支持 `align`，`Spacer` 吸收剩余空间，
  `Stack` 重叠，`Scroll` 裁剪 + 滚轮偏移。
* **绘制**：`QPainter` 抗锯齿绘制圆角按钮（悬停/按下/禁用三态）、输入框、文本、图片、
  复选框、滑块；`Image` 找不到文件时画虚线占位框。
* **事件**：绘制顺序被记录成列表，命中测试反向遍历并考虑裁剪矩形；按钮按下与抬起落在同一
  节点才触发 `click`；输入框维护编辑缓冲并在修改时调用 `change`、回车调用 `submit`。
* **响应式**：`VM::onStateChange` 只置脏标记，下一次 `paintEvent` 才重建视图，
  避免在处理函数内部重入虚拟机。
* **无 Qt 构建**：`gui.cpp` 提供桩函数，CLI 打印可照做的中文提示并回退到 `--gui-tree`。
</details>

<details>
<summary><b>分析层：抽象域与 CFG</b></summary>

每个变量一个抽象值：`range`（区间/unknown）、`size`（序列长度区间）、`null`（三态）、
`init`（是否初始化）、`taint`（来源）、`type`。合并规则：区间取凸包、三态合并、
任一分支未初始化即为未初始化、污点取或。

由于语言是深拷贝值语义，**不需要别名分析**。循环用"两遍迭代 + 加宽"求不动点：
循环体改写的变量在出口处放宽到 unknown，避免用一次迭代的结论去推断整个循环。

语句级 CFG（`if` 双边、`while`/`for` 回边、`break`/`continue` 跳转、`return`/`throw` 无后继、
`except` 从块入口引异常边）用于不可达代码与活跃性分析（死存储、未使用变量）。
</details>

## 与文档的取舍

<details>
<summary><b>语言层面的判定与差异</b></summary>

1. **类定义 vs 单行函数**：`f(a) = expr` 与 `Name(a)=( 成员 )` 都以 `=` 开头。判定方法是看
   `=(` 后面是否是成员声明（`:父类`、`字段:类型`、`方法(...) =`、`[[标注]]`）。
2. **方法 vs 组件调用**：类体中 `名字(参数)(子元素)` 二者同形，按"组件名首字母大写"区分。
3. **类型转换 vs 匿名函数**：内置类型名/已声明类名后跟操作数时按转换解析，否则紧跟 `(` 按匿名函数解析。
4. **类型标注不参与运行期检查**：只用于字段默认值推导与分析器。
5. **`[[private]]` / `[[public]]` 不强制**：作为元数据记录，不做访问控制。
6. **整数除法与 C 对齐**：两侧都是整数时向零截断（`7 / 2` → `3`，`-7 / 2` → `-3`），
   任一侧是浮点才是浮点除法（`7 / 2.0` → `3.5`）。
7. **`for (a, b) in xs`**：元素是二元组/列表时按下标 0、1 解构，否则退化为 `(下标, 元素)`。
8. **`children()`**：编译成取当前实例的 `__children` 字段。
9. **递归宏**必然触发 `[[macro_depth]]` 限制。
10. **`input a`** 一次只读一行赋给一个变量。
11. **`to` / `step` 是软关键字**：只有在两个操作数之间才是区间语法，因此 `copy(from, to)` 这类
    参数名照常可用。
12. **`sep` 不是关键字**：`print a, b, sep="-"` 由语法层面特判，`sep` 仍可作普通标识符。
13. **`a to b` 只向上计数**：`10 to 1` 是空区间，倒序需显式 `step -1`。
</details>

<details>
<summary><b>分析层的边界</b></summary>

14. **过程间分析限于单文件 + 单模块**：按名字解析被调函数，不解析虚调用、高阶参数或跨文件符号。
15. **模块内联代码默认不报告**：`use` 进来的语句带来源标记，默认跳过（`--modules` 打开并标注来源）。
16. **整数宽度**：运行期是 64 位整数，溢出检查默认按文档的 32 位判定（`--int-bits=64` 可切换）。
17. **不动点精度**：循环用"两遍迭代 + 加宽"，深层嵌套或需要区间收窄的场景只给 `?`，
    不会给出错误的 `✗`；`[[assert]]` 是缩小状态的手段。
18. **污点是值级、非净化感知**：不做字段敏感或路径敏感的净化分析，净化处需显式 `[[ignore: taint]]`。
19. **`[[trusted]]` 关闭该函数体分析**：其 `require` 仅作为文档保留。
20. **宏副作用检查发生在展开期**：仅当同一占位符在宏体中出现多次且对应实参含调用时报告。
</details>

## 示例

| 文件 | 内容 |
|---|---|
| `examples/selfcheck.ant` | **自检**：72 条断言覆盖语言全部特性 |
| `examples/stdlib.ant` | 标准库导览（Seq / Str / Dict / Mathx / slice / Test） |
| `examples/algorithms.ant` | 快排/归并/冒泡、二分查找、记忆化斐波那契、LCS、编辑距离、筛法、矩阵乘法 |
| `examples/files.ant` | 文件与目录 API，39 条断言 |
| `examples/input.ant` | `input`：控制台读 stdin，GUI/IDE 弹对话框 |
| `examples/smoke.ant` | 各类语法的最小集合 |
| `examples/buffer.ant` | 文档第 17 章综合示例（`--contracts` 可开契约检查） |
| `examples/gui_counter.ant` | 文档 15.12 GUI 示例，`--gui` 开窗、`--gui-shot` 出图 |
| `examples/modules.ant` + `shapes.mod` | 自定义模块、`__call__`/`__get__`/`__set__`/`__iter__` 魔法函数 |
| `examples/analysis/*.ant` | 12 个静态分析用例（正例 / 反例 / 边界 / 抑制） |
