# Annota 代码规范

目标是让代码**一眼看出意图、一眼看出规格**：Annota 的标注不是注释，而是工具会去证明或反驳的
规范。写代码时把"我想让分析器保证什么"和"代码怎么跑"分开考虑，会省下大量调试时间。

语法细节见 [syntax.md](syntax.md)；标注与检查码清单见 [reference.md](reference.md)。

---

## 1. 文件结构

按固定顺序排列，读者不用搜索：

```annota
-[ 一句话说明这个文件做什么 ]-
[[module: order]]
[[version: 1.0]]

use std                       -- 1. 依赖

const MAX_ITEMS = 100         -- 2. 常量
new total = 0                 -- 3. 全局可变状态（尽量少，能让分析器困惑）

Helper()=( ... )              -- 4. 类

helper(x) = ...               -- 5. 函数：先小工具，后主逻辑

-- 6. 顶层代码（程序的入口）：读起来像一段脚本
new data = load("in.txt")
print summarize(data)
```

* **不要包 `main()`**。顶层就是入口；包一层只会让"什么时候执行"变得不明显。需要参数时用
  命令行 `annota tool.ant -- arg1 arg2` 或环境变量/文件。
* 一个文件只做一件事；超过 ~400 行考虑拆模块（`lib/` 下的模块用 `use` 引入）。
* 文件头注释用 `-[ ... ]-`，写清职责与用法，不要复述代码。

## 2. 命名

| 对象 | 形式 | 例 |
|---|---|---|
| 类 / 视图 / 组件 | `PascalCase` | `OrderBook`、`Counter` |
| 函数、方法、变量 | `snake_case` | `read_config`、`total_price` |
| 常量 | `UPPER_SNAKE` | `MAX_ITEMS` |
| 私有成员 | 前缀 `_` | `_scratch` |
| 底层原语 | 前缀 `_`，**用户代码不要直接用** | `_file_read` → 用 `File.read` |
| 类型注解 | 小写内建名 | `int` / `float` / `bool` / `List` / `String` |

函数名用**动词短语**（`parse_header`），布尔用**断言式**（`is_empty`、`has_key`），
避免 `data2`、`tmp`、`flag` 这类无信息名字。

## 3. 排版

* 缩进 **4 空格**（IDE 的 `Tab` 已按 4 空格处理），不要混用。
* 一行一条语句；只有语义上成对的才用逗号同行：`new x = 1, y = 2`、`a = 1, b = 2`。
* 块体：空括号块**优先一行写完**短的，长块起头换行、闭合 `)` 单独一行。

