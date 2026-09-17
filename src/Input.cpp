#include "Input.hpp"

#include <sys/select.h>
#include <unistd.h>

#include <cstdio>

#include "Utf8.hpp"

namespace {

bool byteReady(int timeoutMs) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    if (timeoutMs < 0) {
        int r = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, nullptr);
        return r > 0 && FD_ISSET(STDIN_FILENO, &rfds);
    }
    struct timeval tv {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
    return r > 0 && FD_ISSET(STDIN_FILENO, &rfds);
}

// Returns -1 on timeout, -2 on EOF/error.
int readByte(int timeoutMs) {
    if (!byteReady(timeoutMs)) return -1;
    unsigned char c = 0;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 0) {
        // EOF (pty closed / stdin pipe drained): back off so we never
        // busy-spin; the caller treats this as "no key".
        usleep(100 * 1000);
        return -2;
    }
    if (n != 1) return -1;
    return c;
}

Key arrowKey(char final, bool shift) {
    Key::Type t = Key::Type::None;
    switch (final) {
        case 'A': t = Key::Type::ArrowUp; break;
        case 'B': t = Key::Type::ArrowDown; break;
        case 'C': t = Key::Type::ArrowRight; break;
        case 'D': t = Key::Type::ArrowLeft; break;
        case 'H': t = Key::Type::Home; break;
        case 'F': t = Key::Type::End; break;
        default: return Key::make(Key::Type::None);
    }
    Key k = Key::make(t);
    k.shift = shift;
    return k;
}

// xterm modifier parameter: 1 + flags(shift=1, alt=2, ctrl=4).
bool hasShiftParam(int mod) { return mod >= 1 && ((mod - 1) & 1) != 0; }
bool hasCtrlParam(int mod) { return mod >= 1 && ((mod - 1) & 4) != 0; }

// Split ';'-separated CSI params into ints (empty -> 0).
void splitParams(const std::string& s, int* out, int cap) {
    for (int i = 0; i < cap; ++i) out[i] = 0;
    int idx = 0;
    int val = 0;
    bool have = false;
    for (size_t i = 0; i <= s.size() && idx < cap; ++i) {
        char c = (i < s.size()) ? s[i] : ';';
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            have = true;
        } else if (c == ';' || c == ':') {
            out[idx++] = have ? val : 0;
            val = 0;
            have = false;
        } else {
            return;  // unexpected char: keep what we parsed
        }
    }
}

Key readPaste() {
    Key k = Key::make(Key::Type::Paste);
    const std::string end = "\x1b[201~";
    std::string pending;
    bool carriageReturn = false;
    bool overflow = false;
    for (;;) {
        int c = readByte(-1);
        if (c == -2) return Key::make(Key::Type::None);
        if (c < 0) continue;
        pending.push_back(static_cast<char>(c));
        if (pending == end) break;
        while (!pending.empty() && end.compare(0, pending.size(), pending) != 0) {
            unsigned char ch = static_cast<unsigned char>(pending.front());
            pending.erase(0, 1);
            if (ch == '\r') {
                if (!overflow) k.text += '\n';
                carriageReturn = true;
            } else {
                if (!(ch == '\n' && carriageReturn) &&
                    (ch == '\n' || ch == '\t' || ch >= 0x20) && ch != 0x7f &&
                    !overflow) {
                    k.text += static_cast<char>(ch);
                }
                carriageReturn = false;
            }
            if (k.text.size() > 8 * 1024 * 1024) {
                overflow = true;
                k.text.clear();
            }
        }
    }
    if (overflow) return Key::make(Key::Type::None);
    std::string valid;
    for (size_t i = 0; i < k.text.size();) {
        auto [cp, len] = utf8::decode(k.text, i);
        if (len == 0) break;
        if (cp < 0x80 || cp >= 0xa0) valid += utf8::encode(cp);
        i += len;
    }
    k.text = std::move(valid);
    return k;
}

