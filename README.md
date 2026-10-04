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

main()(
    new xs = [3, 1, 4, 1, 5]
    for x in sorted(xs)( print x )

    new text = "hello"
    print text.upper(), len(text)

    new d = Dict()
    d.set("k", 42)
    print d.get("k")

    try_it()          -- raises, caught below
    except e( print "caught: " + e )
)
```

Features: deep-copy value semantics, closures with shared captures, classes with inheritance and
magic methods (`__len__`, `__str__`, `__call__`, `__iter__`, …), declarative `view` components with
reactive `state`, compile-time macros, `Ok`/`Err`, slicing (`xs[1:4]`), optional/named/variadic
parameters, piecewise expressions, `guard`-free error handling with `except`.

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

| Module | Object | Content |
|---|---|---|
| `slice` | `SliceView` | `arr[1:4]`, `arr[2:]`, `arr[:3]`, `arr[:]` |
| `seq` | `Seq` | `map` `filter` `reject` `reduce` `fold` `any` `all` `count` `find` `index_where` `reverse` `unique` `flatten` `take` `drop` `slice_of` `chunk` `sort` `min_of` `max_of` `first` `last` `zip_with` `join` `sum_of` |
| `str` | `Str` | `repeat` `pad_left` `pad_right` `center` `reverse` `capitalize` `title` `count` `blank` `is_digit` `lines` `join` `hex` `format` |
| `dict` | `Dict` | `set` `get` `has` `remove` `size` `is_empty` `key_list` `value_list` `items` `clear` `merge` `copy` `keys_sorted` |
| `mathx` | `Mathx` | `gcd` `lcm` `factorial` `fib` `fib_iter` `is_prime` `primes` `clamp` `lerp` `round_to` `mean` `median` `variance` `stddev` `is_even` `is_odd` `digit_sum` |
| `file` | `File` `Dir` `Path` `Text` `Result` | see below |
| `time` | `Time` `Stopwatch` | `millis` `clock` `parts` `year`…`weekday` `make` `format` (strftime-style, implemented in Annota) `iso` `date` `stamp_text` `leap` `days_in_month` `day_of_year` `diff_ms` `describe_ms` |
| `os` | `Os` | `platform` `arch` `cpus` `pid` `home` `temp` `cwd` `env` `set_env` `env_all` `exec` `run` `ok` `which` |
| `thread` | `Thread` `Task` `Pool` | `hardware` `spawn` `run` `parallel_map` `parallel_each` `pool` |
| `net` | `Net` `Tcp` `Url` `Response` | `tcp` `server` `get` `post` `request` `parse_http` `resolve`, `Tcp` send/recv/recv_until/recv_all/close, `Url.encode/decode/parse` |
| `test` | `Test` | `suite` `ok` `eq` `ne` `near` `raises` `report` `check` |

```powershell
build\annota.exe examples\stdlib.ant      # tour of the standard library
build\annota.exe examples\algorithms.ant  # classic algorithms built on it
build\annota.exe examples\files.ant       # file API, 39 assertions
build\annota.exe examples\system.ant      # clock, threads, sockets, HTTP parsing (53 assertions)
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
