#include "Renderer.hpp"

#include <algorithm>
#include <ctime>
#include <numeric>

#include "Buffer.hpp"
#include "Markdown.hpp"
#include "Terminal.hpp"
#include "Utf8.hpp"

namespace {

// Split a header marker (indent + '#' run + one separator) so only the
// '#' run gets Kitty scaling; indent and the separator stay 1-cell.
struct MarkerParts {
    size_t hstart = 0;  // first '#'
    size_t hend = 0;    // one past last '#'
    size_t msize = 0;   // marker byte length clamped to line
};

MarkerParts splitHeaderMarker(const std::string& line, size_t markerLen) {
    MarkerParts m;
    m.msize = std::min(markerLen, line.size());
    m.hstart = m.msize;
    for (size_t i = 0; i < m.msize; ++i) {
        if (line[i] == '#') {
            m.hstart = i;
            break;
        }
    }
    m.hend = m.hstart;
    while (m.hend < m.msize && line[m.hend] == '#') ++m.hend;
    return m;
}

// Display cells of line[from,to) with tab stops every 8 from baseCol.
size_t cellsBefore(const std::string& line, size_t from, size_t to,
                   size_t baseCol = 0) {
    size_t col = baseCol;
    size_t i = from;
    if (i > line.size()) i = line.size();
    if (to > line.size()) to = line.size();
    while (i < to) {
        auto [cp, len] = utf8::decode(line, i);
        if (len == 0) break;
        if (cp == '\t') {
            col = (col / 8 + 1) * 8;
        } else {
            col += utf8::charWidth(cp);
        }
        i += len;
    }
    return col;
}

// Tab-expanded copy of a line's content region (tabs -> spaces, 8-stops
// from baseCol), with contentStart/spans remapped. Needed because raw tabs
// inside Kitty sizing escapes don't behave like terminal tab stops.
struct ExpandedLine {
    std::string text;
    size_t contentStart = 0;
    std::vector<Span> spans;
    std::vector<std::pair<size_t, size_t>> skip;
    std::vector<size_t> byteMap;  // original byte offset -> text offset
};

ExpandedLine expandTabs(const std::string& line, size_t contentStart,
                        const std::vector<Span>& spans,
                        const std::vector<std::pair<size_t, size_t>>& skip,
                        size_t baseCol) {
    ExpandedLine out;
    size_t prefix = std::min(contentStart, line.size());
    out.text = line.substr(0, prefix);
    std::vector<size_t> map(line.size() + 1, 0);
    for (size_t k = 0; k <= prefix; ++k) map[k] = k;  // identity for prefix
    size_t col = baseCol;
    size_t i = prefix;
    while (i < line.size()) {
        auto [cp, len] = utf8::decode(line, i);
        if (len == 0) break;
        map[i] = out.text.size();
        for (size_t k = 1; k < len && i + k <= line.size(); ++k)
            map[i + k] = out.text.size();
        if (cp == '\t') {
            size_t next = (col / 8 + 1) * 8;
            out.text.append(next - col, ' ');
            col = next;
        } else {
            out.text.append(line, i, len);
            col += static_cast<size_t>(utf8::charWidth(cp));
        }
        i += len;
    }
    map[line.size()] = out.text.size();
    auto remap = [&](size_t off) {
        if (off > line.size()) off = line.size();
        return map[off];
    };
    out.contentStart = remap(contentStart);
    out.spans = spans;
    for (auto& s : out.spans) {
        size_t ns = remap(s.start);
        size_t ne = remap(s.start + s.len);
        s.start = ns;
        s.len = (ne > ns) ? ne - ns : 0;
        size_t rs = remap(s.srcStart);
        size_t re = remap(s.srcEnd);
        s.srcStart = rs;
        s.srcEnd = (re > rs) ? re : rs;
    }
    out.skip = skip;
    for (auto& r : out.skip) {
        size_t ns = remap(r.first);
        size_t ne = remap(r.second);
        r.first = ns;
        r.second = (ne > ns) ? ne : ns;
    }
    out.byteMap = map;
    return out;
}

Style styleAt(const std::vector<Span>& spans, size_t byteOff) {
    Style st;
    for (const auto& s : spans) {
        if (byteOff >= s.start && byteOff < s.start + s.len) {
            st.bold |= s.bold;
            st.italic |= s.italic;
            st.code |= s.code;
            st.strike |= s.strike;
            st.link |= s.linkText;
        }
    }
    return st;
}

std::string styleSgr(const Style& st) {
    std::string out;
    if (st.bold) out += sgr::kBold;
    if (st.italic) out += sgr::kItalic;
    if (st.strike) out += sgr::kStrike;
    if (st.code) out += sgr::kCode;
    if (st.link) out += sgr::kLink;
    return out;
}

// Plain line with byte range [a,b) reverse-highlighted. Endpoints must
// already be clamped to character boundaries.
std::string highlightPlain(const std::string& line, size_t a, size_t b) {
    if (a >= b || a >= line.size()) return line;
    if (b > line.size()) b = line.size();
    return line.substr(0, a) + sgr::kReverse + line.substr(a, b - a) +
           "\x1b[27m" + line.substr(b);
}

std::string cup(int row1, int col1) {
    return "\x1b[" + std::to_string(row1) + ";" + std::to_string(col1) + "H";
}

// Visible cell width ignoring SGR sequences.
size_t visibleWidth(const std::string& s) {
    size_t w = 0, i = 0;
    while (i < s.size()) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && s[i] != 'm') ++i;
            if (i < s.size()) ++i;
        } else {
            auto [cp, len] = utf8::decode(s, i);
            if (len == 0) break;
            w += static_cast<size_t>(utf8::charWidth(cp));
            i += len;
        }
    }
    return w;
}

