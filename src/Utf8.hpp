#pragma once
// Small UTF-8 + cell-width helpers shared by Buffer/Markdown/Renderer.
// Width table is intentionally approximate (ASCII fast path + common
// wide/combining ranges). Exact widths for ambiguous glyphs are enforced
// on the terminal side via the Kitty text-sizing `w=` key.

#include <cstdint>
#include <string>

namespace utf8 {

// Length of UTF-8 sequence from lead byte. Returns 0 for continuation bytes.
inline size_t seqLen(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

inline bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// Decode code point starting at byte offset i. Returns {cp, len}.
// On invalid data returns {0xFFFD, 1}.
inline std::pair<uint32_t, size_t> decode(const std::string& s, size_t i) {
    if (i >= s.size()) return {0, 0};
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80) return {c0, 1};
    size_t want = seqLen(c0);
    if (want < 2 || want > 4 || i + want > s.size()) return {0xFFFD, 1};
    for (size_t k = 1; k < want; ++k)
        if (!isContinuation(static_cast<unsigned char>(s[i + k])))
            return {0xFFFD, 1};
    uint32_t cp = 0;
    if (want == 2) {
        cp = ((c0 & 0x1F) << 6) | (s[i + 1] & 0x3F);
        if (cp < 0x80) return {0xFFFD, 1};
    } else if (want == 3) {
        cp = ((c0 & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return {0xFFFD, 1};
    } else {
        cp = ((c0 & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
             ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return {0xFFFD, 1};
    }
    return {cp, want};
}

inline std::string encode(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

// Byte offset of the start of the previous character before byte pos.
inline size_t prevCharStart(const std::string& s, size_t pos) {
    if (pos == 0 || pos > s.size()) return 0;
    size_t p = pos - 1;
    while (p > 0 && isContinuation(static_cast<unsigned char>(s[p]))) --p;
    return p;
}

// Approximate terminal cell width of a code point: 0, 1, or 2.
inline int charWidth(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    // Combining marks / variation selectors / ZWJ -> zero width.
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D || cp == 0xFE0E ||
        cp == 0xFE0F) {
        return 0;
    }
    // Wide: CJK, Hangul, fullwidth, common emoji blocks.
    if ((cp >= 0x1100 && cp <= 0x115F) || cp == 0x2329 || cp == 0x232A ||
        (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0xA4CF) ||
        (cp >= 0xA960 && cp <= 0xA97C) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD) || (cp >= 0x1F1E6 && cp <= 0x1F1FF)) {
        return 2;
    }
    return 1;
}

// Cell width of a whole UTF-8 string.
inline size_t strWidth(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        auto [cp, len] = decode(s, i);
        if (len == 0) break;
        w += charWidth(cp);
        i += len;
    }
    return w;
}

// Number of code points (display column basis for the status bar).
inline size_t charCount(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        auto [cp, len] = decode(s, i);
        (void)cp;
        if (len == 0) break;
        ++n;
        i += len;
    }
    return n;
}

// Code-point index -> byte offset (clamped).
inline size_t byteOffsetForChar(const std::string& s, size_t charIdx) {
    size_t i = 0, n = 0;
    while (i < s.size() && n < charIdx) {
        auto [cp, len] = decode(s, i);
        (void)cp;
        if (len == 0) break;
        i += len;
        ++n;
    }
    return i;
}

}  // namespace utf8
