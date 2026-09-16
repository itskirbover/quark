#include "terminal.h"
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

namespace terminal {

RawMode::RawMode() {
    if (::tcgetattr(STDIN_FILENO, &saved_) != 0) return;
    termios raw = saved_;
    raw.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON | IXOFF | ISTRIP | INPCK | BRKINT | PARMRK);
    raw.c_oflag &= ~(tcflag_t)(OPOST);
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHONL | ISIG | IEXTEN);
    raw.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        active_ = true;
    }
}

RawMode::~RawMode() {
    if (active_) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }
}

Size size() {
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        return Size{static_cast<int>(ws.ws_row), static_cast<int>(ws.ws_col)};
    }
    return Size{24, 80};
}

void enter_alt_screen() {
    const char* s = "\x1b[?1049h\x1b[2J\x1b[?25l";
    ::write(STDOUT_FILENO, s, 15);
    ::fsync(STDOUT_FILENO);
}

void leave_alt_screen() {
    const char* s = "\x1b[?25h\x1b[?1049l";
    ::write(STDOUT_FILENO, s, 12);
    ::fsync(STDOUT_FILENO);
}

bool stdin_ready(int timeout_ms) {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    int r = ::poll(&pfd, 1, timeout_ms);
    return r > 0 && (pfd.revents & POLLIN);
}

int read_byte() {
    unsigned char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    return static_cast<int>(c);
}

} // namespace terminal