// End-truncate to maxCells cells at a codepoint boundary.
std::string truncateCells(const std::string& s, size_t maxCells) {
    size_t w = 0, i = 0;
    while (i < s.size()) {
        auto [cp, len] = utf8::decode(s, i);
        if (len == 0) break;
        size_t cw = static_cast<size_t>(utf8::charWidth(cp));
        if (w + cw > maxCells) break;
        w += cw;
        i += len;
    }
    return s.substr(0, i);
}

// Terminal rows a logical line occupies. Headers are single-row: the `#`
// marker renders inline (same size as the text), never on its own row.
int blockRows(const ParsedLine& p, bool scaled) {
    int rows = 1;
    if (scaled) {
        rows = headerStyle(p).rows;
    }
    return rows;
}

// Merge overlapping ranges (defensive; the parser emits disjoint ones).
std::vector<std::pair<size_t, size_t>> mergedRanges(
    std::vector<std::pair<size_t, size_t>> ranges) {
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<size_t, size_t>> out;
    for (const auto& r : ranges) {
        if (!out.empty() && r.first <= out.back().second) {
            out.back().second = std::max(out.back().second, r.second);
        } else {
            out.push_back(r);
        }
    }
    return out;
}

// Conceal ranges minus spans revealed by the cursor or the selection:
// a span reveals (shows raw source) when the cursor lies in its full
// source extent, or when the selection overlaps its source extent.
// Without either on the line, everything concealed stays hidden.
std::vector<std::pair<size_t, size_t>> skipRangesForSel(
    const std::vector<Span>& spans,
    const std::vector<std::pair<size_t, size_t>>& conceal, bool hasCursor,
    size_t cx, bool selOnLine, size_t selA, size_t selB) {
    if (!hasCursor && !selOnLine) return mergedRanges(conceal);
    std::vector<std::pair<size_t, size_t>> keep;
    for (const auto& r : conceal) {
        bool revealed = false;
        for (const auto& s : spans) {
            if (r.first < s.srcStart || r.second > s.srcEnd) continue;
            if (hasCursor && cx >= s.srcStart && cx <= s.srcEnd) {
                revealed = true;
                break;
            }
            if (selOnLine && selA <= s.srcEnd && selB >= s.srcStart) {
                revealed = true;
                break;
            }
        }
        if (!revealed) keep.push_back(r);
    }
    return mergedRanges(keep);
}

// Split content into emitted segments, skipping concealed ranges.
// ASCII runs group by style (flushed past 3000B); with fractional packing
// (packN/packD, H2/H3) ASCII groups carry explicit w so advance matches the
// shrunken glyphs: smallest exact group first (combined while w <= 7),
// remainders via ceil (safe: slight gap, never truncation).
// Every other grapheme is its own segment with explicit width.
// skipped must be merged.
std::vector<Segment> layoutContent(
    const std::string& text, size_t cs, const std::vector<Span>& spans,
    const std::vector<std::pair<size_t, size_t>>& skipped, int packN,
    int packD, size_t selStart, size_t selEnd) {
    int G0 = 0, W0 = 0, maxK = 0;
    if (packN > 0 && packD > packN) {
        int g = std::gcd(packN, packD);
        G0 = packD / g;
        W0 = packN / g;
        maxK = W0 > 0 ? 7 / W0 : 0;
        if (maxK < 1) maxK = 1;
    }
    std::vector<Segment> segs;
    size_t skipIdx = 0;
    size_t i = cs;
    size_t runStart = cs;
    Style curStyle{};
    bool haveCur = false;
    auto flushRun = [&](size_t end) {
        if (end <= runStart) return;
        if (G0 == 0) {
            Segment g;
            g.start = runStart;
            g.end = end;
            g.style = curStyle;
            g.w = 0;
            segs.push_back(g);
            return;
        }
        // Pure ASCII run: bytes == chars. Full exact groups first
        // (combined while w <= 7), then a ceil-rounded remainder.
        size_t total = end - runStart;
        size_t p = runStart;
        while (total > 0) {
            size_t take;
            int w;
            size_t fullGroups = total / static_cast<size_t>(G0);
            if (fullGroups > 0) {
                size_t k = std::min(fullGroups, static_cast<size_t>(maxK));
                take = k * static_cast<size_t>(G0);
                w = static_cast<int>(k) * W0;
            } else {
                take = total;
                w = static_cast<int>((take * static_cast<size_t>(packN) +
                                      static_cast<size_t>(packD) - 1) /
                                     static_cast<size_t>(packD));
                if (w < 1) w = 1;
                if (w > 7) w = 7;
            }
            Segment g;
            g.start = p;
            g.end = p + take;
            g.style = curStyle;
            g.w = w;
            g.chars = static_cast<int>(take);
            segs.push_back(g);
            p += take;
            total -= take;
        }
    };
    while (i < text.size()) {
        if (selStart < selEnd && (i == selStart || i == selEnd)) {
            flushRun(i);
            runStart = i;
        }
        auto [cp, len] = utf8::decode(text, i);
        if (len == 0) break;
        while (skipIdx < skipped.size() && i >= skipped[skipIdx].second)
            ++skipIdx;
        if (skipIdx < skipped.size() && i >= skipped[skipIdx].first) {
            flushRun(i);
            runStart = i + len;
            i = runStart;
            continue;
        }
        Style st = styleAt(spans, i);
        int w = utf8::charWidth(cp);
        if (!(cp < 0x80 && w == 1)) {
            flushRun(i);
            int cw = w <= 0 ? 1 : w;
            Segment g;
            g.start = i;
            g.end = i + len;
            g.style = st;
            if (G0 == 0) {
                g.w = cw;
            } else {
                int ww = (cw * packN + packD - 1) / packD;
                if (ww < 1) ww = 1;
                if (ww > 7) ww = 7;
                g.w = ww;
            }
            g.chars = 1;
            segs.push_back(g);
            i += len;
            runStart = i;
            haveCur = false;
            continue;
        }
        if (!haveCur) {
            curStyle = st;
            haveCur = true;
        } else if (st != curStyle) {
            flushRun(i);
            curStyle = st;
            runStart = i;
        }
        i += len;
        if (i - runStart >= 3000) {
            flushRun(i);
            runStart = i;  // keep style: no redundant SGR mid-run
        }
    }
    flushRun(i);
    return segs;
}

