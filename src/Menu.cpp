#include "Menu.hpp"

#include "Kitty.hpp"
#include "Terminal.hpp"
#include "Utf8.hpp"

namespace {

const char* kItems[] = {"Open file...", "New untitled file", "Quit"};
constexpr int kItemCount = 3;

std::string cup(int row1, int col1) {
    return "\x1b[" + std::to_string(row1) + ";" + std::to_string(col1) + "H";
}

// Transparent watermark band behind the title: faint "quark" words with
// single-space rhythm on both sides of the bright centered title. Words
// are stamped outward from the title with a 1-cell gap, so no fragments
// appear next to it; words may clip at the screen edges.
void drawTitleWithBand(Terminal& term, int titleRow, int cols) {
    if (titleRow < 1 || cols < 1) return;
    const std::string title = "quark";
    const int titleLen = static_cast<int>(utf8::strWidth(title));
    int tcol = (cols - titleLen) / 2 + 1;  // 1-based
    if (tcol < 1) tcol = 1;
    std::string row(static_cast<size_t>(cols), ' ');
    const std::string word = "quark";
    // Left side: right-aligned words ending before the 1-cell gap,
    // clipped at column 1.
    for (int e = tcol - 2; e >= 1; e -= 6) {
        for (int i = 0; i < 5; ++i) {
            int c = e - 4 + i;  // 1-based column
            if (c >= 1) row[static_cast<size_t>(c - 1)] = word[static_cast<size_t>(i)];
        }
    }
    // Right side: words starting after the 1-cell gap, clipped at cols.
    for (int s = tcol + titleLen + 1; s <= cols; s += 6) {
        for (int i = 0; i < 5; ++i) {
            int c = s + i;  // 1-based column
            if (c <= cols) row[static_cast<size_t>(c - 1)] = word[static_cast<size_t>(i)];
        }
    }
    term.writeStr(cup(titleRow, 1));
    term.writeStr(sgr::kDim);
    term.writeStr(row);
    term.writeStr(sgr::kReset);
    term.writeStr(cup(titleRow, tcol));
    term.writeStr(sgr::kBold);
    term.writeStr(title);
    term.writeStr(sgr::kReset);
}

}  // namespace

Menu::Menu(Terminal& term, InputReader& input) : term_(term), input_(input) {}

void Menu::activate(int idx, MenuResult& out, bool& done) {
    if (idx == 0) {
        askingPath_ = true;
        pathBuf_.clear();
        status_.clear();
    } else if (idx == 1) {
        out.action = MenuResult::Action::New;
        done = true;
    } else {
        out.action = MenuResult::Action::Quit;
        done = true;
    }
}

