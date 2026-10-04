## What does this change?

<!-- A short description, and the motivation. Link the issue if there is one. -->

## Which area?

- [ ] language (lexer / parser / compiler / VM)
- [ ] standard library (`lib/`)
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
- [ ] `docs/reference.md` regenerated if annotations / codes / file primitives changed
      (`build\annota.exe ide docs --out docs/reference.md`)
