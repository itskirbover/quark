#include "Editor.hpp"

#include <cstring>

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

    term_.enterAltScreen();
    term_.enableMouse();

    // No file argument: main menu (Open / New / Quit).
    if (path.empty()) {
        Menu menu(term_, input_, supp_);
        MenuResult mr = menu.show();
        if (mr.action == MenuResult::Action::Quit) {
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
        renderer_->draw(cx_, cy_, status_, promptActive_, promptText_);
        status_.clear();
        Key k = input_.readKey();
        if (promptActive_)
            handlePromptKey(k);
        else
            handleNormalKey(k);
    }

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
            startQuitPrompt();
            break;
        case Key::Type::CtrlS:
            if (buf_.filename().empty()) {
                startSaveAsPrompt(false);
            } else if (buf_.save()) {
                status_ = "Saved " + buf_.filename();
            } else {
                status_ = "Save failed: " + std::string(strerror(errno));
            }
            break;
        case Key::Type::ArrowLeft: moveLeft(); break;
        case Key::Type::ArrowRight: moveRight(); break;
        case Key::Type::ArrowUp: moveUp(); break;
        case Key::Type::ArrowDown: moveDown(); break;
        case Key::Type::Home: cx_ = 0; break;
        case Key::Type::End: cx_ = buf_.lineEndX(cy_); break;
        case Key::Type::PageUp: pageUp(); break;
        case Key::Type::PageDown: pageDown(); break;
        case Key::Type::Enter:
            buf_.insertNewline(cy_, cx_);
            ++cy_;
            cx_ = 0;
            break;
        case Key::Type::Backspace: {
            size_t ny = cy_, nx = cx_;
            buf_.backspace(cy_, cx_, ny, nx);
            cy_ = ny;
            cx_ = nx;
            break;
        }
        case Key::Type::Delete: buf_.deleteForward(cy_, cx_); break;
        case Key::Type::MousePress: {
            size_t nx = cx_, ny = cy_;
            if (renderer_ &&
                renderer_->screenToLogical(k.mouseCol, k.mouseRow, cx_, cy_,
                                           nx, ny)) {
                cy_ = ny;
                cx_ = nx;
            }
            break;
        }
        case Key::Type::WheelUp:
            for (int i = 0; i < 3; ++i) moveUp();
            break;
        case Key::Type::WheelDown:
            for (int i = 0; i < 3; ++i) moveDown();
            break;
        case Key::Type::Char:
            buf_.insertText(cy_, cx_, k.text);
            cx_ += k.text.size();
            break;
        case Key::Type::Esc:
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
