# Changelog

All notable changes to this project are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project aims
to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html): the **language** and the
**diagnostic codes** are the public API, so a change that makes previously valid code invalid is a
major change, and a new check is a minor one.

## [Unreleased]

### Added

* **`lend` parameters: borrow instead of copy.**  `total(lend xs)` declares that the callee only
  *reads* the container, so the call binds the caller's list/array/dict without deep copying it (the
  copy happens at the call boundary, which a `lend` statement inside the callee is too late to
  avoid).  The compiler enforces the promise: mutating the parameter (`xs[i] = v`, `xs.push(v)`,
  `del xs[i]`), letting it escape (`=xs`, `new t = xs`, `[xs]`, closure capture) or using it as a
  method receiver is a compile error.  The standard library's sorting/searching family
  (`Seq.sort` / `sort_by` / `merge_sort` / `heap_sort` / `insertion_sort` / `kth` / `median` /
  `quantile` / `dedup` / `unique` / `is_sorted` / `bsearch` / `bsearch_by` / `lower_bound` /
  `upper_bound` / `sort_native`) now takes `lend xs`, which removes a full copy per call; together
  with an in-place (`ffiItems`-free) `seqnative.lower_bound`/`upper_bound`, the 4000-element binary
  search benchmark went from **440 ms to 14 ms** and the whole `perf.ant` suite from 1.60 s to
  **0.96 s**.  See the syntax reference §4.1.3.
* **Automatic releases.**  `.github/workflows/release.yml` publishes a GitHub Release whenever a
  `v*` tag is pushed (or on demand via `workflow_dispatch`): it builds the interpreter on Windows
  (MinGW + Qt 6) and Linux, assembles a self-contained bundle for each (`lib/` next to the
  executable, plus `src/`, `native/`, `plugins/`, `docs/`, `examples/` and the plugin import
  library on Windows), **smoke-tests the extracted bundle** with `annota examples/selfcheck.ant`
  and fails if it does not report `0 failures`, then uploads both archives with `SHA256SUMS` and
  generated notes.  Tags with a suffix (`v1.2.0-rc1`) become pre-releases.  See the Releasing
  section of CONTRIBUTING.md.
* **Automatic JIT.**  The interpreter counts backward jumps per chunk and, once a loop is hot
  (default 4000 iterations, `ANNOTA_JIT_THRESHOLD`), compiles that function with the x86-64
  backend and switches to it *in place* through a loop-header entry point (on-stack replacement),
  so a single long call speeds up too.  `[[jit]]` now only means "compile at load time".
* **Hot-pluggable C++.**  `annota plugin build <file.cpp>` turns one C++ file into a plugin
  (default `build/plugins/<name>.dll`), and `use <name>` loads it automatically - no interpreter
  rebuild, no flags.  `--plugin` and `ANNOTA_PLUGIN` still work for plugins kept elsewhere, the
  analyzer loads the same plugin, and the loader now also searches next to the executable and the
  program's parent `lib/`, so a GUI launch resolves the standard library regardless of the
  working directory.  See `docs/ffi.md` and `plugins/hello.cpp`.
* `examples/plugin.ant` (a script that `use`s a plugin module) and an automatic-JIT suite in
  `examples/jit.ant`.

### Documentation

* **New usage manual: `docs/tools.md`** — build flags, every subcommand and option, all environment
  variables, the plugin workflow (including the manual compile commands), how to reproduce the
  benchmarks and a troubleshooting section.  Both READMEs link it and summarise the essentials.
* `docs/syntax.md`: reserved words now list `lend` and `elif`; new §4.1.1 (value semantics) and
  §4.1.2 (`lend`) replace the version that had been inserted in the middle of the typed-array
  section; the `[[jit]]` note and §11 gained the automatic-JIT behaviour.
* `docs/jit.md` was rewritten around automatic promotion (thresholds, OSR, measured numbers,
  how to reproduce) and `docs/ffi.md` now leads with `annota plugin build` and shows commands that
  actually link against the interpreter's import library.
