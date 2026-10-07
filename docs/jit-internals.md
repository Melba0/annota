# JIT 后端：现状、设计债与改造规格

这份文档记录机器码后端（`src/jit.hpp`）的当前约定、已知的设计债，以及两件已知未做的工作的
**可执行规格**。目的是让"为什么这么写"和"下一步具体改哪里"都有据可查，而不是靠记忆。

## 1. 当前约定（必须遵守的不变量）

| 项 | 约定 |
|---|---|
| 函数 ABI | `int64_t (*)(const int64_t* L, void* out)`；`L` 是局部量数组，`out` 是 `JitOut` |
| `JitOut` | `{ int64_t value; int64_t kind; }`；`kind`: 0 int / 1 int64 / 2 bool / 3 null / 4 运行时错误 |
| 局部量 | **直接读写 VM 的 `int64_t L[64]`**（`kBase` = rcx/rdi 指向它），与解释器共享内存 |
| 虚拟栈 | `[rsp + vr*8]`，每槽 8 字节 |
| 槽内取值 | **规范化 64 位**：值精确等于其数学值（按种类符号/零扩展） |
| 种类格 | `JitKindSet`（`uint16`，位 = `NumKind`），`kJitBool = 1<<15`，`kJitAllInts` = 任意整数宽度 |
| 逐槽种类 | `JitCode::slotKind[i]` = `NumKind+1`（0 = 不关心）；入口按此精确校验（I64 额外接受无类型） |
| 值宽度 | 支持 `None/I8/I16/I32/I64/U8/U16/U32/U64`；**128 位与浮点不进入后端** |
| 数组（实验） | 仅函数内自建、不逃逸的 1 维整型数组；缓冲区来自 arena（VM 在最外层原生调用后释放） |

## 2. 已修的历史坑（都曾是"静默错误"）

* kind 字节与虚拟栈槽位曾用 **8 位位移**寻址 → 偏移 ≥128 被截断成负偏移，写进调用者帧
  （表现是"结果对但某一行慢 5 倍"）。
* `OP_LOOP` 热点计量漏判 `jitTried` → 不可翻译的大循环每轮回跳都重跑一遍分析/发射。
* 函数体恒有返回时，末尾死的 `return_null` 污染 `retByte` → 所有直接调用被拒。
* 比较指令**无视符号性**（永远 `setl/setg/...`）→ 无符号比较在 JIT 接管后出错
  （`2^64-1 < 1` 报 6000/10000 命中）。
* `OP_CONST` 不被支持 → 只要用到超出 `int1` 立即数的字面量（模数、掩码）整块不可翻译。
* `new x:int64 = 1` 的"无类型 → 声明宽度"转换被判成非恒等 → 常见写法不可翻译。
* 直接调用时 `kBase`/`kOut` 被调用破坏、callee 的 `L[]` 起点算错。

## 3. 设计债一：值宽度

**现状**：槽里存规范化 64 位，所以每个算术运算后补一条收窄指令（`wrapRax`）。
`int32 * int32` 因此是"64 位 `imul` + `movsxd`"两条，而 C++ 只要一条 `imull`。

**为什么不能直接改成窄指令**：局部量直接读写 VM 的 `L[]`，解释器会看到这些字节；一旦槽里允许
"只有低 32 位有意义"，`L[]` 就不再是解释器期望的精确 int64。

**已完成的部分**：当提升后的种类是 **`U32`** 时改用 32 位形式（`add eax, r8d` / `sub eax, r8d` /
`imul eax, r8d`，以及融合指令的 `mov eax,[L+a*8]` + `add eax,[L+b*8]`）：写 32 位寄存器会清零高 32 位，
**正好就是 `U32` 的规范化形式**，收窄指令自然消失（语义完全等价，实测 35× 于解释器）。

**规格（Step 1）**：