```annota
if n < 0( =0 )                      -- 短：一行

if n < 0(                           -- 长：多行，`) else (` 同行
    print "negative"
    =0
) else (
    =n
)
```

* 长表达式用**行尾运算符**续行，与运算符对齐：

```annota
new message = "order " + str(id) +
              " has " + str(count) + " items"
```

* 空行分组：函数之间 1 行，逻辑段之间 1 行。不要连续 3 行以上空行。
* 行宽建议 ≤ 100 字符。

## 4. 声明与类型

* **每个字段/参数都写类型注解**，分析器据此给出精确的范围与空值判断：

```annota
Item(name:String, price:int, tags:List)=( ... )

Order(id:int, items:List, note = null)=( ... )
```

* 变量尽量在**使用处附近**声明并立刻初始化；`new x` 之后再赋值会让 `uninitialized` 有机会
  咬你。
* 默认参数用于"常见情况"，不要用 `null` 表示多种含义。

## 5. 标注：把规格写给分析器

这是 Annota 的核心，也是最能省时间的地方。

```annota
[[require: divisor != 0]]
[[ensure: result * divisor == dividend]]
divide(dividend, divisor) = dividend / divisor
```

* **边界函数写契约**：`[[require]]` 写调用者必须满足的，`[[ensure]]` 写你能保证的。
  有了它们，分析器会在**每个调用点**帮你检查，也会在函数出口验证。
* **循环写不变量与终止度量**：`[[invariant]]` 帮助分析器缩小范围，`[[decrease]]` 证明终止。

```annota
[[invariant: 0 <= i && i <= n]]
[[decrease: n - i]]
sum_to(n)(
    new i = 0
    new s = 0
    while i < n(
        s = s + i
        i = i + 1
    )
    =s
)
```

* **纯函数标 `[[pure]]`**：它同时是文档和检查（禁止写全局/字段、禁止调用非纯函数），
  并行代码里尤其重要。
* **不可达分支用 `[[unreachable]]`**，确实有意的越界/除零用 `[[ignore: code, "原因"]]`。
  **必须写原因**；抑制不再匹配任何诊断时，`annota ide suppressions` 会提醒你删掉它。
* 不要用 `[[trusted]]` 绕过分析，除非是 FFI 边界，并在旁边写清为什么安全。
* 认为分析器结论不够精确时，先想"能不能补一条 `[[assume]]`/`[[assert]]` 把事实说清楚"，
  再考虑抑制。

## 6. 错误处理

* 可预期的失败**返回值**（`Ok`/`Err` 或 `null` + 检查），不可恢复的违反才 `throw`。
* `throw` 只抛**字符串或描述性对象**，别抛裸数字：

```annota
parse_port(text)(
    new n = int(text)
    if n <= 0 || n > 65535( throw "bad port: " + text )
    =n
)
```

* 调用点用 `except e( ... )` 接住并**补上下文**再决定是否继续抛。

```annota
load_config(path)(
    new text = File.read(path)
    =json.parse(text)
    except e( throw "cannot load " + path + ": " + e )
)
```

* 不要用异常做控制流；也不要空 `except` 吞掉错误。

## 7. 类与数据

* 类表达**有行为的状态**；纯数据用 `Dict`/元组，别为了"看起来面向对象"造类。
* 构造参数写全，`__init__` 只做**赋值与校验**，不要在里面做 I/O。

```annota
Account(owner:String, balance:int = 0)=(
    owner:String
    balance:int

    __init__()(
        if balance < 0( throw "balance must be >= 0" )
    )

    deposit(amount:int)(
        [[require: amount > 0]]
        balance = balance + amount
        =balance
    )
)
```

* 需要打印/长度/迭代时实现魔法方法（`__str__` / `__len__` / `__iter__`），而不是暴露 `to_str()`。
* 继承只用于真正的"is-a"，`[[private]]` 是默认，不要随意 `[[public]]`。

## 8. 标准库优先

| 需求 | 用什么 |
|---|---|
| 集合操作 | `Seq`（`map`/`filter`/`fold`/`sort`…）而不是手写 `while` |
| 字符串 | `Str` 与内建方法（`trim`/`split`/`starts_with`…） |
| 键值对 | `Dict` |
| 文件与路径 | `File` / `Dir` / `Path`，不要碰 `_file_*` |
| 时间 | `Time` / `Stopwatch`；`time.millis()` 用于打点 |
| 环境与外部命令 | `Os`（`env`/`run`/`which`） |
| 并行 | `Thread.parallel_map`，工作函数保持纯 |
| 网络 | `Net` / `Tcp`（明文 HTTP；https 需自带 TLS） |
| 测试 | `Test`（见 §10） |

需要新能力时，**先在 `lib/*.mod` 里用 Annota 写**，只有必须碰操作系统时才加一个
`_sys_*` 原语（见 CONTRIBUTING）。

## 9. 切片、续行与其它习惯

* `xs[i:j]` 是**编译期宏**，记得先 `use slice`；字符串切片返回字符串。
* 整数相除用 `/` 就是向零截断（C 语义）；要小数就写 `float(a) / float(b)` 或除以浮点字面量，别依赖隐式提升。
* 字符串按**字节**索引，处理中文时优先用 `Str`/`Str.lines()`，不要手写字节循环。
* 可变参数用 `*args`，不要用"传一个列表"来冒充可选参数。

## 10. 测试与验证

每个模块带一个测试函数，用 `Test` 断言，能被 `annota <file>` 直接跑：

```annota
Test.suite("order")

Test.eq(total_of([]), 0, "空列表")
Test.eq(total_of([1, 2, 3]), 6, "求和")
Test.raises("bad", parse_port("abc"))

Test.check()
```

* 断言消息写**期望的行为**，不要写 "test1"。
* 修 bug 先补一条会失败的断言，再改代码（`examples/analysis/` 下的 fixture 同理）。
* 提交前跑：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify      # 全量
build\annota.exe analyze 你的文件.ant                            # 只分析，要求 0 error 0 warning
build\annota.exe analyze --json 你的文件.ant                     # CI / 编辑器集成用
```

## 11. 提交前检查清单

- [ ] `annota analyze` 对你的文件报告 **0 error / 0 warning**（info 也要看一眼）
- [ ] 公共函数有 `[[require]]` / `[[ensure]]`，循环有 `[[invariant]]`
- [ ] 没有未使用的变量、参数、抑制标注（`annota ide suppressions`）
- [ ] 新代码有断言覆盖；修 bug 有回归断言
- [ ] 没直接用 `_` 前缀原语（除非你在写 `lib/`）
- [ ] `annota <file>` 能跑通，输出符合预期
- [ ] 文档/注释与行为一致（尤其是契约：改了实现就改 `[[ensure]]`）