* `docs/stdlib.md`: the sorting/selection/lookup tables now say which entry points are native
  (`seqnative`) and which are script reference implementations.
* `docs/style.md` gained guidance on when to reach for native code and on value semantics;
  `CONTRIBUTING.md` gained an "Adding native code (FFI, kernels and plugins)" section and an
  updated documentation section; the PR template lists the native/plugin checklist items.
* Both READMEs: command line and environment variables, the new guides, the real verification
  steps (19 examples, 16 fixtures, plugin round-trip, highlighter check) and freshly measured
  performance tables.

* **References: `lend a = b`.**  Value semantics stay the default (assignment, arguments and
  returns deep copy), and `lend` is the explicit way to share: both names then read and write the
  same storage cell, closures capture that same cell, and the analyzer knows the two names are
  aliases (no bogus dead-store reports, and mutating one drops what was known about the other).
  Only a variable can be lent - an expression, a constant or an outer function's local is a
  compile error.  See the syntax reference for the rules.

* **A C++ linking interface (FFI).**  `src/ffi.hpp` lets any C++ translation unit register native
  functions and whole modules (`ANNOTA_MODULE` / `ANNOTA_FUNCTION`); `use <module>` resolves them
  and members are ordinary calls, so new native capability no longer requires a language core
  change.  `native/fast.cpp` is a worked example (numeric kernels, a sieve, callbacks into Annota
  and a timing helper), `build.ps1` links `native/*.cpp` automatically, and `--plugin <file>` /
  `ANNOTA_PLUGIN` load the same module as a shared library.  See `docs/ffi.md`.
* **A machine-code backend for `[[jit]]`.**  Eligible integer functions are translated to x86-64
  at load time (`src/jit.hpp`), on top of the superinstruction pass; anything that cannot be
  translated keeps running on the interpreter, so the marker never changes a program's meaning.
  New fusions: compare-and-branch (`i < n` + jump), compare-and-branch against an immediate, and
  `a[i] = a[i] + k`.  A `while` loop of 400k iterations went from 69 ms to under 1 ms.
* **The hot standard-library kernels moved out of the core.**  `sorted`, `nth`, `argsort`,
  `lower_bound` and `upper_bound` are no longer builtins compiled into the interpreter: they are
  the native module **`seqnative`** (`native/seq_native.cpp`), registered through the FFI, and
  `lib/seq.mod` calls it explicitly (`use seqnative`).  `Seq.sort`, `Seq.kth`, `Seq.median`,
  `Seq.dedup`, `Seq.sort_by`, `Seq.lower_bound`, `Seq.upper_bound` and `Seq.bsearch` are thin
  wrappers over it; the script implementations stay available under their own names as reference
  versions and for custom comparators.  Adding another algorithm now means adding a
  `native/*.cpp` file - `src/builtins.cpp` is untouched.
* `annota ide docs` gained a **Linked C++ modules (FFI)** section generated from the FFI
  registry, so the reference manual lists every linked module and member automatically.
* `examples/ffi.ant` and `examples/jit.ant`, plus `docs/ffi.md` and `docs/jit.md`.

### Fixed

* **Deep copy of containers actually copies them.**  `copyRec` (the engine behind `deepCopy`)
  had no case for `List`, `Tuple` or `Map`, so `new b = a` shared `a`'s list: `b[0] = 99` was
  visible through `a`.  This also silently disabled the documented by-value argument semantics,
  and several library routines only "worked" because of it.
* Library routines that relied on the broken copy: `Seq.heap_sort` (heap sift-down ran on a
  copy), `Seq.kth_script`/`Seq._partition`, `Seq.swap` (removed), `Graph.add_edge` (edges were
  written into a copy of the adjacency list and lost), `Graph`'s heap helpers (now a `_Heap`
  class holding its array in a field) and `Num.hanoi` (accumulator is now an explicit stack;
  `hanoi_into` is gone because a by-value parameter cannot accumulate).
* `Test.check()` now fails the process, including failures from earlier suites - previously a
  failing example still exited 0, which is how the above stayed hidden.