void Menu::draw() {
    term_.refreshSize();
    int rows = term_.rows();
    int cols = term_.cols();
    term_.hideCursor();
    term_.writeStr("\x1b[H\x1b[2J");

    constexpr int kPadX = 4;  // roomy side padding inside the border
    constexpr int kPadY = 1;  // roomy top/bottom padding inside the border

    // Bottom-line content (determines box width).
    std::string bottomMsg;
    bool isPrompt = askingPath_;
    if (isPrompt) {
        bottomMsg = "Open: " + pathBuf_;
    } else {
        bottomMsg = status_.empty()
                        ? "Up/Down select - Enter confirm - o/n shortcuts - q quit"
                        : status_;
    }
    size_t msgW = utf8::strWidth(bottomMsg);
    if (isPrompt) msgW += 1;  // room for the typing cursor
    size_t itemW = 0;
    for (int i = 0; i < kItemCount; ++i) {
        size_t w = utf8::strWidth(kItems[i]);
        if (w > itemW) itemW = w;
    }
    size_t contentW = itemW > msgW ? itemW : msgW;

    int interiorW = static_cast<int>(contentW) + kPadX * 2;
    int boxW = interiorW + 2;
    int boxH = kItemCount + 1 + 1 + kPadY * 2 + 2;  // items + blank + msg + pads + borders
    constexpr int kTitleRows = 2;                    // "quark" + subtitle, outside the box
    int totalH = kTitleRows + 1 + boxH;              // title + gap + box
    int top = (rows - totalH) / 2;
    if (top < 0) top = 0;

    // Fallback: window too small for the framed layout -> legacy borderless.
    if (boxW > cols || totalH > rows) {
        int legacyTotal = kTitleRows + 1 + kItemCount + 2;
        int legacyTop = (rows - legacyTotal) / 2;
        if (legacyTop < 1) legacyTop = 1;
        drawTitleWithBand(term_, legacyTop + 1, cols);
        std::string sub = "a markdown editor";
        int scol = (cols - static_cast<int>(sub.size())) / 2 + 1;
        if (scol < 1) scol = 1;
        term_.writeStr(cup(legacyTop + 2, scol));
        term_.writeStr(sgr::kDim);
        term_.writeStr(sub);
        term_.writeStr(sgr::kReset);
        itemRows_.assign(kItemCount, -1);
        itemCols_.assign(kItemCount, 1);
        int firstItem = legacyTop + kTitleRows + 2;
        for (int i = 0; i < kItemCount; ++i) {
            int row = firstItem + i;
            itemRows_[i] = row;
            std::string label = kItems[i];
            int col = (cols - static_cast<int>(utf8::strWidth(label))) / 2 + 1;
            if (col < 1) col = 1;
            itemCols_[i] = col;
            term_.writeStr(cup(row, col));
            if (i == selected_) term_.writeStr(sgr::kReverse);
            term_.writeStr(label);
            term_.writeStr(sgr::kReset);
        }
        int msgRow = firstItem + kItemCount + 1;
        term_.writeStr(cup(msgRow, 1));
        term_.writeStr("\x1b[2K");
        if (isPrompt) {
            int col = (cols - static_cast<int>(utf8::strWidth(bottomMsg)) - 1) / 2 + 1;
            if (col < 1) col = 1;
            term_.writeStr(cup(msgRow, col));
            term_.writeStr(bottomMsg);
            term_.showCursor();
            term_.writeStr(cup(msgRow, col + static_cast<int>(bottomMsg.size())));
        } else {
            int col = (cols - static_cast<int>(msgW)) / 2 + 1;
            if (col < 1) col = 1;
            term_.writeStr(cup(msgRow, col));
            if (!status_.empty()) term_.writeStr(sgr::kBold);
            term_.writeStr(bottomMsg);
            term_.writeStr(sgr::kReset);
            term_.showCursor();
            term_.writeStr(cup(itemRows_[selected_], itemCols_[selected_]));
        }
        return;
    }

    // Title block stays outside (above) the border.
    int titleRow = top + 1;
    int subRow = top + 2;
    int boxTop = top + 4;  // 1-based; top+3 is the gap row
    int boxLeft = (cols - boxW) / 2 + 1;
    if (boxLeft < 1) boxLeft = 1;

    drawTitleWithBand(term_, titleRow, cols);
    std::string sub = "a markdown editor";
    int scol = (cols - static_cast<int>(utf8::strWidth(sub))) / 2 + 1;
    if (scol < 1) scol = 1;
    term_.writeStr(cup(subRow, scol));
    term_.writeStr(sgr::kDim);
    term_.writeStr(sub);
    term_.writeStr(sgr::kReset);

    // Rounded border, monochrome (no color / gradients).
    std::string horiz;
    for (int i = 0; i < boxW - 2; ++i) horiz += "\u2500";  // ─
    term_.writeStr(cup(boxTop, boxLeft));
    term_.writeStr("\u256d" + horiz + "\u256e");  // ╭ ╮
    for (int r = 1; r < boxH - 1; ++r) {
        term_.writeStr(cup(boxTop + r, boxLeft));
        term_.writeStr("\u2502");  // │
        term_.writeStr(cup(boxTop + r, boxLeft + boxW - 1));
        term_.writeStr("\u2502");  // │
    }
    term_.writeStr(cup(boxTop + boxH - 1, boxLeft));
    term_.writeStr("\u2570" + horiz + "\u256f");  // ╰ ╯

    itemRows_.assign(kItemCount, -1);
    itemCols_.assign(kItemCount, 1);
    int firstItem = boxTop + 1 + kPadY;
    for (int i = 0; i < kItemCount; ++i) {
        int row = firstItem + i;
        itemRows_[i] = row;
        std::string label = kItems[i];
        int lw = static_cast<int>(utf8::strWidth(label));
        int col = boxLeft + 1 + (interiorW - lw) / 2;
        itemCols_[i] = col;
        term_.writeStr(cup(row, col));
        if (i == selected_) term_.writeStr(sgr::kReverse);
        term_.writeStr(label);
        term_.writeStr(sgr::kReset);
    }

    int msgRow = firstItem + kItemCount + 1;
    int msgCol = boxLeft + 1 + (interiorW - static_cast<int>(msgW)) / 2;
    if (msgCol < boxLeft + 1) msgCol = boxLeft + 1;
    if (isPrompt) {
        term_.writeStr(cup(msgRow, msgCol));
        term_.writeStr(bottomMsg);
        term_.showCursor();
        term_.writeStr(cup(msgRow, msgCol + static_cast<int>(bottomMsg.size())));
    } else {
        term_.writeStr(cup(msgRow, msgCol));
        if (!status_.empty()) term_.writeStr(sgr::kBold);
        term_.writeStr(bottomMsg);
        term_.writeStr(sgr::kReset);
        term_.showCursor();
        // Park the cursor at the start of the selected label.
        term_.writeStr(cup(itemRows_[selected_], itemCols_[selected_]));
    }
}

