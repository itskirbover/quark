#include "Menu.hpp"

#include "QuarkLogo.hpp"
#include "Terminal.hpp"
#include "Utf8.hpp"

namespace {

constexpr int kImageId = 31;

const char* kItems[] = {"Open file...", "New untitled file", "Quit"};
constexpr int kItemCount = 3;

std::string cup(int row1, int col1) {
    return "\x1b[" + std::to_string(row1) + ";" + std::to_string(col1) + "H";
}

// PNG dimensions from the IHDR chunk; falls back to the known logo size.
void logoSize(int& w, int& h) {
    w = 118;
    h = 118;
    const unsigned char* b = quark_logo::bytes;
    size_t n = quark_logo::size;
    if (n > 24 && b[12] == 'I' && b[13] == 'H' && b[14] == 'D' &&
        b[15] == 'R') {
        w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
        h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
            w = 118;
            h = 118;
        }
    }
}

}  // namespace

Menu::Menu(Terminal& term, InputReader& input, const KittySupport& supp)
    : term_(term), input_(input), supp_(supp) {}

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

    bool graphics = supp_.graphics && quark_logo::size > 0;
    int imgW = 0, imgH = 0;
    logoSize(imgW, imgH);

    // Logo cell footprint at NATURAL size: pixel dimensions mapped through
    // the measured cell size, so the image is never stretched or rescaled.
    // (Falls back to a 9x18 estimate when the terminal won't report cells.)
    int imgCols = (imgW + cellW_ - 1) / (cellW_ > 0 ? cellW_ : 9);
    int imgRows = (imgH + cellH_ - 1) / (cellH_ > 0 ? cellH_ : 18);
    if (imgCols < 1) imgCols = 1;
    if (imgRows < 1) imgRows = 1;

    int logoRows = graphics ? imgRows : 2;  // ASCII title takes 2 rows
    // Total block: logo + blank + items + blank + hint/status.
    int total = logoRows + 1 + kItemCount + 2;
    int top = (rows - total) / 2;
    if (top < 1) top = 1;

    // Placement for the end-of-frame display command (cursor-anchored).
    int imgRow = -1, imgCol = -1;
    if (graphics) {
        // Retransmitted every frame: the menu redraws only on keypress and
        // the per-frame full clear wipes placements, so stored images can't
        // be relied on across frames.
        term_.writeStr(kitty::transmitPng(quark_logo::bytes,
                                          quark_logo::size, kImageId));
        transmitted_ = true;
        imgCol = (cols - imgCols) / 2 + 1;
        if (imgCol < 1) imgCol = 1;
        imgRow = top + 1;
    } else {
        std::string title = "quark";
        int tcol = (cols - static_cast<int>(title.size())) / 2 + 1;
        if (tcol < 1) tcol = 1;
        term_.writeStr(cup(top + 1, tcol));
        term_.writeStr(sgr::kBold);
        term_.writeStr(title);
        term_.writeStr(sgr::kReset);
        std::string sub = "a markdown editor";
        int scol = (cols - static_cast<int>(sub.size())) / 2 + 1;
        if (scol < 1) scol = 1;
        term_.writeStr(cup(top + 2, scol));
        term_.writeStr(sgr::kDim);
        term_.writeStr(sub);
        term_.writeStr(sgr::kReset);
    }

    itemRows_.assign(kItemCount, -1);
    itemCols_.assign(kItemCount, 1);
    int firstItem = top + logoRows + 2;  // 1-based rows for cup()
    for (int i = 0; i < kItemCount; ++i) {
        int row = firstItem + i;
        itemRows_[i] = row;
        std::string label = std::string(i == selected_ ? "> " : "  ") + kItems[i];
        int col = (cols - static_cast<int>(label.size())) / 2 + 1;
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
    if (askingPath_) {
        std::string prompt = "Open: " + pathBuf_;
        int col = (cols - static_cast<int>(prompt.size()) - 1) / 2 + 1;
        if (col < 1) col = 1;
        term_.writeStr(cup(msgRow, col));
        term_.writeStr(prompt);
        term_.showCursor();
        term_.writeStr(cup(msgRow, col + static_cast<int>(prompt.size())));
    } else {
        std::string msg = status_.empty()
                              ? "Up/Down select - Enter confirm - o/n shortcuts - q quit"
                              : status_;
        size_t w = utf8::strWidth(msg);
        int col = (cols - static_cast<int>(w)) / 2 + 1;
        if (col < 1) col = 1;
        term_.writeStr(cup(msgRow, col));
        if (!status_.empty()) term_.writeStr(sgr::kBold);
        term_.writeStr(msg);
        term_.writeStr(sgr::kReset);
        // Display the logo last: it is cursor-anchored, so placing it after
        // all text guarantees nothing overwrites its cells afterwards.
        if (graphics && imgRow > 0) {
            term_.writeStr(cup(imgRow, imgCol));
            term_.writeStr(kitty::displayImage(kImageId, imgCols, imgRows));
        }
        term_.showCursor();
        // Park the cursor at the start of the selected label.
        term_.writeStr(cup(itemRows_[selected_], itemCols_[selected_]));
    }
}

MenuResult Menu::show() {
    MenuResult out;
    bool done = false;
    // Measure cells once (pre-alt-screen probes already ran; this is inside
    // the alt screen but the query/response is self-contained). On failure
    // the 9x18 member defaults stand.
    kitty::cellSizePx(cellW_, cellH_);
    while (!done) {
        draw();
        Key k = input_.readKey();
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
            continue;
        }
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
    // Never leave the transmitted image behind in the terminal.
    if (transmitted_) {
        term_.writeStr(kitty::deleteImage(kImageId));
        term_.writeStr("\x1b[H\x1b[2J");
        transmitted_ = false;
    }
    return out;
}
