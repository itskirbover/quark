# quark
A basic Markdown editor made in C++.

Renders `#`–`######` headers at custom sizes via the
[Kitty text sizing protocol](https://sw.kovidgoyal.net/kitty/text-sizing-protocol/)
(`ESC ] 66 ; … ; text BEL`), with `**bold**`, `*italic*`,
`` `code` ``, `~~strike~~`, links, lists, quotes and fences styled with SGR.
Outside kitty it degrades to SGR-only styling (status bar shows `plain`).

## Header size ladder (kitty only)

| Level | Rows | Sizing | Look |
|-------|------|--------|------|
| `#` | 2 | `s=2` | 2× glyphs |
| `##` | 2 | `s=2:n=3:d=4:v=2` | ~1.5×, vertically centered |
| `###` | 2 | `s=2:n=1:d=2:v=2` | 1× with padding, centered |
| `####` / `#####` / `######` | 1 | none | normal size, bold + color |

All levels keep SGR bold + a per-level color.

## Concealed markers

Inline markers (`**`, `*`, `_`, `~~`, backticks, `[`…`](url)`) are hidden on
every line except the one under the cursor, which always shows raw source for
editing. Block markers (`#`, `>`, `-`, `1.`) stay dimmed everywhere. Inline
code renders bright-on-black.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Requires a C++17 compiler and CMake. No external dependencies.

## Usage

```sh
./build/quark <file.md>   # opens existing file or starts a new one
```

Try `test/sample.md` for headers, inline styles, emoji/CJK widths.

## Keys

| Key | Action |
|-----|--------|
| Arrows, Home/End, PgUp/PgDn | Move cursor |
| Type, Enter, Backspace, Delete | Edit |
| Ctrl-S | Save |
| Ctrl-C | Quit — always asks `Save …? (y/n)`; `y` saves + exits, `n` exits, `Esc` cancels |
