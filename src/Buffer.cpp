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