MenuResult Menu::show() {
    MenuResult out;
    bool done = false;
    // Poll interval for window resizes while waiting for a key: the read
    // below times out (no SIGWINCH handler needed) and we redraw only
    // when the size actually changed, so there is no idle flicker.
    constexpr int kResizePollMs = 200;
    draw();
    while (!done) {
        Key k = input_.readKeyTimeout(kResizePollMs);
        if (k.type == Key::Type::None) {
            int r = term_.rows(), c = term_.cols();
            if (term_.refreshSize() && (term_.rows() != r || term_.cols() != c)) draw();
            continue;
        }
        if (askingPath_) {
            switch (k.type) {
                case Key::Type::Char:
                    pathBuf_ += k.text;
                    break;
                case Key::Type::Backspace:
                    if (!pathBuf_.empty()) {
                        pathBuf_.erase(
                            utf8::prevCharStart(pathBuf_, pathBuf_.size()));
                    }
                    break;
                case Key::Type::Enter:
                    if (pathBuf_.empty()) {
                        status_ = "Type a path (or Esc to cancel)";
                        askingPath_ = false;
                    } else {
                        out.action = MenuResult::Action::Open;
                        out.path = pathBuf_;
                        done = true;
                    }
                    break;
                case Key::Type::Esc:
                case Key::Type::CtrlC:
                    askingPath_ = false;
                    status_.clear();
                    break;
                default:
                    break;
            }
        } else {
            switch (k.type) {
                case Key::Type::ArrowUp:
                    selected_ = (selected_ + kItemCount - 1) % kItemCount;
                    break;
                case Key::Type::ArrowDown:
                    selected_ = (selected_ + 1) % kItemCount;
                    break;
                case Key::Type::Enter:
                    activate(selected_, out, done);
                    break;
                case Key::Type::Esc:
                case Key::Type::CtrlC:
                    out.action = MenuResult::Action::Quit;
                    done = true;
                    break;
                case Key::Type::MousePress:
                    for (int i = 0; i < kItemCount; ++i) {
                        if (k.mouseRow == itemRows_[i]) {
                            selected_ = i;
                            activate(i, out, done);
                            break;
                        }
                    }
                    break;
                case Key::Type::Char:
                    if (!k.text.empty()) {
                        char c = k.text[0];
                        if (c == 'q' || c == 'Q') {
                            out.action = MenuResult::Action::Quit;
                            done = true;
                        } else if (c == 'o' || c == 'O') {
                            activate(0, out, done);
                        } else if (c == 'n' || c == 'N') {
                            activate(1, out, done);
                        } else if (c == 'k' || c == 'K') {
                            selected_ = (selected_ + kItemCount - 1) % kItemCount;
                        } else if (c == 'j' || c == 'J') {
                            selected_ = (selected_ + 1) % kItemCount;
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        if (!done) draw();
    }
    term_.writeStr("\x1b[H\x1b[2J");
    return out;
}
