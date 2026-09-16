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
    int rows_ = 24;
    int cols_ = 80;
};
