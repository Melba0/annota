# Changelog

All notable changes to this project are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project aims
to follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html): the **language** and the
**diagnostic codes** are the public API, so a change that makes previously valid code invalid is a
major change, and a new check is a minor one.

## [Unreleased]

### Added

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

### Changed

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
