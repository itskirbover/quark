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

// Base64 (standard alphabet) for the graphics protocol payload.
std::string base64Encode(const unsigned char* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned n = static_cast<unsigned>(data[i]) << 16;
        size_t chunk = 1;
        if (i + 1 < len) {
            n |= static_cast<unsigned>(data[i + 1]) << 8;
            chunk = 2;
        }
        if (i + 2 < len) {
            n |= data[i + 2];
            chunk = 3;
        }
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(chunk >= 2 ? kTable[(n >> 6) & 63] : '=');
        out.push_back(chunk >= 3 ? kTable[n & 63] : '=');
    }
    return out;
}

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

std::string transmitPng(const unsigned char* png, size_t len, int id) {
    if (png == nullptr || len == 0) return "";
    std::string b64 = base64Encode(png, len);
    std::string out;
    size_t pos = 0;
    while (pos < b64.size()) {
        size_t n = std::min<size_t>(4096, b64.size() - pos);
        bool last = (pos + n >= b64.size());
        // a=t stores only — never displays (display is an explicit a=p at
        // the target cursor position, so no ghost copy appears at 1,1).
        out += "\x1b_Ga=t,f=100,i=" + std::to_string(id) +
               ",m=" + (last ? "0" : "1") +
               ",q=2;" + b64.substr(pos, n) + "\x1b\\";
        pos += n;
    }
    return out;
}

std::string displayImage(int id, int c, int r) {
    if (c < 1) c = 1;
    if (r < 1) r = 1;
    return "\x1b_Ga=p,i=" + std::to_string(id) + ",c=" +
           std::to_string(c) + ",r=" + std::to_string(r) +
           ",q=2\x1b\\";
}

std::string deleteImage(int id) {
    return "\x1b_Ga=d,d=i,i=" + std::to_string(id) + ",q=2\x1b\\";
}

bool cellSizePx(int& w, int& h) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
    // xterm window op: reply is CSI 6 ; <height> ; <width> t.
    writeAll("\x1b[16t");
    fflush(stdout);
    std::string buf;
    for (int waited = 0; waited < 250; waited += 20) {
        int c = readByteMs(20);
        if (c == -1) continue;
        buf.push_back(static_cast<char>(c));
        size_t esc = buf.find("\x1b[");
        if (esc == std::string::npos) {
            if (buf.size() > 32) buf.erase(0, buf.size() - 32);
            continue;
        }
        size_t t = buf.find('t', esc);
        if (t == std::string::npos) continue;
        int a = 0, ph = 0, pw = 0;
        if (sscanf(buf.c_str() + esc, "\x1b[%d;%d;%dt", &a, &ph, &pw) == 3 &&
            a == 6 && ph > 0 && pw > 0 && ph < 500 && pw < 500) {
            w = pw;
            h = ph;
            return true;
        }
        buf.erase(0, t + 1);
    }
    return false;
}

bool detectGraphics(KittySupport& out) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        out.graphics = false;
        return false;
    }
    // 1x1 black RGB pixel; expect an APC "OK" reply. Fixed id, deleted after.
    const int kProbeId = 99;
    std::string probe = "\x1b_Ga=t,t=d,f=24,s=1,v=1,i=" +
                        std::to_string(kProbeId) + ";AAAA\x1b\\";
    writeAll(probe);
    fflush(stdout);
    std::string buf;
    bool ok = false;
    for (int waited = 0; waited < 300 && !ok; waited += 20) {
        int c = readByteMs(20);
        if (c == -1) continue;
        buf.push_back(static_cast<char>(c));
        if (buf.find("OK") != std::string::npos &&
            buf.find("_G") != std::string::npos) {
            ok = true;
        }
        if (buf.size() > 256) buf.erase(0, buf.size() - 256);
    }
    // Best-effort cleanup of the probe image; quiet so nothing comes back.
    writeAll(deleteImage(kProbeId));
    fflush(stdout);
    for (int i = 0; i < 5; ++i) {
        if (readByteMs(10) == -1) break;
    }
    out.graphics = ok;
    return true;
}

}  // namespace kitty
