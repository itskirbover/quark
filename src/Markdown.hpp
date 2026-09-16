#pragma once
// Minimal Markdown structure: block classification + inline spans.
// Byte offsets (not code points) so the renderer can slice the source line.

#include <string>
#include <vector>

enum class BlockType {
    H1,
    H2,
    H3,
    H4,
    H5,
    H6,
    Quote,
    UList,
    OList,
    CodeFence,  // the ``` delimiter line itself
    CodeBlock,  // inside a fence
    HRule,
    Empty,
    Paragraph,
};

struct Span {
    size_t start = 0;  // byte offset of styled content in line
    size_t len = 0;    // byte length of styled content
    // Full source extent including delimiters ([..](..), **, *, `, ~~).
    // A span reveals (shows raw source) when the cursor is inside it.
    size_t srcStart = 0;
    size_t srcEnd = 0;
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool strike = false;
    bool linkText = false;
};

struct ParsedLine {
    BlockType block = BlockType::Paragraph;
    int headerLevel = 0;        // 1-6 for headers
    size_t contentStart = 0;    // byte offset where styled content begins
    std::string marker;         // e.g. "# ", "> ", "- " (rendered dimmed)
    std::vector<Span> spans;    // inline styles within content
    // Delimiter byte ranges hidden unless the line is being edited:
    // **, *, _, ~~, `, [, ](url). Block markers (#, >, -, 1.) stay visible.
    std::vector<std::pair<size_t, size_t>> conceal;
};

// Parse one source line. inFence tracks whether we are inside a ``` block;
// parseLine updates it when it sees a fence delimiter.
ParsedLine parseLine(const std::string& line, bool& inFence);

// Kitty text-sizing treatment for a parsed line.
// H1 takes 2 rows at s=2; H2/H3 take 2 rows at s=2 with fractional shrink
// (distinct effective sizes); H4-H6 and everything else are single-row s=1
// + SGR.
struct HeaderStyle {
    int rows = 1;      // terminal rows this logical line occupies (== s when scaled)
    int s = 1;         // overall scale
    int n = 0, d = 0;  // fractional scale, applied on top of s
    int v = 0, h = 0;  // alignment for fractional scaling
};

// Sizing treatment for a parsed line (rows == 1, s == 1 for non-headers).
HeaderStyle headerStyle(const ParsedLine& p);

// SGR open sequence for a span ("" if unstyled).
std::string spanSgr(const Span& s);
// SGR open sequence for a header level (includes bold).
const char* headerSgr(int level);
