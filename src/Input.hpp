#pragma once
// Key decoding: UTF-8 text, navigation keys (with Shift modifiers),
// word-deletion keys, Ctrl-C / Ctrl-S / Ctrl-Z / Ctrl-Y.

#include <cstdint>
#include <string>

struct Key {
    enum class Type {
        Char,  // printable text (utf8 may hold multiple bytes)
        Enter,
        Backspace,
        CtrlBackspace,  // word-delete backward (0x08 or Ctrl+Backspace seq)
        Delete,
        CtrlDelete,  // word-delete forward (Ctrl+Delete seq)
        ArrowUp,
        ArrowDown,
        ArrowLeft,
        ArrowRight,
        Home,
        End,
        PageUp,
        PageDown,
        CtrlC,
        CtrlS,
        Copy,
        Paste,
        CtrlZ,  // undo
        CtrlY,  // redo
        Esc,
        MousePress,  // left button press (mouseCol/Row: 1-based screen)
        WheelUp,
        WheelDown,
        None,  // timeout / unknown
    };

    Type type = Type::None;
    std::string text;  // valid for Char/Paste
    uint32_t cp = 0;   // valid for Char
    int mouseCol = 0;  // valid for MousePress (1-based screen column)
    int mouseRow = 0;  // valid for MousePress (1-based screen row)
    // Shift modifier for navigation keys (Shift+arrows/Home/End/PgUp/PgDn).
    bool shift = false;

    static Key make(Type t) {
        Key k;
        k.type = t;
        return k;
    }
};

class InputReader {
public:
    // Blocking read of one logical key.
    Key readKey();
    // Like readKey(), but returns Key::Type::None if no key arrives
    // within timeoutMs (lets callers poll, e.g. for window resizes).
    Key readKeyTimeout(int timeoutMs);
};
