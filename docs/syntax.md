# Annota 语法规范

本文件是 Annota 的语法参考：词法、语句、表达式、类型与预定义结构。工具链实现与本文不一致时，
以本文为准并视为 bug。标注、诊断码与底层原语的完整清单见
[reference.md](reference.md)（由 `annota ide docs` 生成）。

想在浏览器里对照代码阅读，可先看 [style.md](style.md)（代码规范）。

---

## 1. 源文件

* 编码 **UTF-8**（允许 BOM）。换行 `\n`，`\r` 被忽略。
* 一个文件就是一个**脚本**：顶层语句按书写顺序执行，**没有 `main` 函数**。声明必须先于使用
  （函数/类体内的引用在调用时才解析，因此递归与相互调用是允许的）。
* 文件可以 `use` 其它模块（见 §9）。

### 1.1 注释与分段

| 形式 | 含义 |
|---|---|
| `-- 到行尾` | 行注释 |
| `-[ ... ]-` | 块注释 / 文件头说明（可跨行） |
| `—分段 名称` | 行首的特殊标记，仅作视觉分段，等价于注释 |

```annota
-[ 文件级说明，通常放在最前面 ]-
-- 行注释

—分段 工具函数
```

### 1.2 续行与语句分隔

* 语句以**换行**结束。`;` **不是**分隔符（会报语法错误）。
* 同一行写多条语句用**逗号**：`a = 1, b = 2`（也支持 `new a, b`、`del a, b`）。
* 行尾是**二元运算符**（`+ - * / % ** & | ^ << >> && || == != < > <= >=`）、`=` 或 `.` 时，
  自动续到下一行；`\` 是显式续行符。`,` 结尾不自动续行（否则 `new a, b` 会有歧义）。

```annota
new total = 1 +
            2 +
            3              -- 6

new other = 1 + \
            2              -- 3
```

**必须同行的一处**：`else` 要紧跟 `if` 体结尾的 `)`。

```annota
if n > 0( print "positive" ) else ( print "other" )   -- ✓
if n > 0(
    print "positive"
) else (                                              -- ✓ `) else (` 在同一行
    print "other"
)
```

---

## 2. 词法

### 2.1 关键字

```
new  del  const  macro  use  view  state
if  else  for  while  in  break  continue
throw  except  print  input
true  false  null  this  super
```

`to` 与 `step` 是**软关键字**：只在 `a to b [step c]` 这个位置有特殊含义，因此可以当普通
变量名/参数名使用（`File.copy(from, to)`）。

### 2.2 字面量

| 字面量 | 例 |
|---|---|
| 整数 | `42`、`-7`、`0xFF`、`0b1010`、`1_000_000` |
| 浮点 | `3.14`、`1e-9` |
| 字符串 | `"hello"`，转义 `\n \t \r \0 \\ \" \'`（未识别的转义原样保留） |
| 布尔 / 空 | `true`、`false`、`null` |
| 列表 | `[1, 2, 3]`、`[]` |
| 元组 | `(1, 2)`（逗号分隔，见 §5.6） |
| 字节串 | `Bytes(...)`、`_file_read_bytes(...)` |
| 颜色 | `Color(...)` |

> 字符串是**字节串**：`len("世界")` 为 6，索引按字节。

### 2.3 命名

标识符由字母、数字、`_` 组成，不以数字开头；可含非 ASCII 字符（UTF-8）。约定见
[style.md](style.md)：`PascalCase` 用于类/组件，`snake_case` 用于函数与变量，`UPPER_SNAKE`
用于常量，`_` 前缀表示底层原语（用户代码不直接用，改用 `lib/` 封装）。

---

## 3. 运算符与优先级

从低到高：

| 级别 | 运算符 | 结合性 |
|---|---|---|
| 1 | `\|\|` | 左 |
| 2 | `&&` | 左 |
| 3 | `\|` | 左 |
| 4 | `^` | 左 |
| 5 | `&` | 左 |
| 6 | `==` `!=` | 左 |
| 7 | `<` `>` `<=` `>=` | 左 |
| 8 | `<<` `>>` | 左 |
| 9 | `+` `-` | 左 |
| 10 | `*` `/` `%` | 左 |
| 11 | `**` | 右 |
| 12 | 前缀 `-` `!`（`not`）`~`，后缀 `(...)` `[...]` `.name` | — |

要点：

* **`/` 是浮点除法**：整除时结果为 `int`，否则为 `float`（`6 / 3` → `2`，`7 / 2` → `3.5`）。
  需要整数除法用 `int(a / b)`，取余用 `%`。
