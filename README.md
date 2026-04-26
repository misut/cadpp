# cad++

DWG viewer for Android (and macOS for debugging), built on
[phenotype](https://github.com/misut/phenotype). Reads `.dwg` files via
[LibreDWG](https://www.gnu.org/software/libredwg/) and renders the 2D
entities to a hardware-accelerated canvas.

The repository is named `cadpp` because GitHub repository names cannot
contain `+`. The product, binary, and window title are `cad++`.

## Status

**Pre-alpha — M1 bootstrap.** Opens a placeholder window. File picker
and entity rendering arrive in subsequent milestones.

Roadmap (v0.1.0):

- M1 — repository bootstrap (this milestone)
- M2 — LibreDWG parse-only smoke test
- M3 — minimal renderer (lines)
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

A 900×700 window with a "cad++" placeholder appears.

## License

GPLv3 — see [LICENSE](LICENSE).

cad++ inherits GPLv3 from its dependency on LibreDWG (also GPLv3+).
The `phenotype` UI framework, the `exon` build system, and other
upstream libraries (`cppx`, `txn`, `jsoncpp`) remain MIT and are
linked into cad++ under their respective licenses.
