#include "Renderer.hpp"

#include <algorithm>

#include "Buffer.hpp"
#include "Markdown.hpp"
#include "Terminal.hpp"
#include "Utf8.hpp"

namespace {

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

// Advance in terminal columns from absolute column baseCol over
// line[from,to) under a HeaderStyle. Mirrors emitLine: each grapheme takes
// s * cellWidth cells; tabs use absolute 8-stops.
size_t advanceCells(const std::string& line, size_t from, size_t to,
                    const HeaderStyle& st, bool scaled, size_t baseCol) {
    int s = scaled ? st.s : 1;
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
            int w = utf8::charWidth(cp);
            col += static_cast<size_t>(s * w);
        }
        i += len;
    }
    return col - baseCol;
}

// Tab-expanded copy of a line's content region (tabs -> spaces, 8-stops
// from baseCol), with contentStart/spans remapped. Needed because raw tabs
// inside Kitty sizing escapes don't behave like terminal tab stops.
struct ExpandedLine {
    std::string text;
    size_t contentStart = 0;
    std::vector<Span> spans;
    std::vector<std::pair<size_t, size_t>> skip;
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
    }
    out.skip = skip;
    for (auto& r : out.skip) {
        size_t ns = remap(r.first);
        size_t ne = remap(r.second);
        r.first = ns;
        r.second = (ne > ns) ? ne : ns;
    }
    return out;
}

// Combined style key for span lookup.
struct Style {
    bool bold = false, italic = false, code = false, strike = false, link = false;
    bool operator==(const Style& o) const {
        return bold == o.bold && italic == o.italic && code == o.code &&
               strike == o.strike && link == o.link;
    }
    bool operator!=(const Style& o) const { return !(*this == o); }
};

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
            int sc = 1;
            if (supp_.scale) sc = headerStyle(p).rows;
            used += sc;
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

