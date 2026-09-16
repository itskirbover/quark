# AGENTS.md – quark

C++17 Markdown TUI editor (`./build/quark <file.md>`). No deps, no tests, no lint/CI.

## Build (out-of-source only)

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/quark test/sample.md

- Root already contains ignored in-source CMake artifacts — never build in-source, don't commit them.
- `-Wall -Wextra -Wpedantic` is on: a warning-free build is the check. No test suite; smoke-test with `test/sample.md`.

## Source map (only these build)

`CMakeLists.txt` compiles 8 files in `src/`: `main, Terminal, Input, Buffer, Markdown, Kitty, Renderer, Editor` (+ `Utf8.hpp` header-only).
- `src/term/`, `src/md/`, `src/ui/` are untracked/empty experiments — ignore.
- Flow: `main → Editor::run` (loop/cursor/save-prompt) → `Renderer::draw` (Markdown styling + Kitty sizing + status bar) on `Terminal` (raw/alt-screen/mouse).

## Conventions agents miss

- Cursor is byte offset (`cx`) + line index (`cy`); always clamp via `Buffer::clampToChar`, never split UTF-8 mid-sequence.
- Offsets/slicing are bytes everywhere (`Markdown::Span`, `Renderer::Segment`/`LineLayout`); display width via `utf8::` helpers in `src/Utf8.hpp`.
- Kitty `sized()` text ≤ 4096 bytes; SGR carries bold/color, never the OSC escape. `detectSupport` needs a real TTY in raw mode — won't work piped.
- `Buffer::open` on missing file = new buffer; preserves trailing-newline on round-trip.
