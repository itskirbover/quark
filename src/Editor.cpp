#include "Editor.hpp"

#include <cstring>
#include <utility>
#include <vector>

#include "Menu.hpp"
#include "Utf8.hpp"

Editor::Editor() : renderer_(nullptr) {}

void Editor::startQuitPrompt() {
    promptActive_ = true;
    promptMode_ = PromptMode::Quit;
    std::string name =
        buf_.filename().empty() ? "untitled" : buf_.filename();
    promptText_ = buf_.dirty() ? "Save changes? (" + name + ") (y/n)"
                               : "Save file? (" + name + ") (y/n)";
}

void Editor::startSaveAsPrompt(bool thenQuit) {
    promptActive_ = true;
    promptMode_ = PromptMode::SaveAs;
    promptBuf_.clear();
    saveAsQuit_ = thenQuit;
    refreshSaveAsPrompt();
}

void Editor::refreshSaveAsPrompt() { promptText_ = "Save as: " + promptBuf_; }

int Editor::run(const std::string& path) {
    if (!path.empty()) buf_.open(path);

    if (!term_.enableRaw()) return 1;

    // Detect Kitty support BEFORE entering the alt screen so the probe
    // spaces don't pollute the UI. Screen will be cleared right after.
    kitty::detectSupport(supp_);
    kitty::detectGraphics(supp_);
    kitty::detectKeyboard(supp_);

    term_.enterAltScreen();
    term_.enableMouse();
    term_.enablePaste();
    if (supp_.keyboard) term_.enableKeyboard();

    // No file argument: main menu (Open / New / Quit).
    if (path.empty()) {
        Menu menu(term_, input_);
        MenuResult mr = menu.show();
        if (mr.action == MenuResult::Action::Quit) {
            term_.disableKeyboard();
            term_.disablePaste();
            term_.disableMouse();
            term_.exitAltScreen();
            term_.disableRaw();
            return 0;
        }
        if (mr.action == MenuResult::Action::Open) buf_.open(mr.path);
        // New: keep the fresh untitled buffer.
        cx_ = 0;
        cy_ = 0;
    }

    runEditorLoop();
    return loopExit_;
}

void Editor::runEditorLoop() {
    Renderer renderer(term_, buf_, supp_);
    renderer_ = &renderer;

    while (!shouldQuit_) {
        renderer_->draw(cx_, cy_, selecting_, ax_, ay_, status_,
                        promptActive_, promptText_);
        status_.clear();
        Key k = input_.readKey();
        if (promptActive_)
            handlePromptKey(k);
        else
            handleNormalKey(k);
    }

    term_.disableKeyboard();
    term_.disablePaste();
    term_.disableMouse();
    term_.exitAltScreen();
    term_.disableRaw();

    loopExit_ = 0;
    if (quitSave_) {
        if (!buf_.save()) {
            // Report failure outside alt screen.
            std::string msg = "quark: failed to save '" + buf_.filename() + "'\n";
            Terminal::writeRaw(msg);
            loopExit_ = 1;
        }
    }
}

void Editor::clampCursor() {
    size_t n = buf_.lineCount();
    if (n == 0) {
        cy_ = 0;
        cx_ = 0;
        return;
    }
    if (cy_ >= n) cy_ = n - 1;
    cx_ = buf_.clampToChar(cy_, cx_);
    if (selecting_) {
        if (ay_ >= n) ay_ = n - 1;
        ax_ = buf_.clampToChar(ay_, ax_);
    }
}

void Editor::applyExtend(bool extend) {
    if (extend) {
        if (!selecting_) {
            ax_ = cx_;
            ay_ = cy_;
            selecting_ = true;
        }
    } else {
        selecting_ = false;
    }
    breakTyping();
}

void Editor::moveWithExtend(bool extend, void (Editor::*move)()) {
    applyExtend(extend);
    (this->*move)();
}

bool Editor::hasSelection() const {
    return selecting_ && (ax_ != cx_ || ay_ != cy_);
}