Key readEscapeSequence() {
    // Called after ESC was consumed. Lone ESC (no follow-up within 50ms)
    // counts as the Esc key (used to cancel the quit prompt).
    int b1 = readByte(50);
    if (b1 == -1) return Key::make(Key::Type::Esc);
    if (b1 == '[') {
        int b2 = readByte(50);
        if (b2 == -1) return Key::make(Key::Type::Esc);
        if (b2 == '<') {
            // SGR mouse (1006): ESC [ < Pb ; Px ; Py M/m.
            std::string params;
            int c = 0;
            while (params.size() < 16) {
                c = readByte(50);
                if (c == -1) return Key::make(Key::Type::None);
                if (c == 'M' || c == 'm') break;
                params.push_back(static_cast<char>(c));
            }
            if (c != 'M' && c != 'm') return Key::make(Key::Type::None);
            int pb = -1, px = -1, py = -1;
            if (sscanf(params.c_str(), "%d;%d;%d", &pb, &px, &py) != 3)
                return Key::make(Key::Type::None);
            if (c == 'm') return Key::make(Key::Type::None);  // release
            if (pb & 64) {
                // Wheel (bit 0: 0 = up, 1 = down).
                return Key::make((pb & 1) ? Key::Type::WheelDown
                                          : Key::Type::WheelUp);
            }
            // Shifted clicks are left to the terminal (selection bypass).
            if (pb & 4) return Key::make(Key::Type::None);
            if ((pb & 3) != 0) return Key::make(Key::Type::None);  // non-left
            if (px < 1 || py < 1) return Key::make(Key::Type::None);
            Key k = Key::make(Key::Type::MousePress);
            k.mouseCol = px;
            k.mouseRow = py;
            return k;
        }
        // Generic CSI: params (digits/';'), optional intermediates,
        // then a final byte. Covers plain arrows (A-D), Home/End (H/F),
        // '~' keys (3=Delete, 1/7=Home, 4/8=End, 5/6=PgUp/PgDn),
        // modified keys (1;mod + letter), CSI-u (code;mod u), URxvt
        // shift-arrows (a-d), and modifyOtherKeys (27;mod;code ~).
        if ((b2 >= 'A' && b2 <= 'Z') || b2 == '~') {
            if (b2 >= 'A' && b2 <= 'D') return arrowKey((char)b2, false);
            if (b2 == 'H' || b2 == 'F') return arrowKey((char)b2, false);
            if (b2 == 'Z') return Key::make(Key::Type::None);  // shift-tab
            return Key::make(Key::Type::None);
        }
        // URxvt-style shift-arrows: ESC [ a/b/c/d.
        if (b2 >= 'a' && b2 <= 'd') {
            return arrowKey((char)(b2 - 'a' + 'A'), true);
        }
        if ((b2 >= '0' && b2 <= '9') || b2 == ';') {
            std::string params;
            params.push_back(static_cast<char>(b2));
            int c = 0;
            while (params.size() < 24) {
                c = readByte(50);
                if (c == -1) return Key::make(Key::Type::None);
                if ((c >= '0' && c <= '9') || c == ';' || c == ':') {
                    params.push_back(static_cast<char>(c));
                    continue;
                }
                break;
            }
            // Skip intermediates (e.g. '$', '"', ' ').
            while (c >= ' ' && c <= '/' && (size_t)params.size() < 32) {
                c = readByte(50);
                if (c == -1) return Key::make(Key::Type::None);
            }
            if (c < '@' || c > '~') return Key::make(Key::Type::None);
            char final = (char)c;
            int p[4] = {0, 0, 0, 0};
            splitParams(params, p, 4);
            if (final >= 'A' && final <= 'D') {
                return arrowKey(final, hasShiftParam(p[1]));
            }
            if (final == 'H' || final == 'F') {
                return arrowKey(final, hasShiftParam(p[1]));
            }
            if (final == '~') {
                if (p[0] == 200) return readPaste();
                if (p[0] == 27 && (p[2] == 99 || p[2] == 67)) {
                    // modifyOtherKeys Ctrl+Shift+C (copy) / Ctrl+C.
                    if (hasCtrlParam(p[1]) && hasShiftParam(p[1]))
                        return Key::make(Key::Type::Copy);
                    if (hasCtrlParam(p[1]))
                        return Key::make(Key::Type::CtrlC);
                    return Key::make(Key::Type::None);
                }
                if (p[0] == 27 && (p[2] == 115 || p[2] == 83)) {
                    // modifyOtherKeys Ctrl+S.
                    if (hasCtrlParam(p[1]))
                        return Key::make(Key::Type::CtrlS);
                    return Key::make(Key::Type::None);
                }
                if (p[0] == 27 && (p[2] == 122 || p[2] == 90)) {
                    // modifyOtherKeys Ctrl+Z.
                    if (hasCtrlParam(p[1]))
                        return Key::make(Key::Type::CtrlZ);
                    return Key::make(Key::Type::None);
                }
                if (p[0] == 27 && (p[2] == 121 || p[2] == 89)) {
                    // modifyOtherKeys Ctrl+Y.
                    if (hasCtrlParam(p[1]))
                        return Key::make(Key::Type::CtrlY);
                    return Key::make(Key::Type::None);
                }
                if (p[0] == 27 && p[2] == 127) {
                    if (hasCtrlParam(p[1]))
                        return Key::make(Key::Type::CtrlBackspace);
                    return Key::make(Key::Type::Backspace);
                }
                bool shift = hasShiftParam(p[1]);
                bool ctrl = hasCtrlParam(p[1]);
                if (p[0] == 3) {
                    if (ctrl) return Key::make(Key::Type::CtrlDelete);
                    return Key::make(Key::Type::Delete);
                }
                if (p[0] == 1 || p[0] == 7)
                    return arrowKey('H', shift);
                if (p[0] == 4 || p[0] == 8)
                    return arrowKey('F', shift);
                if (p[0] == 5) {
                    Key k = Key::make(Key::Type::PageUp);
                    k.shift = shift;
                    return k;
                }
                if (p[0] == 6) {
                    Key k = Key::make(Key::Type::PageDown);
                    k.shift = shift;
                    return k;
                }
                return Key::make(Key::Type::None);
            }
            if (final == 'u') {
                // Kitty/CSI-u: code;mod u (127=Backspace, 13=Enter,
                // letter codes with Ctrl re-encoded by keyboard mode).
                int mod = p[1] == 0 ? 1 : p[1];
                if (p[0] == 127) {
                    if (hasCtrlParam(mod))
                        return Key::make(Key::Type::CtrlBackspace);
                    return Key::make(Key::Type::Backspace);
                }
                if (p[0] == 13) return Key::make(Key::Type::Enter);
                if ((p[0] == 99 || p[0] == 67) && hasCtrlParam(mod) &&
                    hasShiftParam(mod))
                    return Key::make(Key::Type::Copy);
                // Ctrl+letter re-encoded as CSI-u (keyboard mode on):
                // restore the legacy actions.
                if (hasCtrlParam(mod)) {
                    if (p[0] == 99 || p[0] == 67)
                        return Key::make(Key::Type::CtrlC);
                    if (p[0] == 115 || p[0] == 83)
                        return Key::make(Key::Type::CtrlS);
                    if (p[0] == 122 || p[0] == 90)
                        return Key::make(Key::Type::CtrlZ);
                    if (p[0] == 121 || p[0] == 89)
                        return Key::make(Key::Type::CtrlY);
                }
                return Key::make(Key::Type::None);
            }
            return Key::make(Key::Type::None);
        }
        return Key::make(Key::Type::None);
    }
    if (b1 == 'O') {
        int b2 = readByte(50);
        if (b2 == 'H') return Key::make(Key::Type::Home);
        if (b2 == 'F') return Key::make(Key::Type::End);
        // URxvt/SS3 shift-arrows: ESC O a/b/c/d.
        if (b2 >= 'a' && b2 <= 'd') {
            return arrowKey((char)(b2 - 'a' + 'A'), true);
        }
        // Application-cursor modified keys: ESC O 1;mod X.
        if (b2 >= '0' && b2 <= '9') {
            std::string params;
            params.push_back(static_cast<char>(b2));
            int c = 0;
            while (params.size() < 24) {
                c = readByte(50);
                if (c == -1) return Key::make(Key::Type::None);
                if ((c >= '0' && c <= '9') || c == ';' || c == ':') {
                    params.push_back(static_cast<char>(c));
                    continue;
                }
                break;
            }
            if (c < 'A' || c > 'Z') return Key::make(Key::Type::None);
            int p[4] = {0, 0, 0, 0};
            splitParams(params, p, 4);
            if (c >= 'A' && c <= 'D') return arrowKey((char)c, hasShiftParam(p[1]));
            if (c == 'H' || c == 'F') return arrowKey((char)c, hasShiftParam(p[1]));
        }
        return Key::make(Key::Type::None);
    }
    // Alt+C (ESC + c): copy fallback, delivered by every terminal.
    if (b1 == 'c' || b1 == 'C') return Key::make(Key::Type::Copy);
    return Key::make(Key::Type::None);
}

}  // namespace

