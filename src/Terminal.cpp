#include "Terminal.hpp"

#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
struct termios g_orig;
bool g_haveOrig = false;

std::string base64Encode(const std::string& in) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned n = static_cast<unsigned char>(in[i]) << 16;
        size_t chunk = 1;
        if (i + 1 < in.size()) {
            n |= static_cast<unsigned char>(in[i + 1]) << 8;
            chunk = 2;
        }
        if (i + 2 < in.size()) {
            n |= static_cast<unsigned char>(in[i + 2]);
            chunk = 3;
        }
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(chunk >= 2 ? kTable[(n >> 6) & 63] : '=');
        out.push_back(chunk >= 3 ? kTable[n & 63] : '=');
    }
    return out;
}

}  // namespace

Terminal::Terminal() { refreshSize(); }

Terminal::~Terminal() {
    disableKeyboard();
    disablePaste();
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
    // Restore the default cursor shape (DECSCUSR 0) so the bar cursor
    // doesn't leak into the shell after quark exits.
    writeRaw("\x1b[?1049l\x1b[0 q\x1b[?25h");
    altActive_ = false;
}

void Terminal::hideCursor() { writeRaw("\x1b[?25l"); }
// Steady bar (line) instead of the terminal default block.
void Terminal::showCursor() { writeRaw("\x1b[6 q\x1b[?25h"); }

void Terminal::enableMouse() { writeRaw("\x1b[?1000h\x1b[?1006h"); }
void Terminal::disableMouse() { writeRaw("\x1b[?1006l\x1b[?1000l"); }

void Terminal::enablePaste() { writeRaw("\x1b[?2004h"); }
void Terminal::disablePaste() { writeRaw("\x1b[?2004l"); }

void Terminal::enableKeyboard() {
    if (kbActive_) return;
    writeRaw("\x1b[>1u");
    kbActive_ = true;
}

void Terminal::disableKeyboard() {
    if (!kbActive_) return;
    writeRaw("\x1b[<u");
    kbActive_ = false;
}

void Terminal::copyToClipboard(const std::string& text) {
    if (text.empty()) return;
    writeRaw("\x1b]52;c;" + base64Encode(text) + "\x07");
}

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
