# Contributing to Annota

Thanks for taking the time to contribute! This document explains how to build, test and extend
the project so that a pull request can be reviewed quickly.

- [Ground rules](#ground-rules)
- [Development setup](#development-setup)
- [Running the checks](#running-the-checks)
- [Project conventions](#project-conventions)
- [Adding an analyzer check](#adding-an-analyzer-check)
- [Adding an annotation](#adding-an-annotation)
- [Adding a standard library function](#adding-a-standard-library-function)
- [Adding native code (FFI, kernels and plugins)](#adding-native-code-ffi-kernels-and-plugins)
- [Documentation](#documentation)
- [Commit and pull request style](#commit-and-pull-request-style)

## Ground rules

* Be kind and constructive; assume good faith.
* One logical change per pull request. Refactors and behaviour changes belong in separate PRs.
* Every PR must keep `build.ps1 -Verify` green.
* New behaviour needs a test: an example assertion (`examples/*.ant`), an analyzer fixture
  (`examples/analysis/*.ant`) or an LSP smoke case.

## Development setup

```powershell
git clone https://github.com/Melba0/annota.git
cd annota

# fastest loop: no Qt, builds in a few seconds
powershell -ExecutionPolicy Bypass -File build.ps1 -NoQt

# full build with the window and the IDE
powershell -ExecutionPolicy Bypass -File build.ps1
```

Requirements: a C++17 compiler (MinGW-w64 g++ 13+ or MSVC 2022) and, for the GUI, Qt 6 Widgets.

Debug build with symbols:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Config debug
```

The project has **no external dependencies beyond Qt**: the JSON reader/writer, the analyzer and
the IDE are all part of the tree. Please keep it that way.

## Running the checks

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify     # everything
```

CI runs the same script on Windows (with Qt 6) and a no-Qt Linux build. Two environment notes:

* A CI runner has **no interactive desktop**, so the IDE checks need Qt's offscreen platform.
  `build.ps1` deploys `platforms/qoffscreen.dll` next to the executable and sets
  `QT_QPA_PLATFORM=offscreen` around `annota studio` by itself; set it
  manually if you drive `studio` from another script:
  ```powershell
  $env:QT_QPA_PLATFORM = "offscreen"
  build\annota.exe studio examples\files.ant --run --shot out.png
  ```
* Headless runs of a program that calls `input` need `ANNOTA_INPUT=<text>` (see the README FAQ).

On Linux, build with CMake (`cmake -S . -B build && cmake --build build`); the runtime links
`ws2_32` on Windows and plain POSIX sockets elsewhere, and no other dependency is allowed.

Individual pieces while iterating:

```powershell
build\annota.exe examples\selfcheck.ant                # language semantics (78 assertions)
build\annota.exe examples\files.ant                    # file API (39 assertions)
build\annota.exe examples\jit.ant                      # manual and automatic JIT (12 assertions)
build\annota.exe examples\plugin.ant                   # loads the plugin built below
build\annota.exe analyze-suite examples\analysis       # analyzer fixtures
build\annota.exe analyze <file> --json                 # one file, machine readable
build\annota.exe bench                                 # performance budgets
powershell -ExecutionPolicy Bypass -File tools/lsp_smoke.ps1
build\annota.exe studio <file> --shot out.png          # headless IDE render
build\annota.exe studio <file> --check-highlight       # no file ends inside a -[ ]- comment
```

The plugin round-trip from `-Verify`, step by step (this is the whole hot-plug story):

```powershell
build\annota.exe plugin build plugins\hello.cpp        # -> build\plugins\hello.dll
build\annota.exe examples\plugin.ant                   # `use hello` loads it
```

A new analyzer fixture must declare what it expects in its first line:

```
-[ expect: division-by-zero, out-of-bounds ]-        <- these codes must appear
-[ expect: none ]-                                   <- no error and no warning at all
```

Warnings and errors that are **not** listed make the fixture fail, so fixtures double as
false-positive guards.

## Project conventions

* C++17, no exceptions thrown across the analyzer (it reports, it does not throw).
* 4 spaces, no tabs; braces on the same line.
* Comments explain *why*; the code should explain *what*. Chinese and English comments are both
  fine — match the surrounding file.
* Source files are UTF-8. String literals that end up in diagnostics are Chinese; identifiers,
  codes and JSON keys are English.
* Keep the layering: `lexer → parser → ast → compiler → vm`, with the analyzer reading the AST
  and never re-implementing the front end.
* Anything that can be checked at analysis time should not be deferred to run time, and vice
  versa: report bounds errors in the analyzer, not as a runtime crash.

## Adding an analyzer check

1. Pick a stable code (kebab-case, e.g. `iterator-bounds`) and register it in
   `checkCodeRegistry()` in `src/analyzer.cpp`, together with a one-line summary in
   `checkCodeSummary()`.
2. Implement it in `src/analysis_flow.cpp`. L1 checks go through the `Layer1` walker in
   `analyzer.cpp`; L2/L3 checks emit through `col_.error/warn/info`.
3. Severity rule: `error` only when the analyzer can *prove* the violation, `warning` when it may
   happen, `info` for advice.
4. Always fill `detail` with the abstract values that led to the conclusion and `fix` with a
   concrete annotation the user can paste (the IDE turns `[[...]]` inside `fix` into a quick fix).
5. Add a fixture under `examples/analysis/` with the expected code, and make sure the existing
   examples stay clean:
   `build\annota.exe analyze-suite examples\analysis`.

## Adding an annotation

1. Add an `AnnotationSpec` to `annotationRegistry()` (name, category, summary, detail, argument
   count, allowed scopes) — the IDE help, completion and `ide docs` pick it up automatically.
2. Teach the analyzer what it means. Typical hooks: `Flow::stmt` (statement-level annotations),
   `extractFuncAnnotations` (function-level), `analyzeLoop` (loop-level).
3. Update `docs/reference.md` with `build\annota.exe ide docs --out docs/reference.md`.
4. Add an example to `examples/analysis/02_annotations.ant` (rejected usage) or
   `10_guarded.ant` (accepted usage).

## Adding a standard library function

Most features belong in `lib/*.mod` (Annota), not in C++. Only reach for C++ when the
operating system itself must be touched, and then add *one* primitive to `src/sys_api.cpp`
(or `src/builtins.cpp` for file/path primitives), document it in the registry table and wrap
it in a module - see `lib/os.mod` for the smallest complete example.

Library modules live in `lib/*.mod` and are written **in Annota**. Prefer composing existing
functions over adding C++ natives; only touch `src/builtins.cpp` for something the language cannot
express (file system access, time, and so on).

* Low-level primitives are `_`-prefixed, raise on error and are documented in
  `docs/reference.md`.
* High-level helpers go into the module (`lib/seq.mod`, `lib/file.mod`, …) and should provide a
  `try_*` variant when they can fail.
* Cover it with assertions in the matching example (`examples/stdlib.ant`, `examples/files.ant`).

## Adding native code (FFI, kernels and plugins)

The core stays small: **hot algorithms are linked in, not added to the interpreter**. Pick the
lightest option that works:

| Situation | Where it goes |
|---|---|
| A hot loop written in Annota | nowhere - the interpreter compiles hot integer loops to machine code by itself; `[[jit]]` only asks for load-time compilation |
| An algorithm kernel that the standard library calls | a new `native/*.cpp` registering a module through `src/ffi.hpp` (see `native/seq_native.cpp`); `build.ps1` links `native/*.cpp` automatically |
| Native ability that must work without rebuilding | a plugin source under `plugins/`, built with `annota plugin build` and loaded by `use` |
| A raw OS capability the language cannot express | one `_`-prefixed primitive in `src/sys_api.cpp` |

Rules for anything in `native/` or `plugins/`:

* Register through `src/ffi.hpp` - `ANNOTA_MODULE(name) { mod.fn("member", ...); } ANNOTA_END_MODULE`
  for a module, `ANNOTA_FUNCTION(name, fn)` for a global. Never add to `src/builtins.cpp` just to
  expose a helper.
* The interface is documented in [docs/ffi.md](docs/ffi.md): arguments arrive as
  `std::vector<Value>&`, `ffiArg`/`ffiItems`/`ffiList`/`ffiInt` convert, `vm.throwError(...)`
  reports, `vm.callSync(fn, args)` calls back into Annota.
* Keep the module self-checking: add assertions to an example (`examples/ffi.ant`,
  `examples/plugin.ant`) and, for a plugin, a line in the `-Verify` plugin step.
* Mind the value semantics the same way script code must: a native that mutates a container it did
  not create is a bug; return a new value instead (see [docs/syntax.md](docs/syntax.md) §4.1.1).
* `annota plugin build` needs the interpreter's import library, which `build.ps1` writes next to
  the executable (`libannota.dll.a`); the plugins search path is documented in
  [docs/tools.md](docs/tools.md).

## Documentation

* `README.md` (English) and `README_zh.md` (Chinese) are kept in sync; update both.
* `docs/reference.md` is **generated**: run `build\annota.exe ide docs --out docs/reference.md`
  after changing annotations, diagnostic codes, primitives or subcommands. Never edit it by hand.
* The hand-written guides are `docs/syntax.md` (language), `docs/stdlib.md` (library),
  `docs/tools.md` (build, CLI, environment variables, plugins, troubleshooting),
  `docs/jit.md` (optimisation) and `docs/ffi.md` (linking C++). Keep them in Chinese like the
  rest, and link them from both READMEs.
* Screenshots live in `docs/images/`. They are produced headlessly:
  `build\annota.exe studio <file> --run --shot docs/images/ide-problems.png`.
* After moving files around, run `powershell -ExecutionPolicy Bypass -File tools/doc_links.ps1`
  (also part of `-Verify`): a broken relative link fails CI.

## Commit and pull request style

* Commit messages: imperative subject under 72 characters, body explaining the *why* when it is
  not obvious. Example: `analyzer: report the origin module in --modules diagnostics`.
* PRs should describe: what changed, why, how it was verified (`build.ps1 -Verify` output), and
  any user-visible behaviour change (mention it in `CHANGELOG.md` under *Unreleased*).
* Keep the diff focused; unrelated formatting changes make review harder.

By contributing you agree that your contribution is licensed under the terms of the
[LICENSE](LICENSE) file of this repository.