void Renderer::emitLine(const std::string& line, size_t contentStart,
                        const std::string& baseSgr, const std::vector<Span>& spans,
                        const std::vector<std::pair<size_t, size_t>>& conceal,
                        const HeaderStyle& style, bool concealed, int cols) {
    (void)cols;
    bool sized = supp_.scale && style.s > 1;

    // Tabs inside sizing escapes don't act as tab stops: expand content tabs
    // to spaces first (cursor math uses the original line with tab stops from
    // the same base column, so columns stay consistent).
    ExpandedLine owned;
    const std::string* lineP = &line;
    const std::vector<Span>* spansP = &spans;
    const std::vector<std::pair<size_t, size_t>>* skipP = &conceal;
    size_t cs = contentStart;
    if (sized && line.find('\t', std::min(contentStart, line.size())) !=
                     std::string::npos) {
        // markerCells is unknown here; recompute cheaply (ASCII fast path).
        size_t markerCells = cellsBefore(line, 0, contentStart);
        owned = expandTabs(line, contentStart, spans, conceal, markerCells);
        lineP = &owned.text;
        spansP = &owned.spans;
        skipP = &owned.skip;
        cs = owned.contentStart;
    }
    const std::string& text = *lineP;
    const std::vector<Span>& sp = *spansP;

    // Merge conceal ranges (defensive; the parser emits non-overlapping ones).
    std::vector<std::pair<size_t, size_t>> skip;
    if (concealed && skipP && !skipP->empty()) {
        skip = *skipP;
        std::sort(skip.begin(), skip.end());
        std::vector<std::pair<size_t, size_t>> merged;
        for (const auto& r : skip) {
            if (!merged.empty() && r.first <= merged.back().second) {
                merged.back().second =
                    std::max(merged.back().second, r.second);
            } else {
                merged.push_back(r);
            }
        }
        skip.swap(merged);
    }
    // Offsets are visited in increasing order, so a cursor works.
    size_t skipIdx = 0;
    auto isSkipped = [&](size_t off) -> bool {
        while (skipIdx < skip.size() && off >= skip[skipIdx].second) ++skipIdx;
        return skipIdx < skip.size() && off >= skip[skipIdx].first;
    };

    std::string out;
    out.reserve(text.size() + 64);

    auto flushAsciiRun = [&](const std::string& run) {
        if (run.empty()) return;
        if (!sized) {
            out += run;
            return;
        }
        // Auto-split into s-by-s cells (split defensively under 4096B).
        size_t pos = 0;
        while (pos < run.size()) {
            size_t chunk = std::min<size_t>(3000, run.size() - pos);
            out += kitty::sized(run.substr(pos, chunk), style.s, 0,
                                style.n, style.d, style.v, style.h);
            pos += chunk;
        }
    };

    // Walk content codepoint by codepoint, grouping by style.
    size_t i = cs;
    std::string asciiRun;
    Style curStyle{false, false, false, false, false};
    bool haveCur = false;
    auto flushRun = [&]() {
        flushAsciiRun(asciiRun);
        asciiRun.clear();
    };

    // Base SGR for the whole content (header color etc.).
    out += baseSgr;
    while (i < text.size()) {
        auto [cp, len] = utf8::decode(text, i);
        if (len == 0) break;
        if (!skip.empty() && isSkipped(i)) {
            i += len;  // concealed delimiter: emit nothing, keep runs/styles
            continue;
        }
        Style st = styleAt(sp, i);
        if (!haveCur) {
            curStyle = st;
            haveCur = true;
            out += styleSgr(st);
        } else if (st != curStyle) {
            flushRun();
            out += sgr::kReset;
            out += baseSgr;
            out += styleSgr(st);
            curStyle = st;
        }
        std::string ch = text.substr(i, len);
        int w = utf8::charWidth(cp);
        if (cp < 0x80 && w == 1) {
            asciiRun += ch;
            if (asciiRun.size() >= 3000) flushRun();
        } else {
            flushRun();
            int cw = w <= 0 ? 1 : w;
            if (sized) {
                if (cw > 7) cw = 7;  // clamp; terminal does best-effort
                out += kitty::sized(ch, style.s, cw, style.n, style.d,
                                    style.v, style.h);
            } else if (supp_.width && cw > 1) {
                out += kitty::sized(ch, 1, cw);  // pin width, no scaling
            } else {
                out += ch;
            }
        }
        i += len;
    }
    flushRun();
    out += sgr::kReset;
    term_.writeStr(out);
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
        } else if (p.block == BlockType::CodeFence || p.block == BlockType::CodeBlock) {
            term_.writeStr(sgr::kDim);
            term_.writeStr(line);
            term_.writeStr(sgr::kReset);
        } else if (p.block == BlockType::HRule) {
            term_.writeStr(sgr::kDim);
            term_.writeStr(line);
            term_.writeStr(sgr::kReset);
        } else {
            // Marker dimmed at normal size, then styled content.
            size_t markerCells = 0;
            if (!p.marker.empty()) {
                term_.writeStr(sgr::kDim);
                term_.writeStr(p.marker);
                term_.writeStr(sgr::kReset);
                markerCells = cellsBefore(line, 0, p.marker.size());
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
            // Conceal inline markers (**, *, `, ~~, []()) on every line
            // except the one being edited, which shows raw source.
            bool concealedLine = (y != cy) || promptActive;
            emitLine(line, p.contentStart, base, p.spans, p.conceal, hs,
                     concealedLine, cols);

            if (y == cy && !promptActive) {
                size_t colCells;
                if (cx < p.contentStart) {
                    colCells = cellsBefore(line, 0, cx);
                } else {
                    // Scaled advance: each grapheme takes s * allocated
                    // cells (tabs use absolute 8-stops from markerCells).
                    colCells = markerCells + advanceCells(line, p.contentStart,
                                                          cx, hs, supp_.scale,
                                                          markerCells);
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
        screenRow += sc;
    }
    // Filler tilde rows.
    for (int r = screenRow; r < viewRows; ++r) {
        term_.writeStr(cup(r + 1, 1));
        term_.writeStr("\x1b[2K");
        term_.writeStr(sgr::kDim);
        term_.writeStr("~");
        term_.writeStr(sgr::kReset);
    }

    // Status bar.
    std::string bar;
    if (promptActive) {
        bar = promptText + " (y/n)";
    } else {
        std::string name = buf_.filename().empty() ? "[no file]" : buf_.filename();
        std::string mod = buf_.dirty() ? " [+]" : "";
        size_t charCol = utf8::charCount(buf_.line(cy).substr(
            0, std::min(cx, buf_.line(cy).size())));
        std::string kittyTag = !supp_.any() ? "plain"
                               : (supp_.scale ? "kitty-full" : "kitty-width");
        bar = " " + name + mod + " | Ln " + std::to_string(cy + 1) + "/" +
              std::to_string(n) + " Col " + std::to_string(charCol + 1) + " | " +
              kittyTag + (status.empty() ? "" : " | " + status) +
              " | Ctrl-C quit  Ctrl-S save";
    }
    // Truncate to cols (byte-safe-ish: ASCII status, filename may be UTF-8;
    // truncate by bytes but avoid splitting mid-codepoint).
    if (bar.size() > static_cast<size_t>(cols)) {
        size_t cut = cols;
        while (cut > 0 && utf8::isContinuation(static_cast<unsigned char>(bar[cut])))
            --cut;
        bar.erase(cut);
    }
    bar += std::string(cols > (int)bar.size() ? (cols - bar.size()) : 0, ' ');
    term_.writeStr(cup(rows, 1));
    term_.writeStr("\x1b[7m");
    term_.writeStr(bar);
    term_.writeStr(sgr::kReset);

    if (promptActive) {
        term_.writeStr(cup(rows, (int)promptText.size() + 7));
        term_.showCursor();
    } else if (cursorPlaced) {
        term_.writeStr(cup((int)cursorScreenRow + 1, (int)cursorScreenCol));
        term_.showCursor();
    } else {
        term_.showCursor();
    }
}
