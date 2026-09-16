#include "Markdown.hpp"

#include "Kitty.hpp"

namespace {

// --- inline parser -------------------------------------------------------
// Code spans first (opaque), then **bold**, __bold__, *italic*, _italic_,
// ~~strike~~, [link](url). Markers stay in the source; spans cover content.

void pushSpan(std::vector<Span>& out, size_t start, size_t len, size_t srcStart,
              size_t srcEnd, bool bold, bool italic, bool code, bool strike,
              bool link) {
    if (len == 0) return;
    Span s;
    s.start = start;
    s.len = len;
    s.srcStart = srcStart;
    s.srcEnd = srcEnd;
    s.bold = bold;
    s.italic = italic;
    s.code = code;
    s.strike = strike;
    s.linkText = link;
    out.push_back(s);
}

// Find `needle` in line starting at `from`. Returns npos if missing.
size_t findFrom(const std::string& line, const std::string& needle, size_t from) {
    if (from > line.size()) return std::string::npos;
    return line.find(needle, from);
}

std::vector<Span> parseInline(const std::string& line, size_t from,
                             std::vector<std::pair<size_t, size_t>>& conceal) {
    std::vector<Span> spans;
    // Mask of bytes claimed by code spans (no nested styling inside).
    std::vector<char> inCode(line.size(), 0);
    // 1) code spans `...`
    for (size_t i = from; i < line.size();) {
        if (line[i] != '`') {
            ++i;
            continue;
        }
        size_t close = findFrom(line, "`", i + 1);
        if (close == std::string::npos) break;
        for (size_t k = i; k <= close && k < inCode.size(); ++k) inCode[k] = 1;
        pushSpan(spans, i + 1, close - (i + 1), i, close + 1, false, false,
                 true, false, false);
        conceal.emplace_back(i, i + 1);
        conceal.emplace_back(close, close + 1);
        i = close + 1;
    }
    auto claimed = [&](size_t a, size_t b) {
        for (size_t k = a; k < b && k < inCode.size(); ++k)
            if (inCode[k]) return true;
        return false;
    };
    // 2) strong + strike (two-char delimiters).
    const char* pairs[][2] = {{nullptr, nullptr}};  // placeholder, real loop below
    (void)pairs;
    struct Delim {
        const char* open;
        bool bold;
        bool italic;
        bool strike;
    };
    const Delim delims[] = {
        {"**", true, false, false},
        {"__", true, false, false},
        {"~~", false, false, true},
    };
    for (const auto& d : delims) {
        std::string o = d.open;
        for (size_t i = from; i < line.size();) {
            size_t a = findFrom(line, o, i);
            if (a == std::string::npos) break;
            size_t b = findFrom(line, o, a + o.size());
            if (b == std::string::npos) break;
            size_t cs = a + o.size(), cl = (b > cs) ? b - cs : 0;
            if (cl > 0 && !claimed(a, b + o.size())) {
                pushSpan(spans, cs, cl, a, b + o.size(), d.bold, d.italic,
                         false, d.strike, false);
                conceal.emplace_back(a, a + o.size());
                conceal.emplace_back(b, b + o.size());
            }
            i = b + o.size();
        }
    }
    // 3) emphasis * and _ (single char, skip when part of ** or __).
    for (char m : {'*', '_'}) {
        for (size_t i = from; i < line.size();) {
            if (line[i] != m) {
                ++i;
                continue;
            }
            // Skip ** / __ runs (handled above).
            if (i + 1 < line.size() && line[i + 1] == m) {
                i += 2;
                continue;
            }
            size_t j = i + 1;
            size_t close = std::string::npos;
            while (j < line.size()) {
                if (line[j] == m && !(j + 1 < line.size() && line[j + 1] == m)) {
                    close = j;
                    break;
                }
                ++j;
            }
            if (close == std::string::npos) break;
            if (close > i + 1 && !claimed(i, close + 1)) {
                pushSpan(spans, i + 1, close - (i + 1), i, close + 1, false,
                         true, false, false, false);
                conceal.emplace_back(i, i + 1);
                conceal.emplace_back(close, close + 1);
            }
            i = close + 1;
        }
    }
    // 4) links [text](url): style the text part.
    for (size_t i = from; i < line.size();) {
        size_t a = findFrom(line, "[", i);
        if (a == std::string::npos) break;
        size_t b = findFrom(line, "]", a + 1);
        if (b == std::string::npos || b + 1 >= line.size() || line[b + 1] != '(') {
            i = a + 1;
            continue;
        }
        size_t c = findFrom(line, ")", b + 2);
        if (c == std::string::npos) {
            i = a + 1;
            continue;
        }
        if (b > a + 1 && !claimed(a, c + 1)) {
            pushSpan(spans, a + 1, b - (a + 1), a, c + 1, false, false, false,
                     false, true);
            conceal.emplace_back(a, a + 1);
            conceal.emplace_back(b, c + 1);
        }
        i = c + 1;
    }
    return spans;
}

}  // namespace

