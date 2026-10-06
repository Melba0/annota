## What does this change?

<!-- A short description, and the motivation. Link the issue if there is one. -->

## Which area?

- [ ] language (lexer / parser / compiler / VM)
- [ ] standard library (`lib/`)
- [ ] native code (`native/`, `plugins/`, FFI)
- [ ] static analysis (new check, precision, diagnostics)
- [ ] IDE / LSP / CLI
- [ ] documentation
- [ ] build and packaging

## How was it verified?

<!-- Paste the relevant part of `build.ps1 -Verify`, or explain the manual check. -->

```
powershell -ExecutionPolicy Bypass -File build.ps1 -Verify
```

## Checklist

- [ ] `build.ps1 -Verify` passes
- [ ] new behaviour is covered by an example assertion or an `examples/analysis/` fixture
- [ ] `README.md` and `README_zh.md` updated if user visible
- [ ] `CHANGELOG.md` updated under *Unreleased*
- [ ] `docs/reference.md` regenerated if annotations / codes / primitives / subcommands changed
      (`build\annota.exe ide docs --out docs/reference.md`)
- [ ] hand-written docs updated and links checked (`tools/doc_links.ps1`) if files moved
- [ ] for a plugin or native kernel: `annota plugin build` (or the `native/` link) exercised, and
      the example that `use`s it still passes
