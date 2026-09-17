#pragma once
// Editor: main loop, cursor, selection, undo/redo,
// quit-with-save-prompt (Ctrl-C -> y/n).

#include <string>
#include <vector>

#include "Buffer.hpp"
#include "Input.hpp"
#include "Kitty.hpp"
#include "Renderer.hpp"
#include "Terminal.hpp"

// Snapshot for undo/redo: full line text plus cursor/selection.
struct HistState {
    std::vector<std::string> lines;
    size_t cx = 0, cy = 0;
    size_t ax = 0, ay = 0;
    bool selecting = false;
};

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
    // Text selection: anchor (ax_/ay_) + cursor head (cx_/cy_).
    bool selecting_ = false;
    size_t ax_ = 0, ay_ = 0;
    // Undo/redo stacks (bounded); typing groups consecutive inserts.
    std::vector<HistState> undo_;
    std::vector<HistState> redo_;
    bool typingActive_ = false;
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
    // Selection helpers: extend=true keeps/starts the selection at the
    // pre-move cursor; extend=false clears it.
    void applyExtend(bool extend);
    void moveWithExtend(bool extend, void (Editor::*move)());
    bool hasSelection() const;
    void normSel(size_t& y0, size_t& x0, size_t& y1, size_t& x1) const;
    void clearSelection();
    // Erases the selection (cursor moves to its start). No history push;
    // the caller pushes once so replace-typing stays a single undo step.
    // Returns false when there was no selection.
    bool eraseSelection();
    // History: snapshot current state before a mutation (coalesce groups
    // consecutive typing into one undo step); new edits clear redo.
    void pushHistory(bool coalesceTyping);
    void breakTyping();
    void doUndo();
    void doRedo();
};