ParsedLine parseLine(const std::string& line, bool& inFence) {
    ParsedLine p;
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;

    // Fence delimiters toggle code-block mode.
    if (line.compare(i, 3, "```") == 0) {
        p.block = BlockType::CodeFence;
        p.marker = line.substr(0, i + 3);
        p.contentStart = i + 3;
        inFence = !inFence;
        return p;
    }
    if (inFence) {
        p.block = BlockType::CodeBlock;
        p.contentStart = 0;
        return p;
    }
    if (line.empty()) {
        p.block = BlockType::Empty;
        return p;
    }
    // ATX headers: 1-6 '#' followed by space/EOL.
    if (i < line.size() && line[i] == '#') {
        size_t j = i;
        while (j < line.size() && line[j] == '#') ++j;
        size_t level = j - i;
        if (level >= 1 && level <= 6 &&
            (j >= line.size() || line[j] == ' ' || line[j] == '\t')) {
            p.headerLevel = static_cast<int>(level);
            p.block = static_cast<BlockType>(static_cast<int>(BlockType::H1) + level - 1);
            size_t cs = j;
            if (cs < line.size()) ++cs;  // skip one space
            p.marker = line.substr(0, cs);
            p.contentStart = cs;
            p.spans = parseInline(line, cs, p.conceal);
            return p;
        }
    }
    // Horizontal rule: ---, ***, ___ (only those chars + spaces).
    {
        size_t dash = 0, star = 0, under = 0, other = 0;
        for (char c : line) {
            if (c == '-') ++dash;
            else if (c == '*') ++star;
            else if (c == '_') ++under;
            else if (c != ' ' && c != '\t') ++other;
        }
        size_t tot = dash + star + under;
        if (other == 0 && tot >= 3 && (dash == tot || star == tot || under == tot)) {
            p.block = BlockType::HRule;
            p.contentStart = 0;
            return p;
        }
    }
    // Blockquote.
    if (i < line.size() && line[i] == '>') {
        p.block = BlockType::Quote;
        size_t cs = i + 1;
        if (cs < line.size() && line[cs] == ' ') ++cs;
        p.marker = line.substr(0, cs);
        p.contentStart = cs;
        p.spans = parseInline(line, cs, p.conceal);
        return p;
    }
    // Unordered list.
    if (i < line.size() && (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
        i + 1 < line.size() && (line[i + 1] == ' ' || line[i + 1] == '\t')) {
        p.block = BlockType::UList;
        p.marker = line.substr(0, i + 2);
        p.contentStart = i + 2;
        p.spans = parseInline(line, i + 2, p.conceal);
        return p;
    }
    // Ordered list: digits + '.' + space.
    {
        size_t j = i;
        while (j < line.size() && line[j] >= '0' && line[j] <= '9') ++j;
        if (j > i && j + 1 < line.size() && line[j] == '.' &&
            (line[j + 1] == ' ' || line[j + 1] == '\t')) {
            p.block = BlockType::OList;
            p.marker = line.substr(0, j + 2);
            p.contentStart = j + 2;
            p.spans = parseInline(line, j + 2, p.conceal);
            return p;
        }
    }
    p.block = BlockType::Paragraph;
    p.contentStart = 0;
    p.spans = parseInline(line, 0, p.conceal);
    return p;
}

HeaderStyle headerStyle(const ParsedLine& p) {
    HeaderStyle st;
    switch (p.block) {
        case BlockType::H1:
            st.rows = 2;
            st.s = 2;
            break;
        case BlockType::H2:
            // 2 rows at ~1.5x, vertically centered.
            st.rows = 2;
            st.s = 2;
            st.n = 3;
            st.d = 4;
            st.v = 2;
            break;
        case BlockType::H3:
            // 2 rows at ~1x (airy/padded), vertically centered.
            st.rows = 2;
            st.s = 2;
            st.n = 1;
            st.d = 2;
            st.v = 2;
            break;
        default:
            break;
    }
    return st;
}

std::string spanSgr(const Span& s) {
    std::string out;
    if (s.bold) out += sgr::kBold;
    if (s.italic) out += sgr::kItalic;
    if (s.strike) out += sgr::kStrike;
    if (s.code) out += sgr::kCode;
    if (s.linkText) out += sgr::kLink;
    return out;
}

const char* headerSgr(int level) {
    switch (level) {
        case 1: return sgr::kH1;
        case 2: return sgr::kH2;
        case 3: return sgr::kH3;
        case 4: return sgr::kH4;
        case 5: return sgr::kH5;
        default: return sgr::kH6;
    }
}
