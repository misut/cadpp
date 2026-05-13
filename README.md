# cad++

DWG viewer for Android (and macOS for debugging), built on
[phenotype](https://github.com/misut/phenotype). Reads `.dwg` files via
[LibreDWG](https://www.gnu.org/software/libredwg/) and renders the 2D
entities to a hardware-accelerated canvas.

The repository is named `cadpp` because GitHub repository names cannot
contain `+`. The product, binary, and window title are `cad++`.

## Supported platforms

- **Android** (`aarch64-linux-android`, Vulkan via phenotype's Android
  backend) — production target.
- **macOS** (`aarch64-apple-darwin`, Metal via phenotype's macOS
  backend) — debug / preview surface used during development.

Other targets (Windows, Linux desktop, iOS, web) are explicitly out of
scope. The repository's manifests (`exon.toml`, `native/exon.toml`,
`test/exon.toml`) only list the two supported `(os, arch)` pairs above.

## Status

**Pre-alpha — M5 text entities.** Opens a window that loads a
hardcoded sample DWG via LibreDWG, extracts `LINE` / `CIRCLE` /
`ARC` / `LWPOLYLINE` / `TEXT` / `MTEXT` entities, tessellates
curves into chord segments at parse time, and renders the whole
thing (lines + text) through phenotype's `widget::canvas`
immediate-mode painter.

Roadmap (v0.1.0):

- M1 — repository bootstrap ✅
- M2 — LibreDWG parse-only smoke test ✅
- M3 — minimal renderer (lines) ✅
- M4 — circles, arcs, polylines (segment decomposition) ✅
- M5 — text entities ✅
- M6 — Android port + file picker on both platforms
- M4 — circles, arcs, polylines (segment decomposition)
- M5 — text entities
- M6 — Android port + file picker on both platforms
- M7 — release v0.1.0

## Quick start (macOS)

```sh
cd cadpp
mise install
mise exec -- intron install
mise exec -- exon run
```

A 900×800 window opens, parses `test/fixtures/sample_2000.dwg` and
draws its lines / circles / polylines / text in a fixed canvas.

Pass a path to override the bundled sample:

```sh
.exon/debug/cadpp /path/to/your-drawing.dwg
```

## License

GPLv3 — see [LICENSE](LICENSE).

cad++ inherits GPLv3 from its dependency on LibreDWG (also GPLv3+).
The `phenotype` UI framework, the `exon` build system, and other
upstream libraries (`cppx`, `txn`, `jsoncpp`) remain MIT and are
linked into cad++ under their respective licenses.

## Bundled fonts

cad++ ships four SIL OFL 1.1 fonts under [`assets/fonts/`](assets/fonts)
so AutoCAD-shipped typefaces (`TIMES.TTF`, CityBlueprint, …) render on
hosts that don't have them installed:

- **Liberation Serif** — Times Roman substitute. © 2010 Google Corporation;
  © 2012 Red Hat, Inc.
- **Architects Daughter** — CityBlueprint substitute. © 2010 Kimberly Geswein.
- **Source Sans 3** — sans-serif fallback. © 2010-2022 Adobe.
- **JetBrains Mono** — monospace fallback. © 2020 JetBrains Mono Project Authors.

Full OFL 1.1 text, per-font copyright headers, and reproduction commands
live in [`assets/fonts/LICENSE.txt`](assets/fonts/LICENSE.txt) and
[`assets/fonts/README.md`](assets/fonts/README.md).