* `&&` / `||` 短路求值。
* `a to b`（可带 `step c`）构造**迭代器**，只能用于 `for`（见 §4.4）。
* 复合赋值：`+= -= *= /= %= **= &= |= ^= <<= >>=`。

---

## 4. 语句

### 4.1 声明与赋值

```annota
new a = 1                  -- 声明并初始化
new b, c                   -- 一次声明多个（未初始化，读取会被分析器报告）
new x = 1, y = 2, z = 3    -- 各自带初值
const LIMIT = 100          -- 常量：不能再赋值，也不能 del
a = 2, b = 3               -- 同行多条赋值
state count = 0            -- GUI 响应式状态（view 内）
del a, b                   -- 删除（变量回到未初始化）
```

`new` 不写初值时，字段类型决定默认值（`int`→0、`float`→0.0、`bool`→false、`List`→`[]`、
其它→`null`）。

### 4.2 表达式语句

单独一行写表达式是合法的，结果被丢弃：

```annota
noise + "y"        -- 允许，但不产生作用（分析器可能提示）
touch()            -- 常见用法：调用一个只有副作用的函数
```

### 4.3 条件

```annota
if cond( ... )
if cond( ... ) else ( ... )
```

`if` 体是**括号块**，不是缩进块。条件中可用 `&&`、`||`、比较；`if`/`else` 可以嵌套。

### 4.4 循环

```annota
while i < n(
    i = i + 1
)

for x in xs( print x )              -- 列表 / 元组 / 字符串 / 字典 / 迭代器
for i in 0 to 9( print i )          -- 0..9（含 9）
for i in 0 to 9 step 2( print i )   -- 步长
for k, v in pairs(d)( print k, v )  -- 解构
```

`break` 提前退出，`continue` 进入下一轮。循环上可写 `[[invariant]]` 与 `[[decrease]]`。

### 4.5 返回、抛出、异常

```annota
add(a, b)( =a + b )          -- `= expr` 是 return

safe(x)(
    if x < 0( throw "negative" )
    =x
)

try_it()                     -- 可能抛异常
except e( print "caught: " + e )
```

`throw` 的参数会被包装成异常对象；`except` 必须紧跟在块之后（`except-placement` 检查）。

### 4.6 输入输出

```annota
print a, b, c                -- 空格分隔，自动换行
input name                   -- 读一行到 name（GUI 里弹对话框，见 README FAQ）
```

### 4.7 裸作用域

```annota
(
    new tmp = 1
    print tmp
)                            -- tmp 出了作用域即结束（读取会被 `scope` 检查报告）
```

---

## 5. 表达式

### 5.1 调用与具名参数

```annota
f(1, 2)
f(1, key: 2)                 -- 具名参数
obj.method(1)
obj.field
xs[0], xs[i + 1]
```

### 5.2 函数定义

```annota
add(a, b) = a + b                    -- 单表达式（自动返回）
add(a, b)( =a + b )                  -- 块体
greet(name = "world") = "hi " + name -- 默认参数
total(*nums) = len(nums)             -- 可变参数（*args 与 ...args 等价）
```

可变参数收集成 `List`；**不支持 `**kwargs`**，需要键值对请显式传 `Dict`。
Lambda：`(x)( =x * 2 )`，可作为值传递。

### 5.3 闭包

函数捕获外层变量**按引用共享**（不是拷贝），因此：

```annota
counter()(
    new n = 0
    bump()(
        n = n + 1
        =n
    )
    =bump                 -- 返回的闭包与 n 共享同一个单元
)
```

需要各自独立的状态时，用类或每次显式拷贝一份。

### 5.4 分段表达式

```annota
new label = {
    score >= 90: "A"
    score >= 60: "B"
    else: "C"
}
```

分支按顺序判断；`else` 必需；分支之间用换行分隔。

### 5.5 转换与类型

```annota
int(x), float(x), str(x), bool(x)
typeof(x)                    -- "Int" / "Float" / "String" / "List" / "Dict" ...
len(x), is_null(x)
```

### 5.6 元组与解构

```annota
new pair = (1, "a")
new (a, b) = pair            -- 解构声明：括号里写名字
for k, v in pairs(d)( ... )  -- 循环变量同样可解构
```

### 5.7 切片（需 `use slice`）

```annota
use slice
xs[1:3]        -- 列表 → 只读 SliceView（.to_list() / len / 遍历）
xs[2:]  xs[:2]  xs[:]
text[2:5]      -- 字符串 → 字符串（"cde"）
```

