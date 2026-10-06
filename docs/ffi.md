# 链接 C++（FFI）

Annota 的核心很小，但这不意味着新能力都要写进核心。**任何 C++ 文件都可以注册原生函数和模块**，
Annota 这边只是普通的 `use` + 成员调用——语法核心一行都不用改。

这是让标准库变快的推荐路线：热内核用 C++ 写，链接进来，脚本层保持可读。

## 1. 最快的方式：`native/` 目录

`build.ps1` 会自动编译 `native/*.cpp` 并链接进 `annota`：

```powershell
# native/fast.cpp
#include "../src/ffi.hpp"

ANNOTA_MODULE(fast)
    mod.value("version", Value::str("1.0"));

    using ValueList = ValueList;      // 别名，免得尖括号被当成 Markdown 链接

    mod.fn("sum", [](VM& vm, ValueList& a) {
        ValueList xs = ffiList(vm, ffiArg(a, 0), "fast.sum");
        int64_t total = 0;
        for (auto& x : xs) total += ffiInt(vm, x, "fast.sum");
        return Value::integer(total);
    });
ANNOTA_END_MODULE
```

```annota
use fast
print fast.version            -- 1.0
print fast.sum([1, 2, 3])     -- 6
```

注册发生在静态初始化期，登记表本身是函数内静态对象，所以初始化顺序不会出问题。
`use fast` 能解析、`fast.sum(...)` 能调用，是因为登记表告诉核心"这个模块存在"。

## 2. 插件方式：不重新编译解释器

一条命令把 C++ 文件变成插件，然后程序里 `use` 它就行——**不需要重新构建解释器**：

```powershell
annota plugin build plugins/hello.cpp      # -> build/plugins/hello.dll
annota run examples/plugin.ant             # `use hello` 自动加载它
```

* 插件默认写到 `<exe 目录>/plugins/`，这个目录会被自动搜索，所以 `use hello` 直接可用；
* 也可以放别处：`annota run x.ant --plugin ./hello.dll`，或设 `ANNOTA_PLUGIN=hello.dll;other.dll`；
* 构建器会自己找编译器（`CXX` → 解释器构建时记录的编译器 → `g++`/`clang++`），并自动带上
  `src/ffi.hpp` 的包含路径、链接解释器的导入库；
* Linux/macOS 同样是一句 `annota plugin build`（生成 `lib<name>.so`）；
* 静态分析器也会加载同一个插件，所以 `use hello` 在 IDE 里不会报"未定义"。

它替你拼出的就是下面这条命令（想手工编译、或者把它写进自己的 CMake 时照抄即可）：

```powershell
# Windows / MinGW：注意最后那句导入库，插件要链接回解释器
g++ -std=c++17 -O2 -shared -I src plugins\hello.cpp -o build\plugins\hello.dll `
    -static-libgcc -static-libstdc++ build\libannota.dll.a

# Linux / macOS
g++ -std=c++17 -O2 -shared -fPIC -I src plugins/hello.cpp -o build/plugins/libhello.so

annota run examples/plugin.ant                       # 或者用 --plugin 指定路径
$env:ANNOTA_PLUGIN = "a.dll;b.dll"                   # 多个用 ; 或 : 分隔
```

插件从静态初始化器注册即可；如果需要显式入口，导出一个
`extern "C" void annota_plugin_init()`，加载后会被调用。
工作流与排错见 [使用手册](tools.md) §6。

## 3. 接口一览

| 用途 | 接口 |
| --- | --- |
| 注册整模块 | `ANNOTA_MODULE(name) { ... } ANNOTA_END_MODULE` |
| 注册全局函数 | `ANNOTA_FUNCTION(name, fn)` / `ffiFunction(name, fn)` |
| 模块成员 | `mod.fn("member", fn)`、`mod.value("member", v)` |
| 取参数 | `ffiArg(args, i)`（缺失返回 null） |
| 参数转列表 | `ffiList(vm, v, "谁在抱怨")`（复制一份，安全） |
| 参数转序列 | `ffiItems(vm, v)`（列表/元组/字符串/迭代器/模块都行） |
| 参数转数字 | `ffiInt(vm, v, what)` / `ffiFloat(vm, v, what)` |
| 返回值 | `Value::integer/real/boolean/str/list/tuple/null(...)` |
| 报错 | `vm.throwError("消息")`（带 Annota 栈回溯） |
| 回调 Annota | `vm.callSync(fn, {参数...})`，真值判断用 `vm.truthy(r)` |

原生函数签名是 `Value(VM&, ValueList&)`，其中 `ValueList` 是 `std::vector` 的别名（元素类型
`Value`）：与核心内置函数完全一致，所以一旦某段逻辑值得下沉到 C++，不需要发明新语法。
参数按值语义传入的是**副本**，直接遍历、排序都不会影响调用者。

## 4. 双向通信

C++ 可以反过来调用 Annota 闭包，可以用来做回调、比较器、以及从 C++ 侧计时：

```annota
Test.eq(fast.count_if([1,2,3,4,5], (x)( =x % 2 == 1 )), 3)
Test.eq(fast.map_call([1,2,3], (x)( =x * x )), [1, 4, 9])
Test.eq(fast.reduce_call([1,2,3], (a, b)( =a + b ), 0), 6)
```

`fast.time_call(f, iters)` 是 C++ 里的单调时钟，用来测一段 Annota 代码的真实耗时。

## 5. 性能模型

一次原生调用大约 0.1–0.3 微秒（一次调用 + 参数转换），所以：

- **值得**把整个热循环/内核搬进 C++（每次调用摊掉成千上万次迭代）；
- **不值得**把单次小操作包成原生函数（调用开销会盖过收益）；
- 纯整数循环如果不想离开 Annota，用 [`[[jit]]`](jit.md) 让解释器把它翻成机器码。

`examples/ffi.ant` 里的实测（同一台机器）：

| 工作 | 脚本 | 链接 C++ |
| --- | --- | --- |
| 求和 20 万个整数 | 41 ms | 3 ms |
| 10 万以内素数个数（筛法） | 125 ms | < 1 ms |

标准库已经这样做了：排序、`nth`、`argsort`、`lower_bound` / `upper_bound` 这些热原语都在 C++ 侧
（`native/seq_native.cpp`，注册成 `seqnative` 模块），`lib/seq.mod` 只做薄封装；再往下如果还需要
新算法，按本文档加一个 `native/*.cpp` 即可，不必动核心。

## 6. 相关文档

- [使用手册](tools.md)（`annota plugin build`、插件目录、环境变量、排错）
- [语法参考](syntax.md)（模块一节说明了 `use` 如何找到插件）
- [机器码与 `[[jit]]`](jit.md)
- [标准库参考](stdlib.md)
