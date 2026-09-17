#include "Buffer.hpp"

#include <fstream>
#include <sstream>

#include "Utf8.hpp"

namespace {
const std::string kEmpty;
}

Buffer::Buffer() { lines_.push_back(""); }

bool Buffer::open(const std::string& path) {
    filename_ = path;
    lines_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        lines_.push_back("");
        dirty_ = false;
        return true;  // new file
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (content.empty()) {
        lines_.push_back("");
        trailingNewline_ = false;
        dirty_ = false;
        return true;
    }
    trailingNewline_ = content.back() == '\n';
    if (trailingNewline_) content.pop_back();  // store lines, not the terminator
    // Split on '\n', strip trailing '\r' (CRLF files).
    size_t start = 0;
    while (true) {
        size_t nl = content.find('\n', start);
        std::string l = (nl == std::string::npos) ? content.substr(start)
                                                  : content.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines_.push_back(l);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (lines_.empty()) lines_.push_back("");
    dirty_ = false;
    return true;
}

bool Buffer::save() {
    if (filename_.empty()) return false;
    return saveAs(filename_);
}

bool Buffer::saveAs(const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i > 0) out << '\n';
        out << lines_[i];
    }
    if (trailingNewline_ && !lines_.empty()) out << '\n';
    if (!out) return false;
    filename_ = path;
    dirty_ = false;
    return true;
}

const std::string& Buffer::line(size_t y) const {
    if (y >= lines_.size()) return kEmpty;
    return lines_[y];
}

size_t Buffer::lineLen(size_t y) const { return line(y).size(); }

void Buffer::insertText(size_t y, size_t x, const std::string& text) {
    if (y >= lines_.size() || text.empty()) return;
    x = clampToChar(y, x);
    lines_[y].insert(x, text);
    dirty_ = true;
}

void Buffer::insertNewline(size_t y, size_t x) {
    if (y >= lines_.size()) return;
    x = clampToChar(y, x);
    std::string tail = lines_[y].substr(x);
    lines_[y].erase(x);
    lines_.insert(lines_.begin() + y + 1, tail);
    dirty_ = true;
}

void Buffer::backspace(size_t y, size_t x, size_t& outY, size_t& outX) {
    if (y >= lines_.size()) {
        outY = 0;
        outX = 0;
        return;
    }
    x = clampToChar(y, x);
    if (x > 0) {
        size_t prev = utf8::prevCharStart(lines_[y], x);
        lines_[y].erase(prev, x - prev);
        outY = y;
        outX = prev;
        dirty_ = true;
    } else if (y > 0) {
        size_t prevLen = lines_[y - 1].size();
        lines_[y - 1] += lines_[y];
        lines_.erase(lines_.begin() + y);
        outY = y - 1;
        outX = prevLen;
        dirty_ = true;
    } else {
        outY = y;
        outX = x;
    }
}

void Buffer::deleteForward(size_t y, size_t x) {
    if (y >= lines_.size()) return;
    x = clampToChar(y, x);
    if (x < lines_[y].size()) {
        auto [cp, len] = utf8::decode(lines_[y], x);
        (void)cp;
        if (len == 0) len = 1;
        lines_[y].erase(x, len);
        dirty_ = true;
    } else if (y + 1 < lines_.size()) {
        lines_[y] += lines_[y + 1];
        lines_.erase(lines_.begin() + y + 1);
        dirty_ = true;
    }
}

namespace {

// Order range endpoints so (y0,x0) <= (y1,x1).
void orderRange(size_t& y0, size_t& x0, size_t& y1, size_t& x1) {
    if (y1 < y0 || (y1 == y0 && x1 < x0)) {
        std::swap(y0, y1);
        std::swap(x0, x1);
    }
}

// 0 = whitespace, 1 = word char (alnum, '_', non-ASCII), 2 = punctuation.
int charClass(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return 0;
    if (cp == '_') return 1;
    if (cp < 0x80) return (cp < '0' || (cp > '9' && cp < 'A') ||
                            (cp > 'Z' && cp < 'a') || cp > 'z')
                           ? 2
                           : 1;
    return 1;  // non-ASCII (CJK, emoji, accented): word chars
}

}  // namespace

std::string Buffer::extractRange(size_t y0, size_t x0, size_t y1,
                                 size_t x1) const {
    if (lines_.empty()) return "";
    if (y0 >= lines_.size()) y0 = lines_.size() - 1;
    if (y1 >= lines_.size()) y1 = lines_.size() - 1;
    orderRange(y0, x0, y1, x1);
    x0 = clampToChar(y0, x0);
    x1 = clampToChar(y1, x1);
    if (y0 == y1) return lines_[y0].substr(x0, x1 > x0 ? x1 - x0 : 0);
    std::string out = lines_[y0].substr(x0);
    for (size_t y = y0 + 1; y < y1; ++y) {
        out += '\n';
        out += lines_[y];
    }
    out += '\n';
    out += lines_[y1].substr(0, x1);
    return out;
}