切片是**编译期宏**展开，不是内建语法；没有 `use slice` 时 `xs[1:3]` 不可用。

---

## 6. 类

```annota
Point(x, y)=(
    X:int                     -- 声明字段（类型注解可选）
    Y:int
    tag = "p"                 -- 带默认值的字段

    __init__()(               -- 构造函数体：空括号，构造时执行
        X = x                 -- 读构造参数
        Y = y
    )

    norm2() = X * X + Y * Y   -- 方法
    [[static]]
    origin() = "0,0"          -- 静态方法：不绑定实例

    __str__() = "Point(" + str(X) + ", " + str(Y) + ")"
)

new p = Point(3, 4)
print p.norm2()
print Point.origin()
```

要点：

* 类名后的括号是**构造参数**；参数会自动成为同名字段（除非已声明同名字段）。
* `__init__` 必须**空括号**，在字段默认值之后执行，可覆盖默认值；给它参数是编译错误。
* 字段默认值在构造时求值；`X:int` 形式的字段默认 0。
* 魔法方法：`__str__` `__len__` `__iter__` `__call__` `__getitem__` 等，由运行期按名字调用。
* 继承：类体首行写 `:Base(args)`。

```annota
Dog(name)=(
    :Animal(name)             -- 调用基类构造
    speak() = "woof"
)
```

* 可见性标注：`[[private]]`（默认）、`[[public]]`、`[[static]]`、`[[expose]]`。

## 7. 视图与状态（GUI）

```annota
view Counter(title="Counter", width=400, height=300)(
    state count = 0

    inc()(
        count = count + 1
    )

    Column(pad=16)(
        Text(text="Count: " + str(count))
        Button(label="+1", click=inc)
    )
)
```

`view` 是特殊的类：外层 `state` 变化会触发界面重建；组件（`Column` / `Row` / `Text` /
`Button` / …）是内建构件。`annota <file> --gui` 打开窗口，IDE 里按 `F7` 预览。

## 8. 标注

标注是**规范**，放在它描述的单元之前：

```annota
[[module: math]]                 -- 元信息
[[version: 1.0]]
[[require: d != 0]]              -- 契约
[[ensure: result * d == n]]
[[invariant: i <= n]]            -- 循环
[[decrease: n - i]]
[[pure]]                         -- 无副作用
[[ignore: division-by-zero, "此处除零是刻意的"]]   -- 抑制一个检查
```

写在文件级（前后空行分隔）表示整个文件；写在函数/类/循环/语句前表示该单元。完整列表与含义见
[reference.md](reference.md) 第 1 节。

## 9. 模块

```annota
use std                          -- 标准库全部（seq/str/dict/file/time/os/thread/net/test…）
use seq                          -- 单个模块
use "自定义路径/my.mod"           -- 相对路径
```

`use` 在**编译期**把模块内联进来，因此模块里的顶层代码会在此处执行一次。
`lib/*.mod` 里的模块用 `[[module: name]]` 声明自己的名字。

## 10. 宏

```annota
macro twice($x)(
    $x + $x                      -- $x 是占位符，匹配调用处的任意片段
)
print twice(3)                   -- 展开成 3 + 3

macro $x[$a:$b](                 -- 模式宏：匹配 xs[1:3] 这样的下标写法
    Slice.make($x, $a, $b)
)
```

`macro` 后的整段 token 是**模式**，`$名字` 是占位符；括号里是展开后的代码。
宏在编译期展开，参数是**语法片段**而非值。展开深度上限由 `[[macro_depth: N]]` 控制
（默认见实现），超过会报 `Macro expansion depth exceeded`。实参在宏体里出现多次时，
分析器会用 `macro-side-effect` 提醒可能的重复求值。

## 11. 与文档取舍的地方

以下是实现中明确选择的语义，写代码时按这些来：

1. **没有 `main`**：顶层即入口。
2. **括号块而非缩进块**：`if c( ... )`、`f(a)( ... )`。
3. **`else` 必须与 `)` 同行**。
4. **`/` 可能返回浮点**；整数除法写 `int(a / b)`。
5. **字符串按字节索引**，长度是字节数。
6. **`substr(start, stop)` 的第二个参数是结束下标**（Python 风格），不是长度。
7. **语句不能跨行**，除 §1.2 的三种续行方式。
8. **闭包按引用捕获**，工作线程里要注意共享状态（见 README「原语」一节）。
9. **`**kwargs` 不支持**，用 `Dict`。
10. **切片依赖 `use slice`**（宏实现）。