// Count ASCII width-1 chars in [a,b) (packed groups are pure ASCII).
size_t countAscii1(const std::string& text, size_t a, size_t b) {
    size_t n = 0;
    size_t i = std::min(a, text.size());
    size_t end = std::min(b, text.size());
    while (i < end) {
        auto [cp, len] = utf8::decode(text, i);
        if (len == 0) break;
        if (cp < 0x80 && utf8::charWidth(cp) == 1) ++n;
        i += len;
    }
    return n;
}

// Display advance of [.., upto) in columns from baseCol over segments.
// Tabs use absolute 8-stops; sized explicit-w segments advance whole, with
// proportional (floored) partials inside packed ASCII groups.
size_t advanceUpTo(const std::string& text, const std::vector<Segment>& segs,
                   int s, bool useW, size_t baseCol, size_t upto) {
    size_t col = baseCol;
    for (const auto& g : segs) {
        if (upto <= g.start) break;
        size_t e = std::min(upto, g.end);
        if (useW && g.w > 0) {
            if (e >= g.end) {
                col += static_cast<size_t>(s * g.w);
            } else {
                size_t k = countAscii1(text, g.start, e);
                int c = g.chars > 0 ? g.chars : 1;
                col += (k * static_cast<size_t>(s) *
                        static_cast<size_t>(g.w)) /
                       static_cast<size_t>(c);
            }
        } else {
            size_t k = g.start;
            while (k < e) {
                auto [cp, len] = utf8::decode(text, k);
                if (len == 0) break;
                if (cp == '\t') {
                    col = (col / 8 + 1) * 8;
                } else {
                    col += static_cast<size_t>(s * utf8::charWidth(cp));
                }
                k += len;
            }
        }
    }
    return col - baseCol;
}

}  // namespace

Renderer::Renderer(Terminal& term, Buffer& buf, const KittySupport& supp)
    : term_(term), buf_(buf), supp_(supp) {}

void Renderer::ensureVisible(size_t cy, int viewRows) {
    size_t n = buf_.lineCount();
    if (n == 0) {
        topLine_ = 0;
        return;
    }
    if (cy >= n) cy = n - 1;
    if (topLine_ >= n) topLine_ = n - 1;
    if (cy < topLine_) {
        topLine_ = cy;
        return;
    }
    // Parse from topLine_ to cy accumulating scale; shift down if overflow.
    // (Re-parse is O(file); fine for typical md files.)
    bool inFence = false;
    for (size_t i = 0; i < topLine_; ++i) {
        ParsedLine tmp = parseLine(buf_.line(i), inFence);
        (void)tmp;
    }
    while (true) {
        bool fence = inFence;
        int used = 0;
        for (size_t i = topLine_; i <= cy && i < n; ++i) {
            bool f = fence;
            ParsedLine p = parseLine(buf_.line(i), f);
            used += blockRows(p, supp_.scale);
            fence = f;
        }
        if (used <= viewRows || topLine_ >= cy) break;
        // Advance topLine_ past one line (recompute fence cheaply next loop).
        ++topLine_;
        inFence = false;
        for (size_t i = 0; i < topLine_; ++i) {
            ParsedLine tmp = parseLine(buf_.line(i), inFence);
            (void)tmp;
        }
    }
}

LineLayout Renderer::layoutLine(const std::string& line, size_t contentStart,
                                const std::vector<Span>& spans,
                                const std::vector<std::pair<size_t, size_t>>& skip,
                                const HeaderStyle& style, size_t markerCells,
                                size_t selStart, size_t selEnd) {
    LineLayout lay;
    lay.sized = supp_.scale && style.s > 1;
    lay.contentStart = contentStart;
    lay.spans = spans;
    lay.skip = skip;
    if (lay.sized &&
        line.find('\t', std::min(contentStart, line.size())) !=
            std::string::npos) {
        // Tabs inside sizing escapes don't act as tab stops: expand content
        // tabs to spaces first (cursor math maps original offsets through
        // byteMap, so columns stay consistent).
        ExpandedLine owned =
            expandTabs(line, contentStart, spans, skip, markerCells);
        lay.text = std::move(owned.text);
        lay.contentStart = owned.contentStart;
        lay.spans = std::move(owned.spans);
        lay.skip = std::move(owned.skip);
        lay.byteMap = std::move(owned.byteMap);
    } else {
        lay.text = line;
    }
    lay.selStart = std::min(selStart, line.size());
    lay.selEnd = std::min(selEnd, line.size());
    if (!lay.byteMap.empty()) {
        lay.selStart = lay.byteMap[lay.selStart];
        lay.selEnd = lay.byteMap[lay.selEnd];
    }
    lay.segs = layoutContent(lay.text, lay.contentStart, lay.spans, lay.skip,
                             (lay.sized && style.n > 0 && style.d > style.n)
                                 ? style.n
                                 : 0,
                             (lay.sized && style.n > 0 && style.d > style.n)
                                 ? style.d
                                 : 0, lay.selStart, lay.selEnd);
    return lay;
}

