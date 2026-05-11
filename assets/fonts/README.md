# cad++ bundled fonts

Four SIL OFL 1.1 fonts shipped alongside `cadpp-native` so a fresh macOS
install can render AutoCAD-shipped `TIMES.TTF` / `cityb___.ttf` (and
generic Arial / Helvetica / Courier / Consolas references) without the
user hunting down a copy. The bundled basenames are deliberate —
`src/fonts.cpp` hardcodes them in `register_bundled_fonts`.

| Bundled basename | Upstream | Tag / version |
|---|---|---|
| `LiberationSerif-Regular.ttf` | <https://github.com/liberationfonts/liberation-fonts> | 2.1.5 |
| `ArchitectsDaughter-Regular.ttf` | <https://github.com/google/fonts/tree/main/ofl/architectsdaughter> | 1.003 (Kimberly Geswein) |
| `SourceSans3-Regular.ttf` | <https://github.com/adobe-fonts/source-sans> | 3.052 |
| `JetBrainsMono-Regular.ttf` | <https://github.com/JetBrains/JetBrainsMono> | 2.304 |

Attribution and the OFL 1.1 body live at [LICENSE.txt](LICENSE.txt).

## Reproducing the bundle

Run from the repository root:

```sh
mkdir -p assets/fonts

# Architects Daughter
curl -sSfL 'https://github.com/google/fonts/raw/main/ofl/architectsdaughter/ArchitectsDaughter-Regular.ttf' \
    -o assets/fonts/ArchitectsDaughter-Regular.ttf

# Source Sans 3
curl -sSfL 'https://github.com/adobe-fonts/source-sans/raw/release/TTF/SourceSans3-Regular.ttf' \
    -o assets/fonts/SourceSans3-Regular.ttf

# JetBrains Mono
curl -sSfL 'https://github.com/JetBrains/JetBrainsMono/raw/v2.304/fonts/ttf/JetBrainsMono-Regular.ttf' \
    -o assets/fonts/JetBrainsMono-Regular.ttf

# Liberation Serif (tarball — extract the one file)
curl -sSfL 'https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz' \
    -o /tmp/liberation.tar.gz
tar -xzf /tmp/liberation.tar.gz -C /tmp
cp /tmp/liberation-fonts-ttf-2.1.5/LiberationSerif-Regular.ttf assets/fonts/
rm -rf /tmp/liberation.tar.gz /tmp/liberation-fonts-ttf-2.1.5
```

Only the Regular weight is committed. Bold / Italic are synthesised by
CoreText (`CTFontCreateCopyWithSymbolicTraits`); add the real upstream
weights only if synthesis looks visually off on a real DWG.

Do not modify the bundled `.ttf` files in place — when bumping a font,
replace the file with the upstream release verbatim and update the
version row in the table above.