void Buffer::eraseRange(size_t y0, size_t x0, size_t y1, size_t x1) {
    if (lines_.empty()) return;
    if (y0 >= lines_.size()) y0 = lines_.size() - 1;
    if (y1 >= lines_.size()) y1 = lines_.size() - 1;
    orderRange(y0, x0, y1, x1);
    x0 = clampToChar(y0, x0);
    x1 = clampToChar(y1, x1);
    if (y0 == y1 && x0 == x1) return;
    if (y0 == y1) {
        lines_[y0].erase(x0, x1 - x0);
    } else {
        std::string tail = lines_[y1].substr(x1);
        lines_[y0].erase(x0);
        lines_[y0] += tail;
        lines_.erase(lines_.begin() + y0 + 1, lines_.begin() + y1 + 1);
    }
    dirty_ = true;
}

void Buffer::insertMultiline(size_t y, size_t x, const std::string& text) {
    if (y >= lines_.size() || text.empty()) return;
    x = clampToChar(y, x);
    std::string tail = lines_[y].substr(x);
    lines_[y].erase(x);
    size_t start = 0;
    size_t row = y;
    while (true) {
        size_t nl = text.find('\n', start);
        std::string part = (nl == std::string::npos)
                               ? text.substr(start)
                               : text.substr(start, nl - start);
        if (start == 0) {
            lines_[row] += part;
        } else {
            lines_.insert(lines_.begin() + row, part);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
        ++row;
    }
    lines_[row] += tail;
    dirty_ = true;
}

void Buffer::wordStartBackward(size_t y, size_t x, size_t& outY,
                               size_t& outX) const {
    if (lines_.empty()) {
        outY = 0;
        outX = 0;
        return;
    }
    if (y >= lines_.size()) y = lines_.size() - 1;
    x = clampToChar(y, x);
    // At line start: join with the previous line.
    if (x == 0) {
        if (y == 0) {
            outY = 0;
            outX = 0;
            return;
        }
        outY = y - 1;
        outX = lines_[y - 1].size();
        return;
    }
    const std::string& l = lines_[y];
    size_t p = x;
    // Skip whitespace backward, then one run of word/punct chars.
    while (p > 0) {
        size_t s = utf8::prevCharStart(l, p);
        auto [cp, len] = utf8::decode(l, s);
        (void)len;
        if (charClass(cp) != 0) break;
        p = s;
    }
    if (p == 0) {
        outY = y;
        outX = 0;
        return;
    }
    size_t s = utf8::prevCharStart(l, p);
    auto [cp0, len0] = utf8::decode(l, s);
    (void)len0;
    int cls = charClass(cp0);
    while (p > 0) {
        size_t q = utf8::prevCharStart(l, p);
        auto [cp, len] = utf8::decode(l, q);
        (void)len;
        if (charClass(cp) != cls) break;
        p = q;
    }
    outY = y;
    outX = p;
}

void Buffer::wordEndForward(size_t y, size_t x, size_t& outY,
                            size_t& outX) const {
    if (lines_.empty()) {
        outY = 0;
        outX = 0;
        return;
    }
    if (y >= lines_.size()) y = lines_.size() - 1;
    x = clampToChar(y, x);
    const std::string& l = lines_[y];
    // At line end: join with the next line.
    if (x >= l.size()) {
        if (y + 1 >= lines_.size()) {
            outY = y;
            outX = l.size();
            return;
        }
        outY = y + 1;
        outX = 0;
        return;
    }
    size_t p = x;
    while (p < l.size()) {
        auto [cp, len] = utf8::decode(l, p);
        if (len == 0) break;
        if (charClass(cp) != 0) break;
        p += len;
    }
    if (p < l.size()) {
        auto [cp0, len0] = utf8::decode(l, p);
        (void)len0;
        int cls = charClass(cp0);
        while (p < l.size()) {
            auto [cp, len] = utf8::decode(l, p);
            if (len == 0) break;
            if (charClass(cp) != cls) break;
            p += len;
        }
    }
    outY = y;
    outX = p;
}

void Buffer::setLines(const std::vector<std::string>& l) {
    lines_ = l;
    if (lines_.empty()) lines_.push_back("");
    dirty_ = true;
}

size_t Buffer::clampToChar(size_t y, size_t x) const {
    const std::string& l = line(y);
    if (x > l.size()) return l.size();
    while (x > 0 && x < l.size() &&
           utf8::isContinuation(static_cast<unsigned char>(l[x]))) {
        --x;
    }
    return x;
}

size_t Buffer::lineEndX(size_t y) const { return line(y).size(); }
