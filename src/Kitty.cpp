#include "Kitty.hpp"

#include <sys/select.h>
#include <unistd.h>

#include <cstdio>
#include <vector>

namespace kitty {

std::string sized(const std::string& text, int s, int w, int n, int d, int v,
                  int h) {
    std::string meta;
    auto add = [&](const char* k, int val, int def) {
        if (val == def) return;
        if (!meta.empty()) meta.push_back(':');
        meta += k;
        meta.push_back('=');
        meta += std::to_string(val);
    };
    add("s", s, 1);
    add("w", w, 0);
    add("n", n, 0);
    add("d", d, 0);
    add("v", v, 0);
    add("h", h, 0);
    if (meta.empty()) meta = "s=1";
    std::string out = "\x1b]66;";
    out += meta;
    out += ';';
    out += text;
    out += '\a';
    return out;
}

namespace {

void writeAll(const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(STDOUT_FILENO, s.data() + off, s.size() - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

// Read with timeout; returns -1 on timeout/error.
int readByteMs(int timeoutMs) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
    if (r <= 0 || !FD_ISSET(STDIN_FILENO, &rfds)) return -1;
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
}

}  // namespace

bool detectSupport(KittySupport& out) {
    out = KittySupport{};
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;

    // Per spec: CR, CPR, w=2 space, CPR, s=2 space, CPR. Compare columns.
    std::string probe;
    probe += '\r';
    probe += "\x1b[6n";
    probe += sized(" ", 1, 2);
    probe += "\x1b[6n";
    probe += sized(" ", 2, 0);
    probe += "\x1b[6n";
    writeAll(probe);
    fflush(stdout);

    // Collect ESC [ <row> ; <col> R replies (expect 3) within ~600ms total.
    std::vector<int> cols;
    std::string buf;
    for (int waited = 0; waited < 600 && cols.size() < 3; waited += 20) {
        int c = readByteMs(20);
        if (c == -1) continue;
        buf.push_back(static_cast<char>(c));
        // Try to parse trailing CPR replies out of buf.
        while (true) {
            size_t esc = buf.find("\x1b[");
            if (esc == std::string::npos) {
                if (buf.size() > 32) buf.erase(0, buf.size() - 32);
                break;
            }
            size_t rpos = buf.find('R', esc);
            if (rpos == std::string::npos) break;
            std::string seq = buf.substr(esc, rpos - esc + 1);
            buf.erase(0, rpos + 1);
            int row = 0, col = 0;
            if (sscanf(seq.c_str(), "\x1b[%d;%dR", &row, &col) == 2) {
                (void)row;
                cols.push_back(col);
            }
        }
    }
    // Drain anything left briefly so probe bytes don't leak into the editor.
    for (int i = 0; i < 5; ++i) {
        if (readByteMs(10) == -1) break;
    }
    if (cols.size() < 3) return false;
    int d1 = cols[1] - cols[0];
    int d2 = cols[2] - cols[1];
    out.width = (d1 == 2);
    out.scale = (d2 == 2);
    return true;
}

}  // namespace kitty
