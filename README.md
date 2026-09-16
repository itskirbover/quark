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
| `##` | 2 | `s=2:n=3:d=4:v=2` + packed `w` | ~1.5×, tight, centered |
| `###` | 2 | `s=2:n=1:d=2:v=2` + packed `w` | 1×, tight, centered |
| `####` / `#####` / `######` | 1 | none | normal size, bold |

All levels are SGR bold with no color. The `#` marker renders inline at
the same size as its text. Fractional H2/H3 pack characters into explicit-`w`
groups so the advance matches the shrunken glyphs (no letter-spacing gaps).

## Concealed markers

Inline markers (`**`, `*`, `_`, `~~`, backticks, `[`…`](url)`) are hidden
except inside the formatted span under the cursor — move the cursor into
`*text*` and its markers reappear for editing. Header `#` markers are hidden
unless the cursor is on that line. Block markers (`>`, `-`, `1.`) stay
dimmed everywhere. Inline code renders bright-on-black.
Fenced code blocks render as normal body text (only the ```` ``` ````
delimiters stay dimmed).

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
| Left click | Move cursor (needs a mouse-reporting terminal like kitty) |
| Mouse wheel | Move cursor ±3 lines |
| Shift+click | Terminal text selection (bypasses the app) |
| Ctrl-S | Save |
| Ctrl-C | Quit — always asks `Save …? (y/n)`; `y` saves + exits, `n` exits, `Esc` cancels |

## Status bar

LazyVim-inspired blocks: filename on the left, kitty support tag, scroll
position (`Top`/`%`/`Bot`), cursor position and clock on the right. Narrow
windows shed the clock, then the scroll %, then the hints — never the
position.