void Renderer::emitLayout(const LineLayout& lay, const std::string& baseSgr,
                          const HeaderStyle& style) {
    std::string out;
    out.reserve(lay.text.size() + 64);
    // Base SGR for the whole content (header color etc.).
    out += baseSgr;
    Style curStyle{};
    bool haveCur = false;
    bool rev = false;  // selection reverse currently active
    // Selection uses reverse-video toggles (7/27) so bold/color survive;
    // 0m resets clear it, so re-apply after every style change.
    for (const auto& g : lay.segs) {
        bool gsel = lay.selStart < lay.selEnd &&
                    g.start >= lay.selStart && g.end <= lay.selEnd;
        if (!haveCur || g.style != curStyle) {
            if (haveCur) {
                out += sgr::kReset;
                out += baseSgr;
                rev = false;
            }
            out += styleSgr(g.style);
            curStyle = g.style;
            haveCur = true;
        }
        if (gsel && !rev) {
            out += sgr::kReverse;
            rev = true;
        } else if (!gsel && rev) {
            out += "\x1b[27m";
            rev = false;
        }
        std::string chunk = lay.text.substr(g.start, g.end - g.start);
        if (!lay.sized) {
            if (supp_.width && g.w > 1) {
                out += kitty::sized(chunk, 1, g.w);  // pin width, no scaling
            } else {
                out += chunk;
            }
        } else if (g.w == 0) {
            // Auto-split into s-by-s cells (split defensively under 4096B).
            size_t pos = 0;
            while (pos < chunk.size()) {
                size_t n = std::min<size_t>(3000, chunk.size() - pos);
                out += kitty::sized(chunk.substr(pos, n), style.s, 0,
                                    style.n, style.d, style.v, style.h);
                pos += n;
            }
        } else {
            int w = g.w > 7 ? 7 : g.w;
            out += kitty::sized(chunk, style.s, w, style.n, style.d,
                                style.v, style.h);
        }
    }
    if (rev) out += "\x1b[27m";
    out += sgr::kReset;
    term_.writeStr(out);
}