* `examples/collections.ant`'s `group_by` mutated a method result (a copy); it writes back now.
* Stack traces from functions that came from a `use`d module now name the module file instead of
  the program that imported it (`at kth (lib/seq.mod:442)`).
* `Text.pad_left` / `Text.pad_right` count terminal columns, so CJK tables line up
  (`Text.width` is the new helper); `examples/perf.ant` no longer prints a boolean as the
  interpreter version.
* `lib/seq.mod` defined `upper_bound` twice (the native wrapper and the script reference version);
  the script one is now `upper_bound_script`, matching `lower_bound_script` and the documented API.
* The IDE's syntax highlighter ignored a closing `]-` that started a line, so everything after a
  multi-line `-[ ... ]-` header was painted as a comment; `annota studio <file> --check-highlight`
  now guards this in CI.

### Changed

* **Direct calls.**  A call to a top-level function or a `[[static]]` class method that the program
  defines is now compiled to `OP_CALL_DIRECT <name> <argc>`: there is no callee value and no
  argument list on the heap, and the VM binds the arguments straight out of the operand stack
  (a class qualified call passes the class as `this`, so the method body resolves its own bare
  calls as before).  The 40k-call benchmark row went from 23 ms to **11 ms**, and every library
  wrapper benefits.  Names that could be shadowed (a local/upvalue of the same name, a
  reassignment anywhere in the program, a method of the enclosing class) keep the generic path,
  and a name that no longer holds a function falls back at run time.
* **The machine-code backend can call other compiled functions.**  The JIT emits a native `call`
  for a direct call site when the callee has machine code, takes exactly these arguments as plain
  integers, is not a method and has a statically known result kind: the arguments are marshalled
  straight out of the virtual registers and the callee's `L[]`/`JitOut` live on the caller's native
  frame, so recursion works.  Resolving a callee compiles it on demand (a busy flag breaks cycles).
  In practice this only fires for callees whose result kind does not depend on the argument kind -
  a function such as `f(x) = x + 1` still needs the interpreter's dynamic kind, which is the next
  thing to solve.
* `jitLocalsOk` now refuses declared integer widths (`int8`/`int32`/...): the native code works on
  raw int64s and would not wrap like the interpreter, so those values keep being interpreted.

* **Superinstructions are no longer exclusive to `[[jit]]`.**  Every function now gets the
  semantics-preserving fusions (`x = x + k`, `x -= k`, `x = x + y`), so the *interpreter* executes
  fewer dispatches even where the machine-code backend cannot help; `[[jit]]` additionally enables
  the compare-and-branch pair and compiles the function eagerly.  `ANNOTA_FUSE` masks the
  individual patterns for debugging.
* `a[i][j]` compiles to a single `get_index2` / `set_index2` instruction: for arrays the element is
  computed straight from the strides instead of materialising the intermediate row view (which
  copies the whole `Obj`), with exactly the same bounds checks and error text.
* **Two unsound fusions were removed.**  The compare-and-branch pair patched its jump through
  `frames.back().ip`, which still pointed at the *start* of the instruction (a wrong target for
  every non-integer comparison), and the `arr[idx] += k` fusion ignored the two values that the
  target's own `GET_LOCAL` pair leaves on the stack (and never checked that the read and the write
  use the same slots) - applied to every function it corrupted the stack.  Both are fixed or
  withdrawn, which is why the fusions above are enabled per pattern instead of wholesale.

* Faster calls: frames reuse pooled local cells (captured cells are detached), parameter
  presence is a bitmask instead of a vector, immutable values are shared instead of deep-copied
  per argument, and the empty named-argument map is shared.  A 40k-call loop went from ~1250 ns
  to ~550 ns per call.
* Faster dispatch and operand stack: integer operands are combined in place (no temporaries),
  `pop` moves instead of copying, and branches on the fused comparison opcodes avoid a dispatch.

