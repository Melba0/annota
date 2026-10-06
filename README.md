<div align="center">

# Annota

**A statically verifiable language, implemented in C++17 with a bytecode VM — plus the tooling around it: a REPL, a three-layer analyzer and a graphical IDE.**

[English](README.md) | [简体中文](README_zh.md)

[![build](https://github.com/Melba0/annota/actions/workflows/build.yml/badge.svg)](https://github.com/Melba0/annota/actions/workflows/build.yml)
[![Language](https://img.shields.io/badge/language-C%2B%2B17-blue.svg)](#quick-start)
[![GUI](https://img.shields.io/badge/GUI-Qt%206-green.svg)](#the-graphical-ide)
[![Tests](https://img.shields.io/badge/verification-build.ps1%20--Verify-success.svg)](#verification)
[![Analyzer](https://img.shields.io/badge/analyzer-32%20checks-orange.svg)](docs/reference.md)
[![License](https://img.shields.io/badge/license-see%20LICENSE-lightgrey.svg)](#license)

<img src="docs/images/ide-problems.png" alt="Annota Studio" width="820">

</div>

---

Annota is a small, annotation-driven language whose specification and implementation grew
together. Annotations are not decoration: `[[require]]`, `[[ensure]]`, `[[invariant]]`,
`[[decrease]]`, `[[taint]]` and friends are *specifications*, and the analyzer turns them into
either a proof obligation or a concrete bug report. The interpreter runs the same source that
the analyzer reads, so "what the tool tells you" and "what the program does" cannot drift apart.

**Highlights**

* 🧩 **One language, four front ends** — CLI runner, REPL, graphical IDE (`annota studio`), LSP server for editor plugins. All four share the same toolchain: no second implementation to keep in sync.
* 🔍 **Three-layer static analysis** — keystroke (<50 ms), save (<500 ms) and background (<5 s) budgets, 32 diagnostic codes, abstract interpretation over a CFG, contract checking at call sites.
* 🖥️ **Real Qt 6 GUI** — both for programs written in Annota (`view` components) and for the IDE that you write them in. Both degrade gracefully: a `-NoQt` build still gets `--gui-tree`, the REPL and the whole analyzer.
* 🔢 **C style numeric widths** — `int8..int64` / `uint8..uint64` / `float32` / `float64` plus the
  boxed `longlong` (128-bit) and `longdouble` (80-bit); declared widths wrap and round like C, and
  integer division truncates toward zero.
* 🧱 **Fixed size typed arrays** — `int[5]`, `int[3][4]`, `int[]` with contiguous storage, auto
  padding, O(1) row views, static bounds proofs and a `[[unsafe]]` opt-out from the runtime check.
* ⚡ **`[[jit]]` marker** — the one annotation that asks for load-time optimisation (constant folding
  + superinstruction fusion) of a function; measured on the loop benchmark.
* 🧭 **Case analysis and `elif`**: `if` / `elif` / `else` chains, where each path learns its own
  facts (`if i < 1 ( i = 1 )` then proves `i >= 1`), a branch that returns does not pollute the
  join, and every diagnostic says which branch it came from.
* 🧠 **The IDE knows the types**: hovering `int` / `long` / `longlong` / `double` ... reports the
  type (width, signedness, how to declare and convert), completion offers them as `type` items and
  the editor highlights them - all from one registry the parser, analyzer and highlighter share.
* 📁 **Batteries included, in the language itself** — C++ only exposes _-prefixed primitives (clock, env, threads, sockets, files); Seq / Str / Dict / Mathx / File / Path / Time / Os / Thread / Net / Test are .mod files written in Annota. New capabilities are new modules, not rebuilds.
* ✅ **Self-checking** — `build.ps1 -Verify` runs 8 example programs, 12 analyzer fixtures, a performance benchmark, an LSP end-to-end test and a headless IDE render.

---

## Table of contents

- [Quick start](#quick-start)
- [The language in 30 lines](#the-language-in-30-lines)
- [Command line](#command-line)
- [The REPL](#the-repl)
- [The graphical IDE](#the-graphical-ide)
- [Static analysis](#static-analysis)
- [Editor integration (LSP)](#editor-integration-lsp)
- [Standard library](#standard-library)
- [Primitives](#primitives)
- [Files and paths](#files-and-paths)
- [Project layout](#project-layout)
- [Verification](#verification)
- [Performance](#performance)
- [FAQ & troubleshooting](#faq--troubleshooting)
- [Contributing](#contributing)
- [License](#license)

---

## Quick start

```powershell
git clone https://github.com/Melba0/annota.git
cd annota

# build (auto-detects Qt 6 in D:\Qt, C:\Qt, %USERPROFILE%\Qt)
powershell -ExecutionPolicy Bypass -File build.ps1

# build and run every check (examples, analyzer fixtures, benchmark, LSP, IDE)
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify

build\annota.exe examples\selfcheck.ant    # 72 assertions covering the language
build\annota.exe studio                    # the graphical IDE
```

Requirements: a C++17 compiler (MinGW-w64 g++ 13 or MSVC 2022). **Qt 6 Widgets is optional** —
without it you lose the window and the IDE, nothing else.

| Build flavour | Command | You get |
|---|---|---|
| Full | `build.ps1` | interpreter + `--gui` + `annota studio` + LSP |
| No Qt | `build.ps1 -NoQt` | interpreter + REPL + analyzer + LSP (no windows) |
| CMake | `cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt>/<ver>/<kit>` | same as full |

Verify what a binary can do at any time:

```powershell
build\annota.exe --features
# contracts annotations files gui ide <- full build
# contracts annotations files (+ guidance) <- no-Qt build
```

## The language in 30 lines

There is no `main`: a file is a script, and its top-level statements run in order. Declare things
above where you use them.

```annota
-[ annotations are the specification ]-
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

-- top level code: this *is* the entry point
new xs = [3, 1, 4, 1, 5]
for x in sorted(xs)( print x )

new text = "hello"
print text.upper(), len(text)

new d = Dict()
d.set("k", 42)
print d.get("k")

print divide(10, 2)        -- 5
print count_to(4)          -- 4

try_it()                   -- raises, caught below
except e( print "caught: " + e )
```

Wrapping everything in a `main()` function is *not* required and only changes when the code runs
(the function body is skipped until you call it) — see [docs/style.md](docs/style.md).

Features: deep-copy value semantics, closures with shared captures, classes with inheritance and
magic methods (`__len__`, `__str__`, `__call__`, `__iter__`, …), declarative `view` components with
reactive `state`, compile-time macros, `Ok`/`Err`, slicing (`xs[1:4]`), optional/named/variadic
parameters, piecewise expressions, `guard`-free error handling with `except`.

### Syntax details

**Constructors.** The class parameter list holds the constructor parameters; `__init__` takes
**empty parentheses**, runs at construction time, and sees those parameters as locals. Declared
field defaults are applied first, so `__init__` can override them.

```annota
Point(x, y)=(
    X:int              -- declared field, default 0
    Y:int
    __init__()(
        X = x          -- `x`, `y` are the class parameters, visible here
        Y = y
    )
    norm2() = X * X + Y * Y
)
new p = Point(3, 4)
print p.norm2()        -- 25
```

**Several statements per line.** Declarations, assignments and deletions accept comma lists, and a
comma also separates statements when a line holds more than one:

```annota
new a, b                   -- two declarations
new x = 1, y = 2, z = 3    -- each with its own value
a = 1, b = 2               -- two assignments on one line
del a, b                   -- delete both
d + "y"                    -- a bare expression statement is fine
```

**Line continuation.** A line ending in a binary operator continues on the next line, and `\` joins
explicitly, so long arithmetic can be laid out naturally:

```annota
new total = 1 +
            2 +
            3                      -- 6
new joined = "a" + \
             "b"                   -- "ab"
```

**Variadic parameters.** `*args` (Python style) and `...args` are equivalent; the collected value is
an ordinary `List`. `**kwargs` is **not** supported — pass a `Dict` instead.

```annota
total(*nums)(
    new s = 0
    for n in nums( s = s + n )
    =s
)
print total(), total(1), total(1, 2, 3)     -- 0 1 6

join_with(sep, *parts)( ... )               -- fixed parameters first, then the variadic tail
```

**Slicing** (`use slice`) keeps the type: a list or tuple slice is a read-only `SliceView`
(`.to_list()`, `len`, iteration), a **string slice is a string**.

```annota
new arr = [10, 20, 30, 40, 50]
print arr[1:3].to_list()   -- [20, 30]
print arr[2:]              -- SliceView(30, 40, 50)
new text = "abcdefg"
print text[2:5]            -- "cde"   (a String, not a view)
```

**Two formatting rules that bite.** An `else` must sit on the same line as the `)` that closes the
`if` body — the formatter relies on that to tell a statement from an expression:

```annota
if n > 0( print "positive" ) else ( print "not positive" )     -- ✓
if n > 0(
    print "positive"
) else (                                                        -- ✓ `) else (` on one line
    print "not positive"
)
```

and a statement cannot be broken across lines unless the line ends with an operator, a comma inside
brackets, or a `\`:

```annota
new s = "a" +
        "b"          -- ✓ continuation
new t = "a"
        + "b"        -- ✗ `+ "b"` is parsed as a new statement
```

📘 **[Syntax reference](docs/syntax.md)** — every statement, operator and literal, plus the
exact rules for line continuation and `else` placement.
📘 **[Coding style](docs/style.md)** — how to lay out a file, name things and use annotations.
📘 **[`[[jit]]`](docs/jit.md)** — what the optimisation marker does: superinstructions plus a
x86-64 machine-code backend for integer code, with automatic fallback.
📘 **[Linking C++ (FFI)](docs/ffi.md)** — register native functions and modules from C++, either
linked into the binary or loaded as a plugin; the way to make the standard library faster
without touching the language core.

The standard library is layered accordingly: the language core is lexer / parser / compiler / VM,
`lib/*.mod` is the readable script layer, and the hot kernels live in C++ registered through the
FFI — `Seq.sort`, `kth`, `median`, `dedup`, `sort_by`, `lower_bound` and `upper_bound` call the
native `seqnative` module in `native/seq_native.cpp`, which is not part of the core at all.
*(Both guides are currently written in Chinese.)*

## Command line

```
annota                          REPL (also the default with no arguments)
annota studio [file.ant]        graphical IDE (Qt build)
annota run|check <file.ant>     execute / parse only
annota analyze <file> [opts]    static analysis        (see docs/reference.md)
annota analyze-suite <dir>      run the checker over fixtures
annota ide <query> <file> ...   hover, definition, rename, complete, inline,
                                coverage, quickfix, suppressions, report,
                                checks, annotations, docs
annota bench                    analysis performance benchmark
annota lsp                      language server (stdio, JSON-RPC)

  -e <code>            evaluate a snippet
  --dump-tokens        print the token stream
  --dump-ast           print the parsed program
  --dump-bc            disassemble the bytecode
  --dump-annotations   print the annotation index as JSON
  --contracts          evaluate assert/require/ensure/invariant at run time
  --gui                show the program's view in a window   (Qt build)
  --gui-tree           print the view tree as text           (no Qt needed)
  --gui-shot <png>     render the view to a PNG without opening a window
  --features           report the capabilities of this binary
```

## The REPL

`annota` with no arguments starts an interactive session with a persistent VM, so definitions
survive between inputs:

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

| Command | Purpose |
|---|---|
| `:help` / `:quit` | help / leave |
| `:globals` / `:macros` | list globals and registered macros |
| `:load <file>` | execute a file inside the session |
| `:analyze [snippet]` | run the analyzer over the session history (or one snippet) |
| `:bc <code>` / `:tokens <code>` | disassemble / dump tokens |
| `:reset` | forget globals, classes and macros |

Incomplete brackets switch the prompt to `...>` so functions, classes and lists can span lines.

## The graphical IDE

```powershell
build\annota.exe studio examples\algorithms.ant
```

| Problems | Structure | Coverage |
|---|---|---|
| ![problems](docs/images/ide-problems.png) | ![structure](docs/images/ide-structure.png) | ![coverage](docs/images/ide-coverage.png) |

* **Editor** — line numbers, Annota syntax highlighting, diagnostic squiggles, current-line
  highlight, auto-indent, bracket pairing, `Tab` = 4 spaces.
* **Problems** — severity, line, check code and message; **double-click jumps to the line**;
  the tooltip carries the abstract values and the suggested fix.
* **Structure** — every function with its **✓ / ? / ✗** contract status, double-click to jump.
* **Coverage** — contract and loop-invariant coverage, functions still lacking contracts, the
  list of `[[ignore]]` suppressions (including the ones that no longer suppress anything), and
  every available quick fix.
* **Output** — `F5` runs the buffer (or `Ctrl+F5` with contract checks); a failing run jumps to
  the offending line. When the program defines a `view`, the panel tells you to press `F7`.
* **Preview (`F7`)** — runs the buffer and opens the program's own `view` as a real Qt window,
  wired to its `state`, so buttons and counters work. `Ctrl+F7` prints the component tree
  instead. Plain `F5` stays a console run — that is why a GUI program appears to "do nothing"
  if you only press `F5`.
* **Hover panel + status bar** — annotation documentation, the analyzer's abstract state for the
  variable under the cursor, diagnostics of the current line, and a `✓ ? ✗` function summary.
* **`input` works here too** — in the IDE (and in a `--gui` program) `input name` opens a modal
  input dialog titled with the variable name instead of waiting for a console; a cancelled
  dialog yields an empty string. Head-less runs can pre-answer with `ANNOTA_INPUT=<text>`
  (UTF-8, ASCII is safest on a non-UTF-8 console), which is what CI uses. See
  [examples/input.ant](examples/input.ant).
* **Help menu** — annotation manual (`F1`), check manual (`F2`) and the file API, all generated
  from the analyzer's own registries.

Shortcuts: `F5`/`Ctrl+F5` run, `F7`/`Ctrl+F7` preview the program's view (or print its component
tree), `F6` analyze, `Ctrl+S` save, `Ctrl+/` comment, `Ctrl+1` insert the selected quick fix,
`Ctrl+Shift+1` insert all of them.

The IDE can also run head-less, which is what CI uses:

```powershell
build\annota.exe studio examples/files.ant --run --shot ide.png        # screenshot, then exit
build\annota.exe studio examples/gui_counter.ant --preview --shot p.png   # also open the view window
```

Programs written in Annota can open their own windows too:

![Annota view window](docs/images/gui-counter.png)

```powershell
build\annota.exe examples\gui_counter.ant --gui
```

## Static analysis

```powershell
build\annota.exe analyze examples/analysis/01_basics.ant
build\annota.exe analyze file.ant --json > report.json
build\annota.exe analyze file.ant --level=1 --min-severity=warning
build\annota.exe analyze file.ant --int-bits=64 --modules
```

Three layers, each with a latency budget:

| Layer | Trigger | Content | Budget |
|---|---|---|---|
| L1 | keystroke | lexer/parser errors, bracket mismatch, annotation registry/scope/conflicts | 50 ms |
| L2 | save | intra-procedural data flow: division by zero, overflow, out of bounds, null dereference, uninitialised, dead store, unreachable, type/scope/iterator errors, call-site contracts, `ensure`, `invariant` | 500 ms |
| L3 | background | loop invariant preservation, termination, taint, purity, macro side effects, redundant conditions | 5 s |

The **32 diagnostic codes** are stable identifiers (`severity`: `error` = provably wrong,
`warning` = possible, `info` = note). Every report is available as JSON:

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

> Diagnostic **messages are Chinese**; codes, severities and the JSON schema are English, so
> tooling should key on `code`. The full list lives in [docs/reference.md](docs/reference.md)
> (generated from the analyzer's registries by `annota ide docs`).

**A small CAS backs the checks.** Conditions are normalised to integer linear forms
(`sum(k_i * v_i) + c`), so `assert(i + 1 > i)`, `assert(2 * k == k + k)` or `assert(n - 1 < n)`
are *proved* rather than reported as unprovable, `[[assume: x == 5]]` becomes a substitution
that makes `assert(x * 2 == 10)` provable, and `if a + 1 > a` is flagged as redundant.

**Annotations as specifications.** `[[assume]]` narrows the abstract state, `[[assert]]` must be
proved, `[[require]]` is assumed at the entry and *checked at every call site*, `[[ensure]]` is
checked at every return with `result` bound, `[[invariant]]` is checked at the loop entry and at
the end of each iteration, `[[decrease]]` must be positive and strictly decreasing,
`[[taint]]` marks a source and `[[unsafe]]` marks a boundary, `[[ignore: code]]` suppresses one
check (and is reported as unused when it stops matching anything).

## Editor integration (LSP)

`annota lsp` speaks the Language Server Protocol over stdio (`Content-Length` framing):

* `didOpen` / `didChange` → L1 diagnostics immediately (keystroke budget)
* `didSave` → L2 diagnostics synchronously, then L3 **on a cancellable background thread**
* `hover`, `definition`, `rename` (including annotation names), `completion`

VS Code style registration:

```json
{ "command": "D:\\path\\to\\build\\annota.exe", "args": ["lsp"], "filePattern": "*.ant" }
```

```powershell
powershell -ExecutionPolicy Bypass -File tools/lsp_smoke.ps1    # end-to-end check
```

## Standard library

The runtime deliberately knows very little: C++ exposes only `_`-prefixed **primitives** (see
[Primitives](#primitives)), and everything else is a module written in Annota under `lib/`.
Adding a capability therefore normally means writing a `.mod` file, not rebuilding the
interpreter.

Native modules (registered by the runtime, always available): `math`, `io`, `json`, plus the
small `time` clock module and the `system` facade over the primitives.

Script modules in `lib/`, loaded with `use <name>` or all at once with `use std`:

Script modules in `lib/`, loaded with `use <name>` (or `use sub/name` for a module in a
subdirectory) or all at once with `use std`.  A missing module is an error that lists where we
looked, what exists and the closest name.  Full API reference: **[docs/stdlib.md](docs/stdlib.md)**.

| Module | Object | Content |
|---|---|---|
| `seq` | `Seq` `Heap` | `map` `filter` `reduce` `scan` `zip_with` `chunk` `window` `rotate` `flatten` `transpose`; **algorithms**: `merge_sort` `sort_by` `heap_sort` `insertion_sort` `kth` `median` `quantile` `lower_bound` `upper_bound` `bsearch` `merge_sorted` `insert_sorted`; **sets**: `dedup` `union` `intersect` `difference` `is_subset`; **stats**: `mean` `variance` `stddev` `frequencies` `mode` `min_max`; `Heap.from_list(...).drain()` |
| `dict` | `Dict` `Set` `Counter` | hash table with open addressing (FNV-1a, resize at 0.75, shrink at 0.125): `set` `get` `get_or` `has` `remove` `items_sorted` `map_values` `filter` `invert` `merge` `copy` `d[k]` `len(d)`; `Set` union/intersect/difference; `Counter` `most_common` `total` |
| `text` | `Text` | KMP `find_all`/`find_kmp` `split_by` `replace_all` `words` `wrap` `snake_case` `camel_case` `levenshtein` `similarity` `lcs` `lcs_length` `is_palindrome` `is_anagram` `caesar` `rot13` `csv_parse` `csv_format` `base64_encode` `hex_dump` `ngrams` `char_freq` `word_freq` |
| `numeric` | `Num` | `sieve` `is_prime` `nth_prime` `prime_sum` `prime_factors` `divisors` `divisor_count` `divisor_sum` `totient` `goldbach` `mod_pow` `mod_inverse` `gcd_ext` `fib` (fast doubling) `collatz_*` `binomial` `pascal_row` `catalan` `integer_sqrt` `digits` `base_str` `parse_base` `binary_search_monotone` `hanoi` |
| `matrix` | `Matrix` | flat row-major storage, `mul` (i-k-j), `pow` (fast exponentiation), `det` (Gaussian elimination), `transpose` `trace` `add` `scale` `mul_list` `from_lists` `to_lists` `identity` |
| `stats` | `Stats` | `mean` `weighted_mean` `median` `mode` `variance` `stddev` `percentile` `iqr` `skewness` `kurtosis` `covariance` `correlation` `rank` `zscores` `normalize` `histogram` `moving_average` `summary` |
| `graph` | `Graph` `DSU` | `bfs` `dfs` `distances` `shortest_path` (Dijkstra + binary heap) `all_pairs` (Floyd–Warshall) `topological_sort` (Kahn) `has_cycle` `connected_components` `is_bipartite` `kruskal`; `DSU` with path compression + union by rank |
| `geometry` | `Geo` | `dist` `dist2` `cross` `orientation` `segments_intersect` `polygon_area` (shoelace) `point_in_polygon` (ray casting) `convex_hull` (Andrew monotone chain, O(n log n)) `closest_pair` (divide and conquer) `bounding_box` `centroid` |
| `slice` | `Slice` `SliceView` | `arr[1:4]`, `arr[2:]`, `arr[:3]`, `arr[:]` |
| `str` | `Str` | `repeat` `pad_left` `pad_right` `center` `reverse` `capitalize` `title` `count` `blank` `is_digit` `lines` `join` `hex` `format` |
| `mathx` | `Mathx` | `gcd` `lcm` `factorial` `fib` `fib_iter` `is_prime` `primes` `clamp` `lerp` `round_to` `mean` `median` `variance` `stddev` `is_even` `is_odd` `digit_sum` |
| `file` | `File` `Dir` `Path` `FileText` `Result` | see below |
| `time` | `Time` `Stopwatch` | `millis` `clock` `parts` `make` `format` (strftime-style, implemented in Annota) `iso` `date` `stamp_text` `leap` `days_in_month` `diff_ms` `describe_ms` |
| `os` | `Os` | `platform` `arch` `cpus` `pid` `home` `temp` `cwd` `env` `set_env` `env_all` `exec` `run` `ok` `which` |
| `thread` | `Thread` `Task` `Pool` | `hardware` `spawn` `run` `parallel_map` `parallel_each` `pool` |
| `net` | `Net` `Tcp` `Url` `Response` | `tcp` `server` `get` `post` `request` `parse_http` `resolve`, `Url.encode/decode/parse` |
| `test` | `Test` | `suite` `ok` `eq` `ne` `near` `raises` `report` `check` |

```powershell
build\annota.exe examples\stdlib.ant       # tour of the standard library (69 assertions)
build\annota.exe examples\algorithms.ant   # sorting / DP / graphs / number theory / geometry (82)
build\annota.exe examples\perf.ant         # performance benchmark: 12 workloads + timings
build\annota.exe examples\collections.ant  # hash containers, hash vs linear scan
build\annota.exe examples\strings.ant      # string algorithms
build\annota.exe examples\graphs.ant       # BFS / Dijkstra / topological sort / MST / maze
build\annota.exe examples\numerics.ant     # number theory workbook
build\annota.exe examples\geometry.ant     # convex hull / areas / closest pair
build\annota.exe examples\files.ant        # file API, 39 assertions
build\annota.exe examples\system.ant       # clock, threads, sockets, HTTP parsing (53 assertions)
```


## Primitives

C++ is a thin shim over the OS. These are the only things a `.mod` file cannot express itself;
`docs/reference.md` lists them with signatures.

| Primitive | Purpose |
|---|---|
| `_sys_clock()` `_sys_time()` `_sys_sleep(ms)` | monotonic ms, wall-clock ms, sleep |
| `_sys_local_time([ms])` `_sys_make_time(y,mo,d,…)` | broken-down time ⇄ timestamp |
| `_sys_info(key)` | `platform` `arch` `cpus` `pid` `home` `temp` `cwd` |
| `_sys_env` `_sys_env_set` `_sys_env_all` `_sys_exec` | environment and shell commands |
| `_sys_spawn(fn[, args])` `_sys_task_done` `_sys_join` | worker threads |
| `_sys_socket(op, …)` | `resolve` `connect` `listen` `accept` `send` `recv` `close` `shutdown` `timeout` |
| `_file_*` `_dir_*` `_path_*` `_cwd` `_chdir` `_stdin_*` | files, directories, paths, stdin |

Each one is mirrored by name in the `system` facade, so library code can dispatch dynamically:

```annota
print system.clock()                    -- same as _sys_clock()
print system.call("info", "platform")   -- dynamic: _sys_info("platform")
for p in system.primitives( print p )   -- what this build provides
```

`time.millis()` / `time.now()` / `time.clock()` / `time.sleep(ms)` stay available directly
without `use time`, because measuring time is needed everywhere.

Threads: a worker runs the function on its own VM whose globals are a **deep copy** of the
caller's (code, classes and natives are shared because they are immutable). Cells captured by a
closure stay shared, so worker functions must treat captured state as read-only — the
analyzer's `[[pure]]` is the tool for checking that.

## Files and paths

Two layers. The **`_`-prefixed primitives** are the only place where the runtime touches the
operating system: they raise on error and are always available.

```annota
print _file_read("notes.txt")           -- throws; catch with except
print _file_write("out.txt", "hello")   -- returns the number of bytes
print _dir_list(".")                    -- sorted names
print _path_join("build", "x.txt")      -- platform separator
```

The **high-level API** (`use file`) wraps them with three error-handling styles:

```annota
use file

new text  = File.read("a.txt")                  -- 1) raises, use with except
new probe = File.try_read("a.txt")              -- 2) Result: is_ok / unwrap / unwrap_or
new safe  = File.read_or("a.txt", "fallback")   -- 3) default value

new lines = File.lines("a.txt")
new obj   = File.json("d.json")
new rows  = File.csv("t.csv")

File.write("a.txt", "content")
File.append_line("a.txt", "one more line")
File.write_lines("a.txt", ["x", "y"])
File.write_json("d.json", {"n": 1})

File.exists(p)  File.is_file(p)  File.is_dir(p)  File.size(p)  File.describe(p)
File.copy(from, to)  File.move(from, to)  File.remove(p)  File.touch(p)

Dir.list(p)  Dir.files(p)  Dir.dirs(p)  Dir.walk(p)  Dir.find(p, ".ant")  Dir.size_of(p)
Dir.make(p)  Dir.remove(p, recursive)  Dir.clear(p)

Path.join(a, b[, c])  Path.dirname(p)  Path.basename(p)  Path.ext(p)  Path.stem(p)
Path.with_ext(p, ".md")  Path.abs(p)  Path.normalize(p)  Path.parts(p)
```

Non-ASCII paths are converted to the wide Windows API, so `文件/笔记.txt` works; line endings are
normalised to `\n` on read. The complete list of primitives is in
[docs/reference.md](docs/reference.md#3-file-and-path-primitives).

## Project layout

```
src/                    ~13k lines of C++17
  lexer, parser          tokens, macros, annotations, module loading
  ast, compiler, vm      bytecode compiler and stack VM (deep-copy semantics)
  builtins               natives, pseudo methods, GUI components, native modules
  gui_qt / gui           Qt view renderer / textual tree + no-Qt stubs
  repl                   interactive session (persistent VM)
  analyzer, analysis_flow  annotation registry, CFG, abstract interpretation, checks
  ide_cmd, ide_gui, lsp  CLI queries, graphical IDE, language server
lib/                    standard library modules (*.mod)
examples/               example programs, including examples/analysis/ fixtures
docs/                   reference manual (generated) and screenshots
tools/lsp_smoke.ps1     LSP end-to-end test
build.ps1               one-command build + verification
```

## Verification

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify
```

| Step | What it proves |
|---|---|
| 8 example programs | the language semantics still work (`selfcheck.ant` alone has 72 assertions) |
| 12 analyzer fixtures | each check fires when it should and stays silent when it should |
| analyzer over the examples | **zero false positives** on real, correct code |
| `annota bench` | the three latency budgets are met |
| LSP smoke test | a full editor session (open → hover → rename → save → diagnostics) |
| `studio --shot` | the IDE builds its window and renders headlessly |

## Performance

`annota bench` generates a ~2000-line, 80-function program and measures every layer
(MinGW-w64 g++ 13.1, `-O2`, one core):

| Layer | Budget | Measured |
|---|---|---|
| L1 keystroke | 50 ms | **8 ms** |
| L2 save | 500 ms | **23 ms** |
| L3 background | 5 s | **23 ms** |

### Runtime speed vs C++

`examples/perf.ant` is a self-timing benchmark (`annota examples/perf.ant`), and
`tools/cpp_baseline.cpp` runs the same workloads in C++ (`g++ -O2 tools/cpp_baseline.cpp -o cpp_baseline`).
On one machine (MinGW-w64 g++ 13.1, `-O2`, one core), comparing **total time per workload**:

| Workload | Annota | C++ | Ratio | % of C++ |
|---|---|---|---|---|
| int loop, no `[[jit]]` | 62 ms | 0.085 ms | 729x | 0.14% |
| int loop with `[[jit]]` | 26 ms | 0.085 ms | 306x | 0.33% |
| function call (40k) | 43 ms | 0.056 ms | 768x | 0.13% |
| fixed array `int[n]` | 23 ms | 0.232 ms | 99x | 1.0% |
| list push | 69 ms | 0.508 ms | 136x | 0.74% |
| merge sort (2000) | 207 ms | 0.087 ms | 2379x | 0.04% |
| heap sort (2000) | 25 ms | 0.078 ms | 321x | 0.31% |
| binary search (4k x4k) | 67 ms | 0.185 ms | 362x | 0.28% |
| hash lookup (4k keys) | 147 ms | 0.239 ms | 615x | 0.16% |
| median by sorting | 1455 ms | 0.142 ms | 10246x | 0.01% |
| median by quickselect | 59 ms | 0.058 ms | 1017x | 0.10% |
| sieve of Eratosthenes | 54 ms | 0.107 ms | 505x | 0.20% |
| KMP search (2000 chars) | 4 ms | 0.211 ms | 19x | 5.3% |
| edit distance (80x80) | 665 ms | 0.428 ms | 1554x | 0.06% |
| matrix multiply (30x30) | 128 ms | 0.010 ms | 12800x | 0.01% |
| Dijkstra (14x14 grid) | 149 ms | 0.065 ms | 2292x | 0.04% |
| **total** | **3183 ms** | **2.58 ms** | **1236x** | **0.081%** |

How to read this:

* **Dispatch costs ~30 ns per VM instruction**: the plain loop is 155 ns per iteration (about
  five instructions: add, store, increment, store, compare/branch) and the `[[jit]]`-fused loop is
  65 ns for two instructions - the same ~30 ns either way.  A *scalar* C++ loop is ~0.5-1 ns per
  operation, so instruction dispatch is roughly **2-3% of scalar C++**; the 0.14% above is against
  a vectorized C++ loop.
* **Script-level algorithms land at 0.01%-1%** of C++ (sorting, DP, graphs), because each
  algorithmic step expands into many VM instructions and allocations.  `KMP` (5.3%) and fixed
  arrays (1.0%) are the good cases: long tight loops over cheap operations.
* **Native primitives run at C++ speed**: `sorted()`, `len`, `sum`, string methods and file I/O are
  single C++ calls (the `原生 sorted()` row measures 0 ms - it is one `std::sort`).
* For calibration, this is **CPython-class on dispatch** (CPython's simple int loop is ~20-35 ns per
  iteration, the fused Annota loop is 65 ns) and **~12x slower than CPython on function calls**
  (~1075 ns vs ~50-90 ns), which is the clearest remaining target.
* Algorithm choice still matters more than the interpreter: quickselect is 25x faster than
  "sort then take the middle" *inside Annota*, exactly as `nth_element` beats `sort` in C++.

## FAQ & troubleshooting

<details>
<summary><b><code>--gui</code> or <code>annota studio</code> says "this build has no Qt support"</b></summary>

That binary was built without Qt. Check and fix:

```powershell
build\annota.exe --features        # needs "gui ide" in the output
powershell -ExecutionPolicy Bypass -File build.ps1 -Clean -QtRoot "D:\Qt\6.10.1\mingw_64"
```

`build.ps1` probes `D:\Qt`, `C:\Qt` and `%USERPROFILE%\Qt` for
`6.x/{mingw_64,msvc2022_64,msvc2019_64}`; when it finds nothing it prints a warning and builds
the no-Qt flavour. With CMake pass `-DCMAKE_PREFIX_PATH=<Qt>/<version>/<kit>`.
If `Qt6Widgets.dll` or `platforms/qwindows.dll` are missing from `build/`, the error is
`no Qt platform plugin could be initialized` instead.
</details>

<details>
<summary><b><code>--gui</code> does nothing / the window flashes</b></summary>

`--gui` shows the program's last `view`; the program must define one.
`annota studio` is the IDE — for that you do **not** need a `view`.

```powershell
build\annota.exe examples\gui_counter.ant --gui        # window
build\annota.exe examples\algorithms.ant --gui-tree    # no view: prints nothing
```

In the IDE, `F5` only runs the program and shows its console output — a `view` is *not* opened
by `F5`. Press **`F7`** (Run → Preview view) to open the program's window; the output pane
reminds you whenever the program defines one. See
[examples/gui_counter.ant](examples/gui_counter.ant) for a complete example.
</details>

<details>
<summary><b><code>input</code> does not work / the program hangs</b></summary>

`input` reads from **stdin** in a console run — pipe something in, or it stops at end of input:

```powershell
"world" | build\annota.exe examples\input.ant      # console: reads the pipe
build\annota.exe examples\input.ant                # interactive console: type a line
```

Inside a window there is no console to read from, so a Qt build asks with a **dialog** instead:
either run the program with `--gui`, or press `F5` / `F7` in `annota studio`. A cancelled dialog
returns an empty string, so programs should handle `""`. `_stdin_line()` follows the same rule.

For automated GUI runs, pre-answer with the environment variable:

```powershell
$env:ANNOTA_INPUT = "hello"
build\annota.exe studio examples\input.ant --run --echo --shot out.png
```
</details>

<details>
<summary><b>The analyzer reports "使用了未声明的变量" (undeclared variable)</b></summary>

Check for a typo, or a variable that only exists inside a `use`d module. Analysis reports the main
file by default; `annota analyze --modules` also analyses inlined modules and prefixes each
diagnostic with the `.mod` it came from.
</details>

<details>
<summary><b>Non-ASCII paths fail on Windows</b></summary>

Use `use file` (`File` / `Dir` / `Path`) or the `_file_*` primitives — both convert UTF-8 to the
wide-character API. Opening a path with `io.read_file` on a non-UTF-8 codepage may not.
</details>

More in [docs/reference.md](docs/reference.md) and in the Chinese README's FAQ section.

## Contributing

Issues and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for the build,
test and style conventions, and [CHANGELOG.md](CHANGELOG.md) for the version history.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify   # must pass before a PR
```

## License

Released under the terms of the [LICENSE](LICENSE) file in this repository.

Third-party components: none bundled. A Qt 6 build links against Qt (LGPL-3.0) — the DLLs copied
into `build/` by `build.ps1` are yours to redistribute under Qt's terms.