void Editor::normSel(size_t& y0, size_t& x0, size_t& y1,
                     size_t& x1) const {
    y0 = ay_;
    x0 = buf_.clampToChar(ay_, ax_);
    y1 = cy_;
    x1 = buf_.clampToChar(cy_, cx_);
    if (y1 < y0 || (y1 == y0 && x1 < x0)) {
        std::swap(y0, y1);
        std::swap(x0, x1);
    }
}

void Editor::clearSelection() { selecting_ = false; }

bool Editor::eraseSelection() {
    if (!hasSelection()) return false;
    size_t y0, x0, y1, x1;
    normSel(y0, x0, y1, x1);
    buf_.eraseRange(y0, x0, y1, x1);
    cy_ = y0;
    cx_ = x0;
    selecting_ = false;
    clampCursor();
    return true;
}

void Editor::pushHistory(bool coalesceTyping) {
    if (coalesceTyping && typingActive_) return;
    HistState s{buf_.lines(), cx_, cy_, ax_, ay_, selecting_};
    undo_.push_back(std::move(s));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
    typingActive_ = coalesceTyping;
}

void Editor::breakTyping() { typingActive_ = false; }

void Editor::doUndo() {
    typingActive_ = false;
    if (undo_.empty()) {
        status_ = "Nothing to undo";
        return;
    }
    HistState cur{buf_.lines(), cx_, cy_, ax_, ay_, selecting_};
    redo_.push_back(std::move(cur));
    if (redo_.size() > 200) redo_.erase(redo_.begin());
    HistState s = std::move(undo_.back());
    undo_.pop_back();
    buf_.setLines(s.lines);
    cx_ = s.cx;
    cy_ = s.cy;
    ax_ = s.ax;
    ay_ = s.ay;
    selecting_ = s.selecting;
    clampCursor();
}

void Editor::doRedo() {
    typingActive_ = false;
    if (redo_.empty()) {
        status_ = "Nothing to redo";
        return;
    }
    HistState cur{buf_.lines(), cx_, cy_, ax_, ay_, selecting_};
    undo_.push_back(std::move(cur));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    HistState s = std::move(redo_.back());
    redo_.pop_back();
    buf_.setLines(s.lines);
    cx_ = s.cx;
    cy_ = s.cy;
    ax_ = s.ax;
    ay_ = s.ay;
    selecting_ = s.selecting;
    clampCursor();
}

void Editor::moveLeft() {
    if (cx_ > 0) {
        cx_ = utf8::prevCharStart(buf_.line(cy_), cx_);
    } else if (cy_ > 0) {
        --cy_;
        cx_ = buf_.lineEndX(cy_);
    }
}

void Editor::moveRight() {
    const std::string& line = buf_.line(cy_);
    if (cx_ < line.size()) {
        auto [cp, len] = utf8::decode(line, cx_);
        (void)cp;
        cx_ += (len == 0 ? 1 : len);
    } else if (cy_ + 1 < buf_.lineCount()) {
        ++cy_;
        cx_ = 0;
    }
}

void Editor::moveUp() {
    if (cy_ > 0) {
        // Preserve visual column approximately via character index.
        size_t charCol = utf8::charCount(buf_.line(cy_).substr(0, cx_));
        --cy_;
        cx_ = utf8::byteOffsetForChar(buf_.line(cy_), charCol);
        cx_ = buf_.clampToChar(cy_, cx_);
    }
}

void Editor::moveDown() {
    if (cy_ + 1 < buf_.lineCount()) {
        size_t charCol = utf8::charCount(buf_.line(cy_).substr(0, cx_));
        ++cy_;
        cx_ = utf8::byteOffsetForChar(buf_.line(cy_), charCol);
        cx_ = buf_.clampToChar(cy_, cx_);
    }
}

void Editor::pageUp() {
    int h = term_.rows() - 1;
    if (h < 1) h = 1;
    for (int i = 0; i < h; ++i) {
        if (cy_ == 0) break;
        moveUp();
    }
}

void Editor::pageDown() {
    int h = term_.rows() - 1;
    if (h < 1) h = 1;
    for (int i = 0; i < h; ++i) {
        if (cy_ + 1 >= buf_.lineCount()) break;
        moveDown();
    }
}