* **A much larger, faster standard library.**  `seq` gained proper algorithms - stable merge sort,
  heap sort, binary search (`lower_bound` / `upper_bound` / `bsearch`), quickselect
  (`kth` / `median` / `quantile`), merge/insert helpers, set algebra and a binary `Heap` - so the
  old insertion sort and O(n^2) dedup are gone.  `dict` is now a real hash table (open addressing,
  FNV-1a, resize at 0.75 and shrink at 0.125) with `Set` and `Counter` on top, replacing the linear
  scan.  Five new modules: `text` (KMP search, Levenshtein, LCS, CSV, Base64, case conversion),
  `numeric` (sieve, factorisation, totient, fast modular exponentiation, fast-doubling Fibonacci,
  bases, generic bisection), `matrix` (flat storage, O(n^3) multiply, Gaussian determinant, fast
  power), `stats` (quantiles, correlation, rank, histogram, summary), `graph` (BFS/DFS, heap
  Dijkstra, Kahn topological sort, union-find, Kruskal MST, Floyd-Warshall) and `geometry`
  (monotone-chain convex hull, divide-and-conquer closest pair, ray casting, shoelace area).
  Full API: [docs/stdlib.md](docs/stdlib.md).
* **`use` is now dependable**: `use sub/name` (path specs, `.mod` optional) works, a module that
  cannot be found is an error that lists the searched directories, the available modules and the
  closest name ("did you mean 'seq'?"), the CLI and the analyzer share one resolver
  (`src/module_loader.hpp`), and globals declared at a module's top level are visible to the
  analyzer - previously they were analysed as "undefined" even though the program ran.
* **New examples** (all part of `build.ps1 -Verify`, 17 examples total): `perf.ant` (12 workloads
  with a timing table: `[[jit]]` 162 -> 67 ns/iter, quickselect median ~20x faster than sorting,
  fixed arrays ~24x faster than list push), `collections.ant`, `strings.ant`, `graphs.ant`,
  `numerics.ant`, `geometry.ant`, plus rewritten `stdlib.ant` (69 assertions),
  `algorithms.ant` (82 assertions) and the new `docs/stdlib.md` reference.

* `lib/file.mod` exposes its text helper as **`FileText`** instead of `Text`, because the new
  `text` module owns the name `Text`; `use std` (which loads both) would otherwise shadow one of
  them.  Update `Text.to_lines` / `Text.from_lines` / `Text.count_lines` accordingly.

### Fixed

* **`[[jit]]` fusion wrote a truncated instruction**: the operand byte of the fused `SET_LOCAL`
  was not padded with `NOP`, so the bytecode walked off the instruction and the VM corrupted its
  stack (a plain `[[jit]]` loop could crash).  The disassembler now also bounds-checks its operand
  and constant reads instead of crashing on a malformed chunk.
* **The analyzer treated a call through a variable as "never read"**, so every higher order
  function reported its callback parameter as `unused-variable`; calling an uninitialised
  callable is now reported properly as well.

### Added

* **`elif`**: `elif cond( ... )` is the same construct as `else if cond( ... )`, and `) else` /
  `) elif` may start the next line, so a chain reads one branch per line.
* **Case analysis in the analyzer**: the then path assumes the condition and the else path assumes
  its negation, both learn symbolic facts, a branch that ends in `=value` / `throw` / `break` is
  excluded from the join (`if n < 2( =n )` therefore proves `n >= 2` afterwards), and every
  diagnostic carries the branch it was found in ("分支: 条件成立（i < 1）").
* **Stronger CAS**: linear bounds turn into interval reasoning (`i < n` with `n <= 1000` proves
  `i + 1 <= n`, `2 * i < 2 * n`, `i <= 999`), constant bounds are folded back into the abstract
  ranges so overflow and index checks use them, and comparisons are canonicalised so `A || !A`
  and `A && !A` are recognised as a tautology and a contradiction.
* **The IDE recognises the built-in types**: hover on `int` / `long` / `longlong` / `double` ...
  describes a type (width, signedness, declaration and conversion) instead of calling it a
  function, completion offers them as `type` items, and the editor highlighter takes the names
  from `builtinTypes()` - the same registry the parser and the analyzer use.

### Added

