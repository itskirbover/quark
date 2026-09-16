#pragma once
// Key decoding: UTF-8 text, navigation keys, Ctrl-C / Ctrl-S.

#include <cstdint>
#include <string>

struct Key {
    enum class Type {
        Char,  // printable text (utf8 may hold multiple bytes)
        Enter,
        Backspace,
        Delete,
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
        Esc,
        None,  // timeout / unknown
    };

    Type type = Type::None;
    std::string text;  // valid for Char
    uint32_t cp = 0;   // valid for Char

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
};