bool Renderer::screenToLogical(int sx, int sy, size_t curCx, size_t curCy,
                               bool selActive, size_t selAx, size_t selAy,
                               size_t& outCx, size_t& outCy) {
    int rows = term_.rows();
    int cols = term_.cols();
    // Clicks on the status pills (last three rows) are not content.
    int contentLast = rows >= 4 ? rows - 3 : rows - 1;
    if (sx < 1 || sx > cols || sy < 1 || sy > contentLast) return false;
    size_t n = buf_.lineCount();
    if (n == 0 || topLine_ >= n) return false;
    // Normalized selection (anchor vs cursor head), clamped to lines.
    bool selOn = selActive && (selAx != curCx || selAy != curCy);
    size_t selY0 = 0, selX0 = 0, selY1 = 0, selX1 = 0;
    if (selOn) {
        size_t ay = selAy < n ? selAy : n - 1;
        size_t hy = curCy < n ? curCy : n - 1;
        selY0 = ay;
        selX0 = buf_.clampToChar(ay, selAx);
        selY1 = hy;
        selX1 = buf_.clampToChar(hy, curCx);
        if (selY1 < selY0 || (selY1 == selY0 && selX1 < selX0)) {
            std::swap(selY0, selY1);
            std::swap(selX0, selX1);
        }
    }
    int target = sy - 1;  // 0-based screen row
    // Fence-aware walk from topLine_, mirroring draw().
    bool inFence = false;
    for (size_t y = 0; y < topLine_; ++y) {
        ParsedLine tmp = parseLine(buf_.line(y), inFence);
        (void)tmp;
    }
    int screenRow = 0;
    for (size_t y = topLine_; y < n; ++y) {
        const std::string& line = buf_.line(y);
        ParsedLine p = parseLine(line, inFence);
        HeaderStyle hs = supp_.scale ? headerStyle(p) : HeaderStyle{};
        int total = hs.rows;
        if (target < screenRow || target >= screenRow + total) {
            screenRow += total;
            continue;
        }
        // Click inside this logical line's block.
        outCy = y;
        bool plainKind = (p.block == BlockType::Empty ||
                          p.block == BlockType::CodeFence ||
                          p.block == BlockType::CodeBlock ||
                          p.block == BlockType::HRule);
        // Header `#` markers are hidden unless the cursor is on the line.
        bool isHeader = p.headerLevel >= 1;
        bool lineCursor = (y == curCy);
        bool showMarker = !plainKind && !p.marker.empty() &&
                          (!isHeader || lineCursor);
        size_t selA = 0, selB = 0;
        bool selOnLine = selOn && y >= selY0 && y <= selY1;
        if (selOnLine) {
            selA = (y == selY0) ? selX0 : 0;
            selB = (y == selY1) ? selX1 : line.size();
            selOnLine = selA < selB;
        }
        bool sized = supp_.scale && hs.s > 1;
        // Mirror of draw(): only the '#' run is scaled; separator is 1-cell.
        MarkerParts cmp;
        std::string chash;
        std::vector<Segment> chashSegs;
        size_t cIndent = 0, cHashAdv = 0;
        bool csizedHashes =
            showMarker && isHeader && sized && !plainKind;
        size_t markerCells = 0;
        size_t cs = 0;
        if (!plainKind && !p.marker.empty()) {
            cs = p.contentStart;
            if (csizedHashes) {
                cmp = splitHeaderMarker(line, p.marker.size());
                chash =
                    line.substr(cmp.hstart, cmp.hend - cmp.hstart);
                LineLayout hashLay = layoutLine(
                    chash, 0, {}, {}, hs, 0,
                    selA > cmp.hstart ? selA - cmp.hstart : 0,
                    selB > cmp.hstart ? selB - cmp.hstart : 0);
                chashSegs = std::move(hashLay.segs);
                cIndent = cellsBefore(line, 0, cmp.hstart);
                for (const auto& g : chashSegs) {
                    if (g.w > 0) {
                        cHashAdv +=
                            static_cast<size_t>(hs.s * g.w);
                    } else {
                        size_t k = g.start;
                        while (k < g.end) {
                            auto [cp, len] = utf8::decode(chash, k);
                            if (len == 0) break;
                            cHashAdv += static_cast<size_t>(
                                hs.s * utf8::charWidth(cp));
                            k += len;
                        }
                    }
                }
                markerCells = cIndent + cHashAdv +
                              cellsBefore(line, cmp.hend, cmp.msize);
            } else if (showMarker) {
                markerCells = cellsBefore(line, 0, p.marker.size());
            }
        }
        bool hasCursor = (y == curCy);
        auto skip0 = skipRangesForSel(p.spans, p.conceal, hasCursor,
                                      hasCursor ? curCx : 0, selOnLine, selA,
                                      selB);
        std::vector<Span> useSpans = plainKind ? std::vector<Span>{} : p.spans;
        LineLayout lay = layoutLine(line, cs, useSpans, skip0, hs,
                                    markerCells, selA, selB);
        auto originalOffset = [&](size_t off) {
            if (lay.byteMap.empty()) return buf_.clampToChar(y, off);
            auto it = std::upper_bound(lay.byteMap.begin(), lay.byteMap.end(),
                                       off);
            size_t original = it == lay.byteMap.begin()
                                  ? 0
                                  : static_cast<size_t>(it - lay.byteMap.begin() - 1);
            return buf_.clampToChar(y, original);
        };
        int s = lay.sized ? hs.s : 1;
        long rel = static_cast<long>(sx - 1) - static_cast<long>(markerCells);
        // CodeBlock rows render with a one-cell display pad: clicks on
        // the pad gutter map to offset 0, the rest shifts by one.
        if (p.block == BlockType::CodeBlock) {
            if (rel <= 0) {
                outCx = 0;
                return true;
            }
            rel -= 1;
        }
        if (rel < 0) {
            if (csizedHashes) {
                // Region-aware walk: unsized indent, scaled '#' run,
                // unsized separator.
                long r = rel + static_cast<long>(markerCells);
                if (r < static_cast<long>(cIndent)) {
                    size_t k = 0, col = 0;
                    while (k < cmp.hstart) {
                        auto [cp, len] = utf8::decode(line, k);
                        if (len == 0) break;
                        size_t gad =
                            (cp == '\t')
                                ? ((col / 8 + 1) * 8 - col)
                                : static_cast<size_t>(
                                      utf8::charWidth(cp));
                        if (r < static_cast<long>(col + gad)) break;
                        col += gad;
                        k += len;
                    }
                    outCx = k;
                    return true;
                }
                long hr = r - static_cast<long>(cIndent);
                size_t acc = 0;
                for (const auto& g : chashSegs) {
                    size_t segAdv = 0;
                    if (g.w > 0) {
                        segAdv = static_cast<size_t>(hs.s * g.w);
                    } else {
                        size_t k = g.start;
                        while (k < g.end) {
                            auto [cp, len] = utf8::decode(chash, k);
                            if (len == 0) break;
                            segAdv += static_cast<size_t>(
                                hs.s * utf8::charWidth(cp));
                            k += len;
                        }
                    }
                    if (hr < static_cast<long>(acc + segAdv)) {
                        int c = g.chars > 0 ? g.chars : 1;
                        size_t idx =
                            (static_cast<size_t>(hr) - acc) *
                                static_cast<size_t>(c) /
                            (segAdv >= 1 ? segAdv : 1);
                        if (idx >= static_cast<size_t>(c))
                            idx = static_cast<size_t>(c) - 1;
                        outCx = cmp.hstart + g.start + idx;
                        if (outCx > cmp.hend) outCx = cmp.hend;
                        return true;
                    }
                    acc += segAdv;
                }
                // Separator: walk unsized bytes from cmp.hend.
                size_t k = cmp.hend, col = cIndent + cHashAdv;
                while (k < cmp.msize) {
                    auto [cp, len] = utf8::decode(line, k);
                    if (len == 0) break;
                    size_t gad =
                        (cp == '\t')
                            ? ((col / 8 + 1) * 8 - col)
                            : static_cast<size_t>(utf8::charWidth(cp));
                    if (r < static_cast<long>(col + gad)) break;
                    col += gad;
                    k += len;
                }
                outCx = k;
                return true;
            }
            // Inside the inline marker: walk marker bytes at s = 1.
            size_t k = 0;
            size_t col = 0;
            size_t end = std::min(p.marker.size(), line.size());
            while (k < end) {
                auto [cp, len] = utf8::decode(line, k);
                if (len == 0) break;
                size_t gad = (cp == '\t')
                                 ? ((col / 8 + 1) * 8 - col)
                                 : static_cast<size_t>(utf8::charWidth(cp));
                if (rel < static_cast<long>(col + gad)) break;
                col += gad;
                k += len;
            }
            outCx = k;
            return true;
        }
        // Inverse segment walk from the content origin (past the
        // CodeBlock display pad, whose width rel already excludes).
        size_t abscol = markerCells +
                        (p.block == BlockType::CodeBlock ? 1 : 0);
        size_t acc = 0;
        size_t targetCol = static_cast<size_t>(rel);
        for (const auto& g : lay.segs) {
            size_t segAdv;
            if (lay.sized && g.w > 0) {
                segAdv = static_cast<size_t>(s * g.w);
            } else {
                segAdv = 0;
                size_t k = g.start;
                while (k < g.end) {
                    auto [cp, len] = utf8::decode(lay.text, k);
                    if (len == 0) break;
                    size_t cur = abscol + segAdv;
                    size_t gad;
                    if (cp == '\t') {
                        gad = (cur / 8 + 1) * 8 - cur;
                    } else {
                        gad = static_cast<size_t>(s * utf8::charWidth(cp));
                    }
                    if (targetCol < acc + segAdv + gad) {
                        outCx = originalOffset(k);
                        return true;
                    }
                    segAdv += gad;
                    k += len;
                }
            }
            if (targetCol < acc + segAdv) {
                // Inside an explicit-w segment: proportional char index.
                int c = g.chars > 0 ? g.chars : 1;
                size_t idx = (targetCol - acc) * static_cast<size_t>(c) /
                             segAdv;  // segAdv >= 1 here
                if (idx >= static_cast<size_t>(c))
                    idx = static_cast<size_t>(c) - 1;
                size_t off = (c > 1) ? std::min(g.start + idx, g.end)
                                     : g.start;
                outCx = originalOffset(off);
                return true;
            }
            acc += segAdv;
            abscol += segAdv;
        }
        outCx = line.size();  // past end -> EOL
        return true;
    }
    return false;  // filler area below content: ignore
}