1. **私有局部帧**：开场 `sub rsp, frameBytes` 之后再留 `numLocals*8`，把 `L[0..n)` 拷进来，
   然后**把 `kBase` 指向这块私有区**。这样槽内表示不再受解释器约束（返回路径不需要回拷：
   解释器只经 `JitOut` 取结果；`OSR`/直接调用的实参仍走 `L[]` 拷贝）。
2. 定义新不变量：**每个槽存"该种类宽度的低 N 位"**，高位的意义由种类决定；只有以下场合需要扩展：
   * 运算的**提升宽度**比操作数宽（先 `movsx/movzx` 到提升宽度再算）；
   * 比较的两个操作数宽度不同（在提升宽度上比较）；
   * `OP_CONVERT`、数组元素存/取、返回值、原生调用的实参、`OP_NEW_ARRAY` 的长度。
3. 逐指令表（`src/jit.hpp` 发射器）：

   | 指令 | 需要改什么 |
   |---|---|
   | `OP_ADD/SUB/MUL` | 在提升宽度上算；宽度 == 操作数宽度时免扩展（`U32`/`I32` 已覆盖一半） |
   | `OP_LOCAL_ADD_LOCAL` / `_ADD_IMM` / `_SUB_IMM` | 同上（`U32` 已做，其余按提升宽度） |
   | `OP_EQ..GE` | 在提升宽度上 `cmp` + 对应 `setcc`（符号性已修，宽度待做） |
   | `OP_JUMP_IF_NOT_LT_*` | 同上（融合分支） |
   | `OP_RETURN` / 直接调用实参 | 扩展成精确值 |
   | `OP_CONVERT` | 已是"窄宽度回绕"，补宽度扩展 |
   | `OP_GET/SET_INDEX` | 按元素宽度扩展/回绕（已做） |

4. 测试矩阵：每个整数种类 × {加/减/乘/比较/复合赋值} × {JIT 开, 关} 结果必须逐位一致，
   并覆盖边界值（该宽度的 0、最大值、最小值、回绕点）。

## 4. 设计债二：128 位（`longlong` / `ulonglong`）

**指令层面没有障碍**，与 C++ 编 `__int128` 完全同构：

| 运算 | 指令序列 |
|---|---|
| 加 / 减 | `add lo,lo'` + `adc hi,hi'` / `sub` + `sbb` |
| 左移一位（`a+=a`） | `shld hi,lo,1` + `shl lo,1` |
| 比较 | 先 `cmp hi`，相等再 `cmp lo`（`sbb`/`setcc` 组合） |
| 64×64→128 | `mul`（结果 `rdx:rax`） |
| 128×128 | 三到四次 `mul` 组合 |
| 除 / 取模 | 调 `__udivti3` / `__umodti3`（和现在给数组分配器调 C 函数用的是同一套 `callC`） |

**真正的成本在边界**，逐项列出：

| 卡点 | 现状 | 需要 |
|---|---|---|
| 局部量 | `int64_t L[64]`，逐槽 64 位 | 宽值占**两个连续槽** |
| 结果通道 | `JitOut{int64_t value; int64_t kind;}` | 加宽（或增加一个 hi 字段） |
| 种类格 | `uint16` 位集，无 128 位成员 | 加两个种类 +"宽值跨两槽"规则 |
| 逐槽元数据 | `slotKind` 一字节/槽、`readSlots` | 全套遵守宽值规则 |
| 入口检查 | `jitLocalsOk` 只看 `VT::Int` 的 `i` | 要能表达"装箱宽整数"（解释器里 128 位是装箱的 `Obj`） |
| 进出边界 | 逐槽 `L[k] = fr.locals[k]->i` | 拆箱/装箱、成对打包 |
| 直接调用实参 | 逐槽 8 字节 | 成对传递 |
| OSR 蹦床 | 逐槽拷贝 | 同上 |
| 数组元素 | 8 字节裸 int64 | 16 字节元素或另开元素类型 |
| `[[jit]]` 类型提示 | 已放开到 `uint8..uint64` | 再加 `longlong` / `ulonglong` |

