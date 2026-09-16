#pragma once
// Editor: main loop, cursor, quit-with-save-prompt (Ctrl-C -> y/n).

#include <string>

#include "Buffer.hpp"
#include "Input.hpp"
#include "Kitty.hpp"
#include "Renderer.hpp"
#include "Terminal.hpp"

class Editor {
public:
    Editor();
    int run(const std::string& path);

private:
    Terminal term_;
    InputReader input_;
    Buffer buf_;
    KittySupport supp_{};
    Renderer* renderer_ = nullptr;

    size_t cx_ = 0;  // byte offset in current line
    size_t cy_ = 0;  // line index
    std::string status_;
    bool promptActive_ = false;
    std::string promptText_;
    // Prompt doubles as quit-confirm (y/n) and save-as path entry.
    enum class PromptMode { Quit, SaveAs };
    PromptMode promptMode_ = PromptMode::Quit;
    std::string promptBuf_;  // save-as path being typed
    bool saveAsQuit_ = false;  // save-as Enter quits (came from quit flow)
    bool shouldQuit_ = false;
    bool quitSave_ = false;
    int loopExit_ = 0;

    void runEditorLoop();
    void startQuitPrompt();
    void startSaveAsPrompt(bool thenQuit);
    void refreshSaveAsPrompt();

    void handleNormalKey(const Key& k);
    void handlePromptKey(const Key& k);
    void moveLeft();
    void moveRight();
    void moveUp();
    void moveDown();
    void pageUp();
    void pageDown();
    void clampCursor();
};