void Renderer::draw(size_t cx, size_t cy, bool selActive, size_t selAx,
                    size_t selAy, const std::string& status,
                    bool promptActive, const std::string& promptText) {
    term_.refreshSize();
    int rows = term_.rows();
    int cols = term_.cols();
    size_t n = buf_.lineCount();
    if (cy >= n) cy = n == 0 ? 0 : n - 1;

    // Normalized selection, hidden while a prompt owns the screen.
    bool selOn = selActive && !promptActive && n > 0;
    size_t selY0 = 0, selX0 = 0, selY1 = 0, selX1 = 0;
    if (selOn) {
        size_t ay = selAy < n ? selAy : n - 1;
        size_t hy = cy < n ? cy : n - 1;
        selY0 = ay;
        selX0 = buf_.clampToChar(ay, selAx);
        selY1 = hy;
        selX1 = buf_.clampToChar(hy, cx);
        if (selY1 < selY0 || (selY1 == selY0 && selX1 < selX0)) {
            std::swap(selY0, selY1);
            std::swap(selX0, selX1);
        }
        selOn = (selY0 != selY1 || selX0 != selX1);
    }

    if (rows < 3 || cols < 20) {
        term_.writeStr("\x1b[H\x1b[2J");
        term_.writeStr("Window too small for quark (need 20x3).");
        term_.hideCursor();
        return;
    }
    // Status pills are 3-row bordered boxes docked at the bottom, so
    // rows >= 4 leaves rows - 3 content rows; a 3-row window gets a
    // single plain bar row.
    bool framedBar = rows >= 4;
    int viewRows = framedBar ? rows - 3 : rows - 1;
    ensureVisible(cy, viewRows);

    term_.hideCursor();
    term_.writeStr("\x1b[H\x1b[2J");

    // Single pass from line 0 to keep fence state correct.
    bool inFence = false;
    int screenRow = 0;
    size_t cursorScreenRow = 0, cursorScreenCol = 1;
    bool cursorPlaced = false;

    // Precompute fence-correct parses for visible lines only; scan prefix.
    for (size_t y = 0; y < topLine_ && y < n; ++y) {
        ParsedLine tmp = parseLine(buf_.line(y), inFence);
        (void)tmp;
    }
    for (size_t y = topLine_; y < n; ++y) {
        bool fenced = inFence;
        (void)fenced;
        const std::string& line = buf_.line(y);
        ParsedLine p = parseLine(line, inFence);
        HeaderStyle hs = supp_.scale ? headerStyle(p) : HeaderStyle{};
        int sc = hs.rows;
        if (screenRow + sc > viewRows) break;  // never draw partial blocks
        term_.writeStr(cup(screenRow + 1, 1));
        term_.writeStr("\x1b[2K");  // erase stale multicells on this row

        if (p.block == BlockType::Empty) {
            // A selected empty line inside a multi-line selection paints
            // full-width so the selection reads as continuous.
            if (selOn && y > selY0 && y < selY1) {
                term_.writeStr(sgr::kReverse);
                term_.writeStr(std::string(cols > 0 ? (size_t)cols : 0, ' '));
                term_.writeStr(sgr::kReset);
            }
        } else if (p.block == BlockType::CodeFence ||
                   p.block == BlockType::CodeBlock ||
                   p.block == BlockType::HRule) {
            // Plain-text lines: highlight the selected byte range inline.
            bool selLine = selOn && y >= selY0 && y <= selY1;
            size_t selA = 0, selB = 0;
            if (selLine) {
                selA = (y == selY0) ? std::min(selX0, line.size()) : 0;
                selB = (y == selY1) ? std::min(selX1, line.size())
                                    : line.size();
                selLine = selA < selB;
            }
            std::string body =
                selLine ? highlightPlain(line, selA, selB) : line;
            if (p.block == BlockType::CodeBlock) {
                // Dark text with a one-cell display pad (offsets stay
                // line-based; cursor/click math compensates below).
                term_.writeStr(sgr::kDim);
                term_.writeStr(" ");
                term_.writeStr(body);
                term_.writeStr(sgr::kReset);
            } else if (p.block == BlockType::CodeFence) {
                // Fence delimiters stay hidden unless the cursor is on
                // the line (same reveal convention as concealed `#` and
                // inline markers); the row itself is already erased, so
                // a hidden fence just leaves a blank row.
                if ((y == cy) && !promptActive) {
                    term_.writeStr(sgr::kDim);
                    term_.writeStr(body);
                    term_.writeStr(sgr::kReset);
                }
            } else {
                term_.writeStr(sgr::kDim);
                term_.writeStr(body);
                term_.writeStr(sgr::kReset);
            }
        } else {
            // Header `#` markers are concealed unless the cursor is on the
            // line (like inline `**`/`*`/backtick markers). All other block
            // markers (`>`, `-`, `1.`) stay dimmed inline. A revealed header
            // marker renders inline at the same Kitty scale as its text.
            bool hasCursor = (y == cy) && !promptActive;
            // Selection byte range on this line (original line offsets).
            bool selLine = selOn && y >= selY0 && y <= selY1;
            size_t selA = 0, selB = 0;
            if (selLine) {
                selA = (y == selY0) ? std::min(selX0, line.size()) : 0;
                selB = (y == selY1) ? std::min(selX1, line.size())
                                    : line.size();
                selLine = selA < selB;
            }
            bool isHeader = p.headerLevel >= 1;
            bool showMarker =
                !p.marker.empty() && (!isHeader || hasCursor);
            bool sized = supp_.scale && hs.s > 1;
            // Sized-header marker pieces: only the '#' run is scaled; the
            // separator space stays 1 cell so `## Text` keeps a normal gap.
            // (Kept in scope for the cursor-advance math below.)
            MarkerParts mp;
            std::string hashText;
            LineLayout hashLay;
            size_t indentCells = 0, hashAdv = 0, sepCells = 0;
            size_t markerCells = 0;
            bool sizedHashes = showMarker && isHeader && sized;
            if (sizedHashes) {
                mp = splitHeaderMarker(line, p.marker.size());
                hashText = line.substr(mp.hstart, mp.hend - mp.hstart);
                hashLay = layoutLine(
                    hashText, 0, {}, {}, hs, 0,
                    selA > mp.hstart ? selA - mp.hstart : 0,
                    selB > mp.hstart ? selB - mp.hstart : 0);
                indentCells = cellsBefore(line, 0, mp.hstart);
                for (const auto& g : hashLay.segs) {
                    if (g.w > 0) {
                        hashAdv += static_cast<size_t>(hs.s * g.w);
                    } else {
                        size_t k = g.start;
                        while (k < g.end) {
                            auto [cp, len] =
                                utf8::decode(hashText, k);
                            if (len == 0) break;
                            hashAdv += static_cast<size_t>(
                                hs.s * utf8::charWidth(cp));
                            k += len;
                        }
                    }
                }
                sepCells = cellsBefore(line, mp.hend, mp.msize);
                markerCells = indentCells + hashAdv + sepCells;
            } else if (showMarker) {
                markerCells = cellsBefore(line, 0, p.marker.size());
            }
            if (sizedHashes) {
                term_.writeStr(sgr::kDim);
                term_.writeStr(highlightPlain(line.substr(0, mp.hstart),
                                               selA, selB));
                emitLayout(hashLay, sgr::kDim, hs);
                term_.writeStr(sgr::kDim);
                term_.writeStr(highlightPlain(
                    line.substr(mp.hend, mp.msize - mp.hend),
                    selA > mp.hend ? selA - mp.hend : 0,
                    selB > mp.hend ? selB - mp.hend : 0));
                term_.writeStr(sgr::kReset);
            } else if (showMarker) {
                term_.writeStr(sgr::kDim);
                term_.writeStr(highlightPlain(p.marker, selA, selB));
                term_.writeStr(sgr::kReset);
            }
            std::string base;
            if (p.headerLevel >= 1) base = headerSgr(p.headerLevel);
            else if (p.block == BlockType::Quote) base = sgr::kQuote;
            else if (p.block == BlockType::UList || p.block == BlockType::OList)
                base = "";  // marker colored below instead
            if ((p.block == BlockType::UList || p.block == BlockType::OList) &&
                !p.marker.empty()) {
                // marker already drawn dimmed; acceptable
            }
            // Span-level reveal: hidden formatting shows raw source when
            // the cursor is inside the formatted span's extent, or when
            // the selection overlaps it.
            // (hasCursor already computed above for marker concealment.)
            auto skip0 = skipRangesForSel(p.spans, p.conceal, hasCursor, cx,
                                          selLine, selA, selB);
            LineLayout lay = layoutLine(line, p.contentStart, p.spans, skip0,
                                        hs, markerCells, selA, selB);
            emitLayout(lay, base, hs);

            if (hasCursor) {
                size_t colCells;
                if (cx < p.contentStart && sizedHashes) {
                    if (cx <= mp.hstart) {
                        colCells = cellsBefore(line, 0, cx);
                    } else if (cx < mp.hend) {
                        colCells = indentCells +
                                   advanceUpTo(hashText, hashLay.segs, hs.s,
                                               true, 0, cx - mp.hstart);
                    } else {
                        colCells = indentCells + hashAdv +
                                   cellsBefore(line, mp.hend, cx);
                    }
                } else if (cx < p.contentStart && showMarker) {
                    colCells = cellsBefore(line, 0, cx);
                } else if (cx < p.contentStart) {
                    colCells = 0;  // hidden `#`: pin to content origin
                } else {
                    // Advance shared with emission; map original offsets
                    // through tab expansion (tabs use absolute 8-stops).
                    size_t upto = std::min(cx, line.size());
                    if (!lay.byteMap.empty() && upto < lay.byteMap.size())
                        upto = lay.byteMap[upto];
                    int s = lay.sized ? hs.s : 1;
                    colCells = markerCells +
                               advanceUpTo(lay.text, lay.segs, s, lay.sized,
                                           markerCells, upto);
                }
                cursorScreenRow = screenRow;
                cursorScreenCol = static_cast<size_t>(1) + colCells;
                if (cursorScreenCol > static_cast<size_t>(cols))
                    cursorScreenCol = cols;
                cursorPlaced = true;
            }
        }
        if (y == cy && !promptActive && !cursorPlaced &&
            (p.block == BlockType::Empty || p.block == BlockType::CodeFence ||
             p.block == BlockType::CodeBlock || p.block == BlockType::HRule)) {
            // CodeBlock rows render with a one-cell display pad (tabs
            // measured from column 1 to match); cellsBefore already
            // includes the base column.
            size_t base = (p.block == BlockType::CodeBlock) ? 1 : 0;
            size_t c = 1 + cellsBefore(line, 0, cx, base);
            if (c > static_cast<size_t>(cols)) c = cols;
            cursorScreenRow = screenRow;
            cursorScreenCol = c;
            cursorPlaced = true;
        }
        screenRow += hs.rows;
    }
    // Filler tilde rows.
    for (int r = screenRow; r < viewRows; ++r) {
        term_.writeStr(cup(r + 1, 1));
        term_.writeStr("\x1b[2K");
        term_.writeStr(sgr::kDim);
        term_.writeStr("~");
        term_.writeStr(sgr::kReset);
    }

    // Status bar: separate bordered pills, black background with white
    // text (no fills). The filename (left) and position (right) each get
    // a full rounded border; the empty middle has no frame. A transient
    // status message shows centered between the pills.
    // Draws one pill-box edge: `textW` text cells at 1-based column
    // `col`; total width is textW + 4.
    auto pillEdge = [&](int r, int col, int textW, bool top) {
        std::string dashes;
        for (int i = 0; i < textW + 2; ++i) dashes += "\u2500";  // ─
        term_.writeStr(cup(r, col));
        term_.writeStr(top ? "\u256d" + dashes + "\u256e"   // ╭ ╮
                           : "\u2570" + dashes + "\u256f");  // ╰ ╯
    };
    if (promptActive) {
        // promptText is fully formed by the caller (quit confirm includes
        // "(y/n)"; save-as shows the path being typed). Own fitted pill
        // border, centered.
        std::string shown = truncateCells(promptText, static_cast<size_t>(cols > 4 ? cols - 4 : 0));
        int tW = static_cast<int>(visibleWidth(shown));
        if (framedBar) {
            int bw = tW + 4;
            int left = (cols - bw) / 2 + 1;
            if (left < 1) left = 1;
            pillEdge(rows - 2, left, tW, true);
            term_.writeStr(cup(rows - 1, left));
            term_.writeStr("\u2502 ");  // │
            term_.writeStr(shown);
            term_.writeStr(" \u2502");  // │
            pillEdge(rows, left, tW, false);
            int cc = left + 2 + static_cast<int>(shown.size());
            if (cc > left + bw - 2) cc = left + bw - 2;
            term_.writeStr(cup(rows - 1, cc));
        } else {
            size_t start = static_cast<size_t>(tW) < static_cast<size_t>(cols)
                               ? (static_cast<size_t>(cols) -
                                  static_cast<size_t>(tW)) /
                                     2
                               : 0;
            term_.writeStr(cup(rows, 1));
            term_.writeStr("\x1b[2K");
            term_.writeStr(std::string(start, ' '));
            term_.writeStr(shown);
            int cc = static_cast<int>(start + shown.size());
            if (cc > cols) cc = cols;
            term_.writeStr(cup(rows, cc + 1));
        }
        term_.showCursor();
    } else {
        std::string name = buf_.filename().empty() ? "[no file]" : buf_.filename();
        std::string mod = buf_.dirty() ? "[+]" : "";
        size_t charCol = utf8::charCount(buf_.line(cy).substr(
            0, std::min(cx, buf_.line(cy).size())));
        std::string pos = "Ln " + std::to_string(cy + 1) + "/" +
                          std::to_string(n) + " Col " +
                          std::to_string(charCol + 1);
        // Right pill always shown (truncated only in absurd cases);
        // left pill dropped when nothing fits (1-cell gap kept).
        size_t maxRInner = cols > 4 ? static_cast<size_t>(cols - 4) : 0;
        std::string rShown = truncateCells(pos, maxRInner);
        int rW = static_cast<int>(visibleWidth(rShown));
        int Rp = rW + 4;
        std::string lShown;
        int lW = 0;
        int Lp = 0;
        if (cols - Rp - 5 >= 1) {
            lShown = truncateCells(name + mod,
                                   static_cast<size_t>(cols - Rp - 5));
            lW = static_cast<int>(visibleWidth(lShown));
            Lp = lW + 4;
        }
        if (framedBar) {
            if (Lp > 0) {
                pillEdge(rows - 2, 1, lW, true);
                term_.writeStr(cup(rows - 1, 1));
                term_.writeStr("\u2502 " + lShown + " \u2502");  // │ │
            }
            int rLeft = cols - Rp + 1;
            pillEdge(rows - 2, rLeft, rW, true);
            term_.writeStr(cup(rows - 1, rLeft));
            term_.writeStr("\u2502 " + rShown + " \u2502");  // │ │
            pillEdge(rows, rLeft, rW, false);
            if (Lp > 0) {
                pillEdge(rows, 1, lW, false);
                // Transient status centered in the gap (no border there).
                int gapL = Lp + 1, gapR = cols - Rp;
                int gap = gapR - gapL + 1;
                if (!status.empty() && gap > 0) {
                    std::string mid =
                        truncateCells(status, static_cast<size_t>(gap));
                    int midW = static_cast<int>(visibleWidth(mid));
                    term_.writeStr(cup(rows - 1, gapL + (gap - midW) / 2));
                    term_.writeStr(sgr::kDim);
                    term_.writeStr(mid);
                    term_.writeStr(sgr::kReset);
                }
            } else if (!status.empty()) {
                // No left pill: status fits the gap left of the right pill.
                int gapOnly = cols - Rp;
                if (gapOnly > 0) {
                    std::string mid = truncateCells(
                        status, static_cast<size_t>(gapOnly));
                    term_.writeStr(cup(rows - 1, 1));
                    term_.writeStr(sgr::kDim);
                    term_.writeStr(mid);
                    term_.writeStr(sgr::kReset);
                }
            }
        } else {
            // 3-row window: single plain row, no borders.
            term_.writeStr(cup(rows, 1));
            term_.writeStr("\x1b[2K");
            std::string left = Lp > 0 ? " " + lShown + " " : " ";
            std::string right = " " + rShown + " ";
            int fill = cols - static_cast<int>(visibleWidth(left)) -
                       static_cast<int>(visibleWidth(right));
            if (fill < 0) fill = 0;
            term_.writeStr(left);
            term_.writeStr(std::string(static_cast<size_t>(fill), ' '));
            term_.writeStr(right);
        }
    }

    if (!promptActive) {
        if (cursorPlaced) {
            term_.writeStr(cup((int)cursorScreenRow + 1, (int)cursorScreenCol));
            term_.showCursor();
        } else {
            term_.showCursor();
        }
    }
}