**建议顺序**：先做第 3 节（私有帧 + 宽度表示），因为"宽值跨槽"依赖同一套槽位规则；
再把 128 位作为"两个槽 + 两个新种类"接上去。

## 5. 除法与取模：用 C 辅助函数，不要手写 `idiv` 分支

`OP_DIV` / `OP_MOD` 目前完全不在后端里（`fastIntBinary` 不处理它们，走 `binaryResult`），
所以带 `%` 的函数**整块**退回解释器——`examples/jit.ant` 里的 `sum_even_jit` 就是这样。

手写 `idiv` 序列要处理三件事，每一件都容易出错（我第一次尝试就是崩在这里）：

1. 除数为零：解释器抛可捕获错误，而硬件触发 `#DE` 故障；
2. `INT_MIN / -1`：`idiv` 溢出故障，而解释器的环绕语义给出 `-dividend`、余数 0；
3. `%` 的符号约定必须与解释器（`binaryResult`）逐位一致。

**更稳的做法**：把语义留在 C++ 里，机器码只调一次现成的 `callC`（数组分配器已经在用这套）：

```
// vm.cpp
int64_t annotaJitDiv(int64_t a, int64_t b);   // 与解释器 binaryResult 完全一致的语义
int64_t annotaJitMod(int64_t a, int64_t b);   // 除零时置位 jitDivByZero 并返回 0
```

* 发射侧：`loadVrToRax(d-2)` → `mov rcx, rax`；`loadVrToRax(d-1)` → `mov rdx, rax`（第二参数），
  然后 `callC(&annotaJitDiv, ...)`，结果是 `rax`。宽度规则沿用第 3 节：
  32 位种类直接存 `eax`（写 32 位寄存器即得原始值），其他种类按提升宽度 `wrapRax`。
* 除零：C 辅助函数置位，VM 在两个原生入口（`runNativeIfReady`、OSR）检查该标志后抛
  `除数为零`，与 `JitOut.kind == 4` 同一条通道。
* 这样就不需要在机器码里生成 `test/jz/cmp -1/cqo/idiv` 和两个短路补丁——那三处正是崩溃的来源。

代价是每次除法多一次调用（`idiv` 本身也要 20–40 周期），换来的是语义由一处 C++ 代码定义，
并且 `%` 密集的代码从此可以进机器码。

### 5.1 解释器的精确语义（`vm.cpp:2136` 起，已核对）

```cpp
bool intMode = (a.t == VT::Int && b.t == VT::Int);
NumKind kr = promoteNum(a.numKind(), b.numKind());     // 结果种类
// OP_MOD:  b.i == 0 -> throwError("modulo by zero");  否则 Value::typedInt(a.i % b.i, kr)
// OP_DIV:  同上，除数为零时报 "division by zero"（见同一条分支）
```

即：**C 式截断**（不是向下取整）✓；结果按 `promoteNum` 的种类回绕 ✓；除零抛可捕获错误 ✓。
注意 `INT64_MIN / -1` 在解释器里是 C++ 未定义行为，所以 JIT 侧**显式定义**它更安全：

```cpp
// vm.cpp（与数组分配器同一套：机器码用 callC 调用，VM 在原生入口检查标志）
namespace { int gJitDivErr = 0; }                       // 1 = 除零, 2 = 取模零
int64_t annotaJitDiv(int64_t a, int64_t b) {
    if (b == 0) { gJitDivErr = 1; return 0; }
    if (b == -1) return (int64_t)(0 - (uint64_t)a);      // 环绕语义，避免 UB
    return a / b;
}
int64_t annotaJitMod(int64_t a, int64_t b) {
    if (b == 0) { gJitDivErr = 2; return 0; }
    if (b == -1) return 0;
    return a % b;
}
```

VM 两个原生入口（`runNativeIfReady`、OSR）在现有 `JitOut.kind == 4` 检查旁加：

```cpp
if (gJitDivErr) { int e = gJitDivErr; gJitDivErr = 0;
                  throwError(e == 1 ? "division by zero" : "modulo by zero"); }
```

