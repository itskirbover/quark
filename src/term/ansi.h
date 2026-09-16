#pragma once
#include <string>

// ANSI escape code helpers for terminal output.
namespace ansi {

constexpr const char* RESET    = "\x1b[0m";
constexpr const char* BOLD     = "\x1b[1m";
constexpr const char* DIM      = "\x1b[2m";
constexpr const char* ITALIC   = "\x1b[3m";
constexpr const char* UNDERLINE = "\x1b[4m";
constexpr const char* REVERSE  = "\x1b[7m";
constexpr const char* STRIKE   = "\x1b[9m";

// Foreground color. n in 0..15. 0-7 normal, 8-15 bright.
inline std::string fg(int n) {
    if (n < 8) return "\x1b[" + std::to_string(30 + n) + "m";
    return "\x1b[" + std::to_string(90 + (n - 8)) + "m";
}
// Background color. n in 0..15.
inline std::string bg(int n) {
    if (n < 8) return "\x1b[" + std::to_string(40 + n) + "m";
    return "\x1b[" + std::to_string(100 + (n - 8)) + "m";
}
// 256-color foreground.
inline std::string fg256(int n) { return "\x1b[38;5;" + std::to_string(n) + "m"; }
// 256-color background.
inline std::string bg256(int n) { return "\x1b[48;5;" + std::to_string(n) + "m"; }
// True-color foreground.
inline std::string fg_rgb(int r, int g, int b) {
    return "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}
// True-color background.
inline std::string bg_rgb(int r, int g, int b) {
    return "\x1b[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

// Move cursor to row,col (1-based).
inline std::string move(int row, int col) {
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}
inline std::string clear()      { return "\x1b[2J"; }
inline std::string clear_line() { return "\x1b[2K"; }
inline std::string erase_below(){ return "\x1b[0J"; }
inline std::string hide_cursor(){ return "\x1b[?25l"; }
inline std::string show_cursor(){ return "\x1b[?25h"; }
inline std::string save_cursor()  { return "\x1b[s"; }
inline std::string restore_cursor(){ return "\x1b[u"; }

} // namespace ansi
