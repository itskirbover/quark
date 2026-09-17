#pragma once
// Full-screen renderer: Markdown styling + Kitty text sizing + status bar.
// Logical lines map to HeaderStyle::rows screen rows (H1-H3 are taller).

#include <cstddef>
#include <string>
#include <vector>

#include "Kitty.hpp"
#include "Markdown.hpp"

class Terminal;
class Buffer;

// Combined inline style key for span lookup.
struct Style {
    bool bold = false, italic = false, code = false, strike = false,
         link = false;
    bool operator==(const Style& o) const {
        return bold == o.bold && italic == o.italic && code == o.code &&
               strike == o.strike && link == o.link;
    }
    bool operator!=(const Style& o) const { return !(*this == o); }
};

// One emitted run: byte range, combined style, OSC width (0 = auto-split).
// For packed fractional groups, chars = ASCII char count (advance per char
// is s*w/chars); solo graphemes have chars = 1.
struct Segment {
    size_t start = 0, end = 0;
    Style style;
    int w = 0;
    int chars = 0;
};

// Everything draw/click-mapping needs for one logical line: tab-expanded
// text, style/skip data remapped into it, and emitted segments shared by
// emission, cursor advance, and click mapping.
struct LineLayout {
    std::string text;
    size_t contentStart = 0;
    std::vector<Span> spans;
    std::vector<std::pair<size_t, size_t>> skip;
    std::vector<Segment> segs;
    std::vector<size_t> byteMap;  // original -> text offsets; empty = identity
    bool sized = false;
    size_t selStart = 0, selEnd = 0;
};

class Renderer {
public:
    Renderer(Terminal& term, Buffer& buf, const KittySupport& supp);

    // Redraw everything. cx/cy = editing cursor (cx = byte offset).
    // Selection is anchor (selAx/selAy) + cursor head; highlighted when
    // selActive and non-empty. If promptActive, the status row shows
    // promptText instead and the editing cursor is parked there.
    void draw(size_t cx, size_t cy, bool selActive, size_t selAx,
              size_t selAy, const std::string& status, bool promptActive,
              const std::string& promptText);

    size_t topLine() const { return topLine_; }

    // Map 1-based screen coordinates to a buffer cursor (false = ignore:
    // status bar, out of range). curCx/curCy drive span reveal state;
    // the selection reveals spans the same way.
    bool screenToLogical(int sx, int sy, size_t curCx, size_t curCy,
                         bool selActive, size_t selAx, size_t selAy,
                         size_t& outCx, size_t& outCy);

private:
    Terminal& term_;
    Buffer& buf_;
    const KittySupport& supp_;
    size_t topLine_ = 0;

    void ensureVisible(size_t cy, int viewRows);
    LineLayout layoutLine(const std::string& line, size_t contentStart,
                          const std::vector<Span>& spans,
                          const std::vector<std::pair<size_t, size_t>>& skip,
                          const HeaderStyle& style, size_t markerCells,
                          size_t selStart, size_t selEnd);
    void emitLayout(const LineLayout& lay, const std::string& baseSgr,
                    const HeaderStyle& style);
};