* `docs/syntax.md` (语法规范) and `docs/style.md` (代码规范): a full grammar reference — literals,
  operators with precedence, every statement form, classes, views, annotations, modules and macros —
  and a style guide covering file layout, naming, annotation use, error handling, the standard
  library, testing and a pre-commit checklist.

* **System primitives, and a standard library built on them.** C++ now exposes only `_sys_*`
  thunks — clock, broken-down time, environment, shell, worker threads, sockets — while the
  feature surface lives in `lib/` as Annota modules: `lib/time.mod` (`Time`, `Stopwatch`),
  `lib/os.mod` (`Os`), `lib/thread.mod` (`Thread`, `Task`, `Pool`, `parallel_map`) and
  `lib/net.mod` (`Net`, `Tcp`, `Url`, `Response`, HTTP parsing). Adding a capability normally
  means adding a `.mod` file instead of rebuilding the interpreter. The `system` facade mirrors
  every primitive (`system.clock()`, `system.call("clock")`, `system.help()`,
  `system.primitives()`), and clock access stays available without `use` as `time.now()` /
  `time.millis()` / `time.clock()` / `time.sleep(ms)`.
* `examples/system.ant`: 53 assertions covering the clock and date formatting, environment and
  shell execution, threads (`spawn`, `join`, `parallel_map`, pool) and a loopback TCP echo
  server plus HTTP response parsing — no external network required. Part of `build.ps1 -Verify`.
* IDE: **`F7` previews a program's own `view` window** (`Ctrl+F7` prints the component tree), so
  graphical programs can be developed inside `annota studio` rather than only from the CLI.
  `annota studio <file> --preview` drives the same path head-lessly for CI.
* `CONTRIBUTING.md`, `CHANGELOG.md`, and a generated reference manual (`docs/reference.md`)
  produced by `annota ide docs`.
* Bilingual documentation: `README.md` (English) and `README_zh.md` (Chinese), cross-linked, with
  screenshots and a documentation link check (`tools/doc_links.ps1`) wired into
  `build.ps1 -Verify`.

### Added

* **C style numeric widths** (`src/value.hpp` `NumKind`): `int8/16/32/64`, `uint8/16/32/64`,
  `float32/64` with the aliases `byte/short/int/long/uint/ulong/ubyte/ushort/i8..u64/f32/f64`.
  A width is applied when it is **declared** (`new a:int8 = 200` → `-56`, parameters and
  `state` too) or converted (`uint8(300)` → `44`). Arithmetic follows C usual arithmetic
  conversions (a narrow width promotes to the wider kind when mixed with an untyped number),
  unsigned widths print their full range, `float32` rounds through single precision, and
  `typeof` reports the width (`typeof(int8(1))` == `"int8"`). Untyped integers stay 64 bit, so
  existing programs and tests are unaffected.
* **`longlong` / `ulonglong` (128 bit) and `longdouble` (80 bit)** are boxed scalars
  (`VT::Wide` / `VT::LongDouble`), so arithmetic is exact - `longlong("170141183460469231731687303715884105727") + longlong(1)`
  wraps to `-2^127`, `ulonglong(0) - ulonglong(1)` prints `2^128-1`, `int(longlong("9007199254740993"))`
  stays exact, and `longdouble(1) / longdouble(3)` keeps 18 significant digits. They work as
  declarations (`new x:longlong = ...`, parameters, `state`) and as conversions
  (`longlong` / `ulonglong` / `longdouble`, aliases `int128` / `uint128` / `i128` / `u128` / `ld`).
  A bare literal is still parsed as 64 bit, so huge constants are written as
  `longlong("...")`.
* `annota bench` now measures interpreter throughput (int / call / list / float loops) so the
  speed work has a baseline: int loop ~280 ns/iter, list loop ~190 ns/iter, float loop ~290
  ns/iter, and function calls ~1.45 us/call - the call path is the first optimisation target.

### Added

