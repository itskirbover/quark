#pragma once
// In-memory text buffer: lines of UTF-8, dirty flag, file IO.
// Cursor coordinates are byte offsets (cx) + line index (cy).

#include <string>
#include <vector>

class Buffer {
public:
    Buffer();

    bool open(const std::string& path);  // missing file -> fresh buffer
    bool save();                         // saves to filename_
    bool saveAs(const std::string& path);
    const std::string& filename() const { return filename_; }

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    size_t lineCount() const { return lines_.size(); }
    const std::string& line(size_t y) const;
    size_t lineLen(size_t y) const;

    void insertText(size_t y, size_t x, const std::string& text);
    void insertNewline(size_t y, size_t x);  // split line
    void backspace(size_t y, size_t x, size_t& outY, size_t& outX);
    void deleteForward(size_t y, size_t x);

    // Range edit for selections / undo (multi-line, '\n'-joined text).
    // Endpoints are normalized and clamped to character boundaries.
    std::string extractRange(size_t y0, size_t x0, size_t y1,
                             size_t x1) const;
    void eraseRange(size_t y0, size_t x0, size_t y1, size_t x1);
    void insertMultiline(size_t y, size_t x, const std::string& text);

    // Word motion for Ctrl+Backspace (backward) / Ctrl+Delete (forward).
    // At a line edge the target crosses the newline (join behavior).
    void wordStartBackward(size_t y, size_t x, size_t& outY,
                           size_t& outX) const;
    void wordEndForward(size_t y, size_t x, size_t& outY,
                        size_t& outX) const;

    // Full-line access for undo snapshots.
    const std::vector<std::string>& lines() const { return lines_; }
    void setLines(const std::vector<std::string>& l);

    // Clamp byte offset to a character boundary inside the line.
    size_t clampToChar(size_t y, size_t x) const;
    size_t lineEndX(size_t y) const;

private:
    std::vector<std::string> lines_;
    std::string filename_;
    bool dirty_ = false;
    // Whether the file ends with a newline (preserved across load/save so
    // round-trips neither add phantom blank lines nor drop the final \n).
    bool trailingNewline_ = true;
};
