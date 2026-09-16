#include "Terminal.hpp"

#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
struct termios g_orig;
bool g_haveOrig = false;
}  // namespace

Terminal::Terminal() { refreshSize(); }

Terminal::~Terminal() {
    disableMouse();
    exitAltScreen();
    disableRaw();
}

bool Terminal::enableRaw() {
    if (rawEnabled_) return true;
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &g_orig) == -1) return false;
    g_haveOrig = true;
    orig_ = &g_orig;

    struct termios raw = g_orig;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |= (unsigned)(CS8);
    // Disable ISIG so Ctrl-C / Ctrl-Z arrive as bytes instead of signals.
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;  // 100ms timeout for read()
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return false;
    rawEnabled_ = true;
    // Ignore SIGWINCH default handling; editor polls size each frame.
    signal(SIGWINCH, SIG_IGN);
    return true;
}

void Terminal::disableRaw() {
    if (!rawEnabled_ || !g_haveOrig) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    rawEnabled_ = false;
}

void Terminal::enterAltScreen() {
    if (altActive_) return;
    writeRaw("\x1b[?1049h");
    altActive_ = true;
}

void Terminal::exitAltScreen() {
    if (!altActive_) return;
    writeRaw("\x1b[?1049l");
    altActive_ = false;
}

void Terminal::hideCursor() { writeRaw("\x1b[?25l"); }
void Terminal::showCursor() { writeRaw("\x1b[?25h"); }

void Terminal::enableMouse() { writeRaw("\x1b[?1000h\x1b[?1006h"); }
void Terminal::disableMouse() { writeRaw("\x1b[?1006l\x1b[?1000l"); }

bool Terminal::refreshSize() {
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 &&
        ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
        return false;
    }
    if (ws.ws_row >= 2 && ws.ws_col >= 2) {
        rows_ = ws.ws_row;
        cols_ = ws.ws_col;
        return true;
    }
    return false;
}

void Terminal::writeRaw(const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(STDOUT_FILENO, data + off, len - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

void Terminal::writeRaw(const std::string& s) { writeRaw(s.data(), s.size()); }