* **Fixed size typed arrays**: `int[5]`, `int[3][4]`, `int[]`, `string[3]` ... A new `VT::Array` keeps
  its elements in one shared buffer, so a row of `int[3][4]` is an O(1) view and `m[1][2] = 7` writes
  through to the parent. Elements auto-pad with the declared element type's zero value, assignment
  and argument passing deep copy, `len` reports the first dimension and `for` iterates it. The
  analyzer knows the static length, so `a[5]` on `int[3]` is reported as `out-of-bounds` before the
  program runs, and the compiler drops the runtime check when the index is a proved literal or when
  the statement carries the existing `[[unsafe]]` hint (no second marker was introduced).
* **128 bit literals**: a decimal, `0x`, `0o` or `0b` literal that does not fit in 64 bits becomes an
  `int128` value instead of being clamped.
* **`[[jit]]`**: the single marker that asks for load-time optimisation of a function. The pass is
  deliberately small and safe - it fuses `x = x + k`, `x = x - k` and `x = x + y` into one
  instruction and pads the freed slots with `OP_NOP`, so no jump target moves. Measured with
  `annota bench`: the same loop runs 171 ns/iter without the marker and 136 ns/iter with it.

### Added

* **A small CAS in the analyzer**: expressions are normalised to integer linear forms
  (`sum(k_i * v_i) + c`), so conditions such as `i + 1 > i`, `2 * k == k + k`, `m - m == 0` or
  `n - 1 < n` are *proved* instead of reported as "cannot prove the assertion", and
  `[[assume: x == 5]]` becomes a substitution that makes `[[assert: x * 2 == 10]]` provable.
  It also strengthens `redundant-condition` (`if a + 1 > a` is flagged as always true) and the
  static proof of array indices. Only exact linear forms are decided, everything else still goes
  through the abstract domain, so no false positives were introduced.
* **The IDE now knows the built-in types**: hover on `int` / `long` / `longlong` / `double` ...
  describes a *type* (width, signedness, how to declare and convert) instead of calling it a
  function, and completion offers them as `type` items. The names come from one registry
  (`builtinTypes()`), which the parser, the analyzer and the tooling all share.

### Changed

* **Fewer type aliases**: one spelling per type - `int`, `long`, `longlong`, `uint`, `ulong`,
  `ulonglong`, `int8/16/32/64`, `uint8/16/32/64`, `float` (32 bit), `double` (64 bit),
  `longdouble`. The `i8`/`u64`/`f32`/`byte`/`short`/`int128`/`uint128`/`ld` style aliases are gone;
  the conversion functions keep the same names (`float32`/`float64` remain available as
  conversions when an explicit width is wanted).

* **Integer division now follows C**: `int / int` truncates toward zero (`7 / 2` == `3`,
  `-7 / 2` == `-3`) and only a float operand makes it a float division (`7 / 2.0` == `3.5`).
  This is a **breaking change**: `lib/mathx.mod` (`mean`, `median`, `variance`, `round_to`) now
  converts explicitly, and the docs/assertions were updated. It also removes a float promotion
  from the hot path.
* **Comma lists work for every declaration**, not just `new`: `const a = 1, b = 2`,
  `state a = 0, b = 1`, `input a, b`, and comma lists inside class bodies.

