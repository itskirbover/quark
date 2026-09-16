#pragma once
// Main menu shown when quark starts without a file: centered logo
// (Kitty graphics, ASCII fallback) + Open / New / Quit.

#include <string>
#include <vector>

#include "Input.hpp"
#include "Kitty.hpp"

class Terminal;

struct MenuResult {
    enum class Action { Open, New, Quit };
    Action action = Action::Quit;
    std::string path;  // valid for Open
};

class Menu {
public:
    Menu(Terminal& term, InputReader& input, const KittySupport& supp);

    // Blocks until the user picks an action.
    MenuResult show();

private:
    Terminal& term_;
    InputReader& input_;
    const KittySupport& supp_;

    int selected_ = 0;  // 0=open, 1=new, 2=quit
    bool transmitted_ = false;
    bool askingPath_ = false;
    std::string pathBuf_;
    std::string status_;
    std::vector<int> itemRows_;  // 1-based screen row per item (click map)
    std::vector<int> itemCols_;  // 1-based screen col of each item label
    // Terminal cell size in pixels (for natural-size logo math).
    int cellW_ = 9;
    int cellH_ = 18;

    void draw();
    void activate(int idx, MenuResult& out, bool& done);
};
