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

class Renderer {
public:
    Renderer(Terminal& term, Buffer& buf, const KittySupport& supp);

    // Redraw everything. cx/cy = editing cursor (cx = byte offset).
    // If promptActive, the status row shows promptText instead and the
    // editing cursor is parked there.
    void draw(size_t cx, size_t cy, const std::string& status, bool promptActive,
              const std::string& promptText);

    size_t topLine() const { return topLine_; }

private:
    Terminal& term_;
    Buffer& buf_;
    const KittySupport& supp_;
    size_t topLine_ = 0;

    void ensureVisible(size_t cy, int viewRows);
    void emitLine(const std::string& line, size_t contentStart,
                  const std::string& baseSgr, const std::vector<Span>& spans,
                  const std::vector<std::pair<size_t, size_t>>& conceal,
                  const HeaderStyle& style, bool concealed, int cols);
};
