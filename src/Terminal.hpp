#pragma once
// POSIX terminal handling: raw mode, alternate screen, window size.
// Ctrl-C arrives as byte 0x03 (ISIG disabled) so the editor can prompt.

#include <string>

class Terminal {
public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    bool enableRaw();
    void disableRaw();
    bool isRaw() const { return rawEnabled_; }

    void enterAltScreen();
    void exitAltScreen();

    void hideCursor();
    void showCursor();

    // SGR mouse tracking (1000 + 1006): button presses/releases and wheel.
    // Shifted clicks are left for the terminal (selection bypass).
    void enableMouse();
    void disableMouse();

    // Bracketed paste (2004): pastes arrive as a single Paste key.
    void enablePaste();
    void disablePaste();

    // Disambiguated keyboard mode (CSI > 1 u): terminals report
    // ambiguous keys as CSI-u (e.g. Ctrl+Shift+C as 99;6u). Popped
    // with CSI < u; extra pops are ignored by the terminal.
    void enableKeyboard();
    void disableKeyboard();

    // System clipboard write via OSC 52 (clipboard 'c'). Best-effort:
    // unsupported terminals ignore it.
    static void copyToClipboard(const std::string& text);

    bool refreshSize();
    int rows() const { return rows_; }
    int cols() const { return cols_; }

    static void writeRaw(const char* data, size_t len);
    static void writeRaw(const std::string& s);
    void writeStr(const std::string& s) { writeRaw(s); }

private:
    struct termios* orig_ = nullptr;
    bool rawEnabled_ = false;
    bool altActive_ = false;
    bool kbActive_ = false;
    int rows_ = 24;
    int cols_ = 80;
};