* **Syntax: less rigid, closer to what Python users expect.** `new a, b` declares several
  variables at once (`new x = 1, y = 2`), a comma also separates statements on one line
  (`a = 1, b = 2`), `del a, b` deletes several names, a line ending in a binary operator
  continues on the next line (`1 +` then `2`) and `\` joins explicitly.
* **`__init__` is the constructor body.** It takes empty parentheses, runs during construction and
  sees the class parameters as locals; declared field defaults are applied first so that `__init__`
  can override them. Giving `__init__` parameters is a compile error with a hint.
* **Variadic parameters** accept Python style `*args` as well as `...args`; both collect a `List`.
  `**kwargs` now reports a clear error instead of a confusing syntax error.
* **Slicing keeps the type**: a string slice returns a `String` (it used to return a character
  `SliceView`), while list and tuple slices stay read-only `SliceView`s.

### Fixed

* Linux: `_sys_env_all()` referenced `annota::environ` (the declaration sat inside the namespace)
  rather than the global C `environ`, so linking failed with `undefined reference to
  'annota::environ'`. Every POSIX build was affected.
* CI: the headless IDE checks now run with `QT_QPA_PLATFORM=offscreen`, since a CI runner has no
  interactive desktop and `annota studio` could not create a window there (`--shot`, `--preview`
  and the GUI input check all failed).
* Analyzer: a method call (`x.method()`) did not count as a use of `x`, so receivers and class
  parameters used only through method calls were reported as unused.
* `examples/syntax.ant`: 31 assertions over the syntax above, wired into `build.ps1 -Verify`.
## [1.0.0] - 2026-10-04

First public release: the language, the standard library, the analysis layer and the tooling
around them.

### Added — language

* Lexer, recursive-descent parser, compile-time macro engine, annotation parser and module loader.
* Bytecode compiler and stack VM with deep-copy value semantics, closures with shared captures,
  classes with inheritance and magic methods (`__len__`, `__str__`, `__call__`, `__get__`,
  `__set__`, `__iter__`), slicing, piecewise expressions, `Ok`/`Err`, optional / named / variadic
  parameters, and `except` error handling.
* Declarative `view` components with reactive `state`, rendered either to a native Qt window or to
  a text tree, plus `--gui-shot` for headless rendering.

### Added — tooling

* **REPL** (`annota`, `annota repl`) with a persistent VM, multi-line input, and session analysis
  (`:analyze`).
* **Graphical IDE** (`annota studio`, Qt 6): syntax highlighting, diagnostic squiggles, problems /
  structure / coverage panels, hover documentation, quick fixes, run panel, generated annotation
  and check manuals, and a headless `--shot` mode for CI.
* **LSP server** (`annota lsp`) over stdio: incremental keystroke diagnostics, save-time analysis,
  a cancellable background pass, hover, definition, rename and completion.
* **Static analysis** in three latency budgets (50 ms / 500 ms / 5 s) with 33 diagnostic codes,
  abstract interpretation over a CFG, contract checking at call sites, and `[[ignore]]`
  suppression management.

### Added — standard library

* Modules: `slice`, `seq`, `str`, `dict`, `mathx`, `test`, `file`, and the `std` umbrella.
* Native modules: `math`, `io`, `json`, `time` (and a `net` stub).
* Low-level `_`-prefixed file, directory, path and stdin primitives, wrapped by the `File` / `Dir`
  / `Path` / `Text` / `Result` API with three error-handling styles.

### Added — quality

* `build.ps1 -Verify`: 8 example programs, 12 analyzer fixtures, a false-positive guard over the
  examples, `annota bench`, an LSP end-to-end smoke test and a headless IDE render.
* Self-checking examples: `selfcheck.ant` (72 assertions) and `files.ant` (39 assertions).
* JSON diagnostics report for editor and CI integration.

### Changed

* `to` and `step` became soft keywords, so they are usable as ordinary parameter names
  (`File.copy(from, to)`).
* Diagnostics that come from a `use`d module report that module's own file name under `--modules`.

### Fixed

* GUI: `input` did not work inside a window or in the IDE — it waited for a console that a GUI
  program usually does not have, and returned an empty string while output was being captured.
  A Qt build now asks with an input dialog, in `--gui` and in `annota studio` alike, and
  `ANNOTA_INPUT` pre-answers automated runs. `_stdin_line()` follows the same rule.
* Analyzer: untyped class fields were invisible inside methods (false "undeclared variable").
* Analyzer: `len(x) - k` produced spurious overflow warnings; lengths are now bounded integers.
* Analyzer: iterating a list bound the loop variable as `int`, which produced false type errors.
* Analyzer: suppression via `[[ignore]]` now also applies to reports emitted after the statement
  (unused variables, dead stores).
* Analyzer: unreachable-code and dead-store checks no longer report the same site twice.
* Build: the Qt auto-detection loop clobbered the project root, so verification looked for test
  files inside the Qt installation.
* `lib/dict.mod` was missing its `use seq` dependency.

[Unreleased]: https://github.com/Melba0/annota/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Melba0/annota/releases/tag/v1.0.0