void Editor::handleNormalKey(const Key& k) {
    switch (k.type) {
        case Key::Type::CtrlC:
            // Required quit binding: always ask to save (y/n).
            breakTyping();
            clearSelection();
            startQuitPrompt();
            break;
        case Key::Type::CtrlS:
            breakTyping();
            if (buf_.filename().empty()) {
                clearSelection();
                startSaveAsPrompt(false);
            } else if (buf_.save()) {
                status_ = "Saved " + buf_.filename();
            } else {
                status_ = "Save failed: " + std::string(strerror(errno));
            }
            break;
        case Key::Type::CtrlZ:
            doUndo();
            break;
        case Key::Type::CtrlY:
            doRedo();
            break;
        case Key::Type::Copy: {
            if (!hasSelection()) {
                status_ = "Nothing to copy";
                break;
            }
            size_t y0, x0, y1, x1;
            normSel(y0, x0, y1, x1);
            std::string text = buf_.extractRange(y0, x0, y1, x1);
            if (text.size() > 1024 * 1024) {
                status_ = "Selection too large to copy";
                break;
            }
            Terminal::copyToClipboard(text);
            size_t chars = utf8::charCount(text);
            status_ = "Copied " + std::to_string(chars) +
                      (chars == 1 ? " char" : " chars");
            break;
        }
        case Key::Type::Paste: {
            if (k.text.empty()) break;
            pushHistory(false);
            eraseSelection();
            buf_.insertMultiline(cy_, cx_, k.text);
            size_t nl = 0;
            size_t lastLen = cx_;
            for (char c : k.text) {
                if (c == '\n') {
                    ++nl;
                    lastLen = 0;
                } else {
                    ++lastLen;
                }
            }
            cy_ += nl;
            cx_ = lastLen;
            size_t chars = utf8::charCount(k.text);
            status_ = "Pasted " + std::to_string(chars) +
                      (chars == 1 ? " char" : " chars");
            break;
        }
        case Key::Type::ArrowLeft:
            moveWithExtend(k.shift, &Editor::moveLeft);
            break;
        case Key::Type::ArrowRight:
            moveWithExtend(k.shift, &Editor::moveRight);
            break;
        case Key::Type::ArrowUp: moveWithExtend(k.shift, &Editor::moveUp); break;
        case Key::Type::ArrowDown:
            moveWithExtend(k.shift, &Editor::moveDown);
            break;
        case Key::Type::Home:
            applyExtend(k.shift);
            cx_ = 0;
            break;
        case Key::Type::End:
            applyExtend(k.shift);
            cx_ = buf_.lineEndX(cy_);
            break;
        case Key::Type::PageUp:
            applyExtend(k.shift);
            pageUp();
            break;
        case Key::Type::PageDown:
            applyExtend(k.shift);
            pageDown();
            break;
        case Key::Type::Enter:
            pushHistory(false);
            eraseSelection();
            buf_.insertNewline(cy_, cx_);
            ++cy_;
            cx_ = 0;
            break;
        case Key::Type::Backspace:
            if (hasSelection()) {
                pushHistory(false);
                eraseSelection();
            } else if (cx_ == 0 && cy_ == 0) {
                break;  // nothing to delete
            } else {
                pushHistory(false);
                size_t ny = cy_, nx = cx_;
                buf_.backspace(cy_, cx_, ny, nx);
                cy_ = ny;
                cx_ = nx;
            }
            break;
        case Key::Type::CtrlBackspace:
            if (hasSelection()) {
                pushHistory(false);
                eraseSelection();
            } else {
                size_t wy = cy_, wx = cx_;
                buf_.wordStartBackward(cy_, cx_, wy, wx);
                if (wy == cy_ && wx == cx_) break;  // nothing to delete
                pushHistory(false);
                buf_.eraseRange(wy, wx, cy_, cx_);
                cy_ = wy;
                cx_ = wx;
            }
            break;
        case Key::Type::Delete:
            if (hasSelection()) {
                pushHistory(false);
                eraseSelection();
            } else if (cx_ >= buf_.lineLen(cy_) &&
                       cy_ + 1 >= buf_.lineCount()) {
                break;  // nothing to delete
            } else {
                pushHistory(false);
                buf_.deleteForward(cy_, cx_);
            }
            break;
        case Key::Type::CtrlDelete:
            if (hasSelection()) {
                pushHistory(false);
                eraseSelection();
            } else {
                size_t wy = cy_, wx = cx_;
                buf_.wordEndForward(cy_, cx_, wy, wx);
                if (wy == cy_ && wx == cx_) break;  // nothing to delete
                pushHistory(false);
                buf_.eraseRange(cy_, cx_, wy, wx);
            }
            break;
        case Key::Type::MousePress: {
            breakTyping();
            size_t nx = cx_, ny = cy_;
            if (renderer_ && renderer_->screenToLogical(
                                 k.mouseCol, k.mouseRow, cx_, cy_,
                                 selecting_, ax_, ay_, nx, ny)) {
                cy_ = ny;
                cx_ = nx;
            }
            clearSelection();
            break;
        }
        case Key::Type::WheelUp:
            selecting_ = false;
            breakTyping();
            for (int i = 0; i < 3; ++i) moveUp();
            break;
        case Key::Type::WheelDown:
            selecting_ = false;
            breakTyping();
            for (int i = 0; i < 3; ++i) moveDown();
            break;
        case Key::Type::Char:
            pushHistory(true);
            eraseSelection();
            buf_.insertText(cy_, cx_, k.text);
            cx_ += k.text.size();
            break;
        case Key::Type::Esc:
            clearSelection();
            breakTyping();
            break;
        case Key::Type::None:
            break;
    }
    clampCursor();
}