Key InputReader::readKey() {
    int c = readByte(-1);  // block (raw VTIME still bounds each read)
    // readByte(-1): select with negative timeout -> wait indefinitely.
    while (c == -1) c = readByte(-1);
    if (c == -2) return Key::make(Key::Type::None);  // EOF: no key
    if (c == 0x1B) return readEscapeSequence();
    if (c == 0x03) return Key::make(Key::Type::CtrlC);
    if (c == 0x13) return Key::make(Key::Type::CtrlS);
    if (c == 0x1A) return Key::make(Key::Type::CtrlZ);  // undo
    if (c == 0x19) return Key::make(Key::Type::CtrlY);  // redo
    if (c == '\r' || c == '\n') return Key::make(Key::Type::Enter);
    // Most terminals send Backspace as DEL (0x7F); Ctrl+Backspace arrives
    // as 0x08 (or an explicit sequence decoded above).
    if (c == 0x7F) return Key::make(Key::Type::Backspace);
    if (c == 0x08) return Key::make(Key::Type::CtrlBackspace);
    if (c < 0x20) return Key::make(Key::Type::None);  // other controls ignored

    std::string bytes;
    bytes.push_back(static_cast<char>(c));
    size_t want = utf8::seqLen(static_cast<unsigned char>(c));
    if (want == 0) return Key::make(Key::Type::None);
    for (size_t i = 1; i < want; ++i) {
        int d = readByte(200);
        if (d == -1) return Key::make(Key::Type::None);
        bytes.push_back(static_cast<char>(d));
    }
    auto [cp, len] = utf8::decode(bytes, 0);
    (void)len;
    Key k = Key::make(Key::Type::Char);
    k.text = bytes;
    k.cp = cp;
    return k;
}
