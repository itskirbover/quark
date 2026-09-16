#pragma once
// Kitty text sizing protocol helpers (https://sw.kovidgoyal.net/kitty/text-sizing-protocol/)
// Escape: ESC ] 66 ; <colon-separated key=value> ; <utf8 text> BEL
// Bold/italic/color are NOT part of that protocol; use SGR alongside it.

#include <string>

struct KittySupport {
    bool width = false;  // terminal honors w=
    bool scale = false;  // terminal honors s= / n:d
    bool any() const { return width || scale; }
};

namespace kitty {

// Build one OSC sizing escape. text must be <= 4096 bytes; pass only the
// keys you need (defaults: s=1 w=0 n=0 d=0 v=0 h=0).
std::string sized(const std::string& text, int s = 1, int w = 0, int n = 0,
                  int d = 0, int v = 0, int h = 0);

// Query the terminal via CPR to detect width/scale support.
// Must be called while stdin is a tty in raw mode; restores nothing on
// screen except the probe spaces (caller should clear afterwards).
// Returns true if detection completed (support fields valid).
bool detectSupport(KittySupport& out);

}  // namespace kitty

namespace sgr {
inline const char* kReset = "\x1b[0m";
inline const char* kBold = "\x1b[1m";
inline const char* kDim = "\x1b[2m";
inline const char* kItalic = "\x1b[3m";
inline const char* kUnderline = "\x1b[4m";
inline const char* kStrike = "\x1b[9m";
inline const char* kReverse = "\x1b[7m";
// Header palette (bright, distinct per level).
inline const char* kH1 = "\x1b[1;95m";
inline const char* kH2 = "\x1b[1;94m";
inline const char* kH3 = "\x1b[1;96m";
inline const char* kH4 = "\x1b[1;92m";
inline const char* kH5 = "\x1b[1;93m";
inline const char* kH6 = "\x1b[1;91m";
inline const char* kQuote = "\x1b[36m";
inline const char* kCode = "\x1b[97;40m";
inline const char* kLink = "\x1b[4;34m";
inline const char* kListMark = "\x1b[33m";
}  // namespace sgr