发射侧（`jit.hpp`）：解码把 `OP_DIV`/`OP_MOD` 当作无操作数指令；分析把它们并入
`OP_ADD/SUB/MUL` 那条（弹 2、推 `jitPromoteSet`）；发射是

```
loadVrToRax(d-2) -> mov rcx, rax          // 被除数
loadVrToRax(d-1) -> mov rdx, rax          // 除数
callC(&annotaJitDiv 或 &annotaJitMod, ...) // 结果在 rax；kBase/kOut 由 callC 保存恢复
32 位种类直接存 eax（写 32 位寄存器即得原始值）；其他种类 wrapRax(提升种类)
```

三处改动（两个 C 函数 + 一个标志检查 + 一段发射），不再有任何手写的 `test/jz/cmp -1/cqo/idiv`
与短跳转补丁——那是上一次崩溃的来源。



## 6. 待做：全局变量与区间迭代

这两条是用户侧影响最大的缺口：真实程序的状态几乎都在全局量上，而 `for i in a to b` 是最常用的
循环写法。两条都会让**整个函数**退回解释器。

### 6.1 全局变量

现在 `OP_GET_GLOBAL` / `OP_SET_GLOBAL` 不在后端里。两种做法：

**方案 A（推荐，长期正确）：全局量改成槽位数组。** VM 里 `globals` 从 `std::map<std::string, Cell>`
变成 `std::vector<Cell> slots` 加一份"名字 → 槽位"的映射，编译器把全局名字常量换成槽位号。
之后机器码读全局量与读局部量同构：

```
mov rax, [globals + slot*8]      ; Cell（shared_ptr<Value>）的裸指针
mov rax, [rax]                   ; Value::i
```

好处是解释器的全局访问也一起变快；代价是 VM 里所有 `globals[...]` 访问点、模块内联后的全局重命名、
以及任何序列化/调试输出都要跟着改。

**方案 B（改动小，先落地）：把全局表指针作为第三个参数传给机器码。** Windows ABI 的第三个整数参数
在 `r8`，而 `r8` 目前是发射器的暂存寄存器，所以必须把它放进帧内私有槽，像 `kBase` / `kOut` 那样在
调用前后保存恢复（`callC` 已经是这个模式）。需要同时改三处传递：`JitFn` 签名、
JIT→JIT 的直接调用、OSR 蹦床。

两种方案共同的前置条件：

* 全局量的**种类**必须静态可知（该全局在整个程序里只被赋同一种 64 位以内的整数值），否则退回解释器；
* 入口检查要覆盖全局量——现在 `jitLocalsOk` / `readSlots` 只看局部量，全局量的 Cell 可能在运行时
  是别的类型，必须在原生代码执行前验证；
* 未初始化或类型不符要走错误通道（`JitOut.kind`），不能直接读内存。

### 6.2 区间迭代 `for i in a to b [step s]`

编译器目前把它展开成迭代器协议指令（`OP_ITER_*`）。两条路：

1. **在后端翻译这套协议**：解码 `OP_ITER_*`，分析里建模成"循环头 + 每轮 next 的比较与自增"，
   发射成普通 `cmp` / `jcc` / `add`（与 `while` 同一套）。要逐个核对协议指令的栈效果与边界语义：
   `step` 为负、`a > b` 的空区间、循环变量在体内被改写。
2. **在编译器里把 `for i in a to b` 降级成等价的 `while`**（当上下界都是整型表达式时）。
   后端一行都不用改，解释器也少几条指令；风险是 `for` 的语义细节（循环变量作用域、
   体内修改循环变量的行为）必须与现在逐位一致，需要解释器与分析器两边的对照测试。

**建议顺序**：先做 2（改动集中在编译器、收益立刻可见），再用 1 覆盖 `step` 与负步长等形态；
全局量按 6.1 的方案 B 先打通，再视需要迁移到方案 A。
