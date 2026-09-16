#include "Input.hpp"

#include <sys/select.h>
#include <unistd.h>

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

Key readEscapeSequence() {
    // Called after ESC was consumed. Lone ESC (no follow-up within 50ms)
    // counts as the Esc key (used to cancel the quit prompt).
    int b1 = readByte(50);
    if (b1 == -1) return Key::make(Key::Type::Esc);
    if (b1 == '[') {
        int b2 = readByte(50);
        if (b2 == -1) return Key::make(Key::Type::Esc);
        switch (b2) {
            case 'A': return Key::make(Key::Type::ArrowUp);
            case 'B': return Key::make(Key::Type::ArrowDown);
            case 'C': return Key::make(Key::Type::ArrowRight);
            case 'D': return Key::make(Key::Type::ArrowLeft);
            case 'H': return Key::make(Key::Type::Home);
            case 'F': return Key::make(Key::Type::End);
            case '3':
            case '1':
            case '5':
            case '6':
            case '7':
            case '8': {
                int b3 = readByte(50);
                if (b3 == '~') {
                    if (b2 == '3') return Key::make(Key::Type::Delete);
                    if (b2 == '1' || b2 == '7')
                        return Key::make(Key::Type::Home);
                    if (b2 == '8') return Key::make(Key::Type::End);
                    if (b2 == '5') return Key::make(Key::Type::PageUp);
                    if (b2 == '6') return Key::make(Key::Type::PageDown);
                } else if (b3 == ';') {
                    // Modified keys (e.g. shift-arrows): drain to the letter.
                    int c;
                    do {
                        c = readByte(50);
                    } while (c != -1 && !(c >= 'A' && c <= 'Z'));
                    if (c == 'A') return Key::make(Key::Type::ArrowUp);
                    if (c == 'B') return Key::make(Key::Type::ArrowDown);
                    if (c == 'C') return Key::make(Key::Type::ArrowRight);
                    if (c == 'D') return Key::make(Key::Type::ArrowLeft);
                }
                return Key::make(Key::Type::None);
            }
            default:
                return Key::make(Key::Type::None);
        }
    }
    if (b1 == 'O') {
        int b2 = readByte(50);
        if (b2 == 'H') return Key::make(Key::Type::Home);
        if (b2 == 'F') return Key::make(Key::Type::End);
        return Key::make(Key::Type::None);
    }
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
    if (c == '\r' || c == '\n') return Key::make(Key::Type::Enter);
    if (c == 0x7F || c == 0x08) return Key::make(Key::Type::Backspace);
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
