#include "textsizing.h"
#include <unistd.h>
#include <poll.h>
#include <string>
#include <vector>

namespace textsizing {

namespace {
constexpr char ESC = 0x1b;
constexpr char BEL = 0x07;

// Read bytes from stdin until a given byte terminator (or timeout), returning
// everything read including any leading escape sequence. Used to read CPR
// responses.
std::string read_until(char terminator, int timeout_ms) {
    std::string out;
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    char buf[1];
    long long deadline = 0;
    // We poll repeatedly with small increments so we can cap total time.
    int waited = 0;
    while (waited < timeout_ms) {
        int step = 50;
        if (timeout_ms - waited < step) step = timeout_ms - waited;
        int r = poll(&pfd, 1, step);
        waited += step;
        if (r <= 0) break; // timeout or error
        if (pfd.revents & POLLIN) {
            ssize_t n = ::read(STDIN_FILENO, buf, 1);
            if (n <= 0) break;
            out.push_back(buf[0]);
            if (buf[0] == terminator) break;
        }
    }
    return out;
}

// Send a CPR query and parse the response column (1-based). Returns -1 on
// failure/timeout.
int query_column(int timeout_ms) {
    // Flush any pending input first.
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    while (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        char buf[64];
        if (::read(STDIN_FILENO, buf, sizeof(buf)) <= 0) break;
    }
    const char* q = "\x1b[6n"; // DSR/CPR: report cursor position
    ::write(STDOUT_FILENO, q, 4);
    ::fsync(STDOUT_FILENO);
    std::string resp = read_until('R', timeout_ms);
    // Expected: ESC [ row ; col R
    // Find the semicolon then digits then 'R'.
    auto semi = resp.rfind(';');
    if (semi == std::string::npos) return -1;
    // Parse digits after the semicolon (col).
    int col = 0;
    bool any = false;
    for (size_t i = semi + 1; i < resp.size() && resp[i] >= '0' && resp[i] <= '9'; ++i) {
        col = col * 10 + (resp[i] - '0');
        any = true;
    }
    if (!any) return -1;
    return col;
}

} // namespace

std::string osc(const std::string& metadata, const std::string& text) {
    std::string out = "\x1b]_text_size_code;";
    out += metadata;
    out.push_back(';');
    out += text;
    out.push_back(BEL);
    return out;
}

std::string render(const std::string& text, int s, int w, int n, int d, int v, int h) {
    // Build metadata, only including non-default keys.
    std::string meta;
    if (s > 1) meta += "s=" + std::to_string(s);
    if (w > 0) {
        if (!meta.empty()) meta.push_back(':');
        meta += "w=" + std::to_string(w);
    }
    if (n > 0) {
        if (!meta.empty()) meta.push_back(':');
        meta += "n=" + std::to_string(n);
    }
    if (d > 0) {
        if (!meta.empty()) meta.push_back(':');
        meta += "d=" + std::to_string(d);
    }
    if (v > 0) {
        if (!meta.empty()) meta.push_back(':');
        meta += "v=" + std::to_string(v);
    }
    if (h > 0) {
        if (!meta.empty()) meta.push_back(':');
        meta += "h=" + std::to_string(h);
    }
    // The text portion must not exceed 4096 bytes; chunk if necessary.
    const size_t limit = 4000;
    std::string out;
    if (text.size() <= limit) {
        out = osc(meta, text);
    } else {
        for (size_t i = 0; i < text.size(); i += limit) {
            out += osc(meta, text.substr(i, limit));
        }
    }
    return out;
}

Support detect() {
    // Technique from the spec:
    //   Send CR + CPR, then a space rendered w=2, then CPR; then a space
    //   rendered s=2, then CPR. Compare columns.
    constexpr int t = 300;
    int col0 = query_column(t);
    if (col0 < 0) return Support::None;

    std::string w2 = render(" ", 1, 2); // space rendered 2 cells wide (w=2)
    ::write(STDOUT_FILENO, w2.data(), w2.size());
    ::fsync(STDOUT_FILENO);
    int col1 = query_column(t);
    if (col1 < 0) return Support::None;

    std::string s2 = render(" ", 2); // space rendered in a 2x2 block (s=2)
    ::write(STDOUT_FILENO, s2.data(), s2.size());
    ::fsync(STDOUT_FILENO);
    int col2 = query_column(t);
    if (col2 < 0) return Support::None;

    if (col2 > col1 && col1 > col0) return Support::Scale;
    if (col1 > col0) return Support::Width;
    return Support::None;
}

} // namespace textsizing
