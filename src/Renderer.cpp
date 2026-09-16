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

// Display cells of line[from,to) with tab stops every 8.
size_t cellsBefore(const std::string& line, size_t from, size_t to) {
    size_t col = 0;
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

// Conceal ranges minus spans revealed by the cursor: a span reveals (shows
// raw source) when the cursor lies in its full source extent. Without a
// cursor on the line, everything concealed stays hidden.
std::vector<std::pair<size_t, size_t>> skipRangesFor(
    const std::vector<Span>& spans,
    const std::vector<std::pair<size_t, size_t>>& conceal, bool hasCursor,
    size_t cx) {
    if (!hasCursor) return mergedRanges(conceal);
    std::vector<std::pair<size_t, size_t>> keep;
    for (const auto& r : conceal) {
        bool revealed = false;
        for (const auto& s : spans) {
            if (cx >= s.srcStart && cx <= s.srcEnd && r.first >= s.srcStart &&
                r.second <= s.srcEnd) {
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
    int packD) {
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
                                const HeaderStyle& style, size_t markerCells) {
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
    lay.segs = layoutContent(lay.text, lay.contentStart, lay.spans, lay.skip,
                             (lay.sized && style.n > 0 && style.d > style.n)
                                 ? style.n
                                 : 0,
                             (lay.sized && style.n > 0 && style.d > style.n)
                                 ? style.d
                                 : 0);
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
    for (const auto& g : lay.segs) {
        if (!haveCur || g.style != curStyle) {
            if (haveCur) {
                out += sgr::kReset;
                out += baseSgr;
            }
            out += styleSgr(g.style);
            curStyle = g.style;
            haveCur = true;
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
    out += sgr::kReset;
    term_.writeStr(out);
}

bool Renderer::screenToLogical(int sx, int sy, size_t curCx, size_t curCy,
                               size_t& outCx, size_t& outCy) {
    int rows = term_.rows();
    int cols = term_.cols();
    if (sx < 1 || sx > cols || sy < 1 || sy > rows - 1) return false;
    size_t n = buf_.lineCount();
    if (n == 0 || topLine_ >= n) return false;
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
        int packN = 0, packD = 0;
        bool sized = supp_.scale && hs.s > 1;
        if (sized && hs.n > 0 && hs.d > hs.n) {
            packN = hs.n;
            packD = hs.d;
        }
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
                chashSegs =
                    layoutContent(chash, 0, {}, {}, packN, packD);
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
        auto skip0 = skipRangesFor(p.spans, p.conceal, hasCursor,
                                   hasCursor ? curCx : 0);
        std::vector<Span> useSpans = plainKind ? std::vector<Span>{} : p.spans;
        auto segs = layoutContent(line, cs, useSpans, skip0, packN, packD);
        int s = sized ? hs.s : 1;
        long rel = static_cast<long>(sx - 1) - static_cast<long>(markerCells);
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
        // Inverse segment walk from the content origin.
        size_t abscol = markerCells;
        size_t acc = 0;
        size_t targetCol = static_cast<size_t>(rel);
        for (const auto& g : segs) {
            size_t segAdv;
            if (sized && g.w > 0) {
                segAdv = static_cast<size_t>(s * g.w);
            } else {
                segAdv = 0;
                size_t k = g.start;
                while (k < g.end) {
                    auto [cp, len] = utf8::decode(line, k);
                    if (len == 0) break;
                    size_t cur = abscol + segAdv;
                    size_t gad;
                    if (cp == '\t') {
                        gad = (cur / 8 + 1) * 8 - cur;
                    } else {
                        gad = static_cast<size_t>(s * utf8::charWidth(cp));
                    }
                    if (targetCol < acc + segAdv + gad) {
                        outCx = k;  // click inside glyph -> its start
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
                outCx = off;
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

void Renderer::draw(size_t cx, size_t cy, const std::string& status, bool promptActive,
                    const std::string& promptText) {
    term_.refreshSize();
    int rows = term_.rows();
    int cols = term_.cols();
    size_t n = buf_.lineCount();
    if (cy >= n) cy = n == 0 ? 0 : n - 1;

    if (rows < 3 || cols < 20) {
        term_.writeStr("\x1b[H\x1b[2J");
        term_.writeStr("Window too small for quark (need 20x3).");
        term_.hideCursor();
        return;
    }
    int viewRows = rows - 1;  // last row = status bar
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
            // nothing
        } else if (p.block == BlockType::CodeFence) {
            term_.writeStr(sgr::kDim);
            term_.writeStr(line);
            term_.writeStr(sgr::kReset);
        } else if (p.block == BlockType::CodeBlock) {
            term_.writeStr(line);  // normal body text, no dimming
        } else if (p.block == BlockType::HRule) {
            term_.writeStr(sgr::kDim);
            term_.writeStr(line);
            term_.writeStr(sgr::kReset);
        } else {
            // Header `#` markers are concealed unless the cursor is on the
            // line (like inline `**`/`*`/backtick markers). All other block
            // markers (`>`, `-`, `1.`) stay dimmed inline. A revealed header
            // marker renders inline at the same Kitty scale as its text.
            bool hasCursor = (y == cy) && !promptActive;
            bool isHeader = p.headerLevel >= 1;
            bool showMarker =
                !p.marker.empty() && (!isHeader || hasCursor);
            bool sized = supp_.scale && hs.s > 1;
            int packN = (sized && hs.n > 0 && hs.d > hs.n) ? hs.n : 0;
            int packD = (sized && hs.n > 0 && hs.d > hs.n) ? hs.d : 0;
            // Sized-header marker pieces: only the '#' run is scaled; the
            // separator space stays 1 cell so `## Text` keeps a normal gap.
            // (Kept in scope for the cursor-advance math below.)
            MarkerParts mp;
            std::string hashText;
            std::vector<Segment> hashSegs;
            size_t indentCells = 0, hashAdv = 0, sepCells = 0;
            size_t markerCells = 0;
            bool sizedHashes = showMarker && isHeader && sized;
            if (sizedHashes) {
                mp = splitHeaderMarker(line, p.marker.size());
                hashText = line.substr(mp.hstart, mp.hend - mp.hstart);
                hashSegs =
                    layoutContent(hashText, 0, {}, {}, packN, packD);
                indentCells = cellsBefore(line, 0, mp.hstart);
                for (const auto& g : hashSegs) {
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
                std::string mout;
                mout += sgr::kDim;
                mout += line.substr(0, mp.hstart);  // indent, unsized
                for (const auto& g : hashSegs) {
                    std::string chunk =
                        hashText.substr(g.start, g.end - g.start);
                    if (g.w == 0) {
                        size_t pos = 0;
                        while (pos < chunk.size()) {
                            size_t nn = std::min<size_t>(
                                3000, chunk.size() - pos);
                            mout += kitty::sized(chunk.substr(pos, nn),
                                                 hs.s, 0, hs.n, hs.d,
                                                 hs.v, hs.h);
                            pos += nn;
                        }
                    } else {
                        int w = g.w > 7 ? 7 : g.w;
                        mout += kitty::sized(chunk, hs.s, w, hs.n, hs.d,
                                             hs.v, hs.h);
                    }
                }
                mout += line.substr(mp.hend, mp.msize - mp.hend);  // separator
                mout += sgr::kReset;
                term_.writeStr(mout);
            } else if (showMarker) {
                term_.writeStr(sgr::kDim);
                term_.writeStr(p.marker);
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
            // Span-level reveal: hidden formatting shows raw source only
            // when the cursor is inside the formatted span's extent.
            // (hasCursor already computed above for marker concealment.)
            auto skip0 = skipRangesFor(p.spans, p.conceal, hasCursor, cx);
            LineLayout lay = layoutLine(line, p.contentStart, p.spans, skip0,
                                        hs, markerCells);
            emitLayout(lay, base, hs);

            if (hasCursor) {
                size_t colCells;
                if (cx < p.contentStart && sizedHashes) {
                    if (cx <= mp.hstart) {
                        colCells = cellsBefore(line, 0, cx);
                    } else if (cx < mp.hend) {
                        colCells = indentCells +
                                   advanceUpTo(hashText, hashSegs, hs.s,
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
            size_t c = 1 + cellsBefore(line, 0, cx);
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

    // Status bar: LazyVim-like blocks (left = filename, right = context).
    term_.writeStr(cup(rows, 1));
    term_.writeStr("\x1b[2K");
    if (promptActive) {
        // promptText is fully formed by the caller (quit confirm includes
        // "(y/n)"; save-as shows the path being typed).
        std::string bar = promptText;
        if (bar.size() > static_cast<size_t>(cols)) {
            size_t cut = cols;
            while (cut > 0 &&
                   utf8::isContinuation(static_cast<unsigned char>(bar[cut])))
                --cut;
            bar.erase(cut);
        }
        bar += std::string(cols > (int)bar.size() ? (cols - bar.size()) : 0,
                           ' ');
        term_.writeStr("\x1b[7m");
        term_.writeStr(bar);
        term_.writeStr(sgr::kReset);
    } else {
        std::string name = buf_.filename().empty() ? "[no file]" : buf_.filename();
        std::string mod = buf_.dirty() ? "[+]" : "";
        size_t charCol = utf8::charCount(buf_.line(cy).substr(
            0, std::min(cx, buf_.line(cy).size())));
        std::string kittyTag = !supp_.any() ? "plain"
                               : (supp_.scale ? "kitty-full" : "kitty-width");
        std::string pos = "Ln " + std::to_string(cy + 1) + "/" +
                          std::to_string(n) + " Col " +
                          std::to_string(charCol + 1);
        std::string scroll = (cy == 0) ? "Top"
                             : (cy + 1 >= n)
                                   ? "Bot"
                                   : std::to_string(cy * 100 /
                                                    std::max<size_t>(n - 1, 1)) +
                                         "%";
        char tbuf[6] = "";
        {
            time_t t = time(nullptr);
            strftime(tbuf, sizeof(tbuf), "%H:%M", localtime(&t));
        }
        // Tiered right side for narrow windows: clock, then scroll %.
        // Position and kitty tag always stay; the filename truncates last.
        // Tiers drop until the middle (status/hints) fits, if possible.
        std::string midWant = status.empty()
                                  ? "Ctrl-C quit  Ctrl-S save  click to move"
                                  : " " + status;
        size_t needMid = visibleWidth(midWant);
        bool showClock = true, showScroll = true;
        std::string right;
        std::string leftText = " " + name + mod + " ";
        size_t rightW = 0, maxLeft = 0, midAvail = 0;
        for (;;) {
            right = std::string(sgr::kDim) + " " + kittyTag + " " +
                    sgr::kReset;
            std::string posSeg = pos;
            if (showScroll) posSeg = scroll + " " + posSeg;
            right += std::string("\x1b[97;44m ") + posSeg + " " + sgr::kReset;
            if (showClock)
                right +=
                    std::string("\x1b[30;104m ") + tbuf + " " + sgr::kReset;
            rightW = visibleWidth(right);
            maxLeft = (static_cast<size_t>(cols) > rightW + 1)
                          ? static_cast<size_t>(cols) - rightW - 1
                          : 0;
            size_t leftW =
                std::min(visibleWidth(leftText), maxLeft);
            midAvail = (static_cast<size_t>(cols) > leftW + rightW)
                           ? static_cast<size_t>(cols) - leftW - rightW
                           : 0;
            if (midAvail >= needMid || (!showClock && !showScroll)) break;
            if (showClock)
                showClock = false;
            else
                showScroll = false;
        }
        std::string leftShown = truncateCells(leftText, maxLeft);
        std::string mid = truncateCells(midWant, midAvail);
        size_t pad =
            (midAvail > visibleWidth(mid)) ? midAvail - visibleWidth(mid) : 0;
        term_.writeStr("\x1b[30;104m");
        term_.writeStr(leftShown);
        term_.writeStr(sgr::kReset);
        if (!mid.empty()) {
            term_.writeStr(sgr::kDim);
            term_.writeStr(mid);
            term_.writeStr(sgr::kReset);
        }
        term_.writeStr(std::string(pad, ' '));
        term_.writeStr(right);
    }

    if (promptActive) {
        term_.writeStr(cup(rows, (int)promptText.size() + 1));
        term_.showCursor();
    } else if (cursorPlaced) {
        term_.writeStr(cup((int)cursorScreenRow + 1, (int)cursorScreenCol));
        term_.showCursor();
    } else {
        term_.showCursor();
    }
}