void Editor::handlePromptKey(const Key& k) {
    if (promptMode_ == PromptMode::SaveAs) {
        if (k.type == Key::Type::Char && !k.text.empty()) {
            promptBuf_ += k.text;
            refreshSaveAsPrompt();
        } else if (k.type == Key::Type::Paste && !k.text.empty()) {
            for (char c : k.text) {
                if (c != '\n') promptBuf_ += c;
            }
            refreshSaveAsPrompt();
        } else if (k.type == Key::Type::Backspace) {
            if (!promptBuf_.empty()) {
                promptBuf_.erase(
                    utf8::prevCharStart(promptBuf_, promptBuf_.size()));
            }
            refreshSaveAsPrompt();
        } else if (k.type == Key::Type::Enter) {
            if (promptBuf_.empty()) {
                promptText_ = "Type a path (or Esc to cancel)";
            } else if (buf_.saveAs(promptBuf_)) {
                promptActive_ = false;
                if (saveAsQuit_) {
                    shouldQuit_ = true;
                    quitSave_ = false;  // already saved
                } else {
                    status_ = "Saved " + buf_.filename();
                }
            } else {
                promptText_ = "Save failed - Save as: " + promptBuf_;
                promptBuf_.clear();
            }
        } else if (k.type == Key::Type::Esc || k.type == Key::Type::CtrlC) {
            promptActive_ = false;
            status_ = saveAsQuit_ ? "Quit cancelled" : "Save cancelled";
        }
        return;
    }
    if (k.type == Key::Type::Char && !k.text.empty()) {
        char c = k.text[0];
        if (c == 'y' || c == 'Y') {
            if (buf_.filename().empty()) {
                // Untitled: need a path before quitting with save.
                startSaveAsPrompt(true);
            } else {
                promptActive_ = false;
                shouldQuit_ = true;
                quitSave_ = true;  // save after restoring the terminal
            }
        } else if (c == 'n' || c == 'N') {
            promptActive_ = false;
            shouldQuit_ = true;
            quitSave_ = false;
        }
        // Any other character: keep waiting for y/n.
    } else if (k.type == Key::Type::Esc || k.type == Key::Type::CtrlC) {
        // Cancel quit, back to editing (second Ctrl-C does NOT force-quit,
        // so the save question can't be skipped by accident).
        promptActive_ = false;
        status_ = "Quit cancelled";
    }
    // Enter/others ignored while prompting.
}
