#include <string>

#include "Editor.hpp"

int main(int argc, char** argv) {
    // No argument: main menu (open / new / quit). With one: straight to
    // the editor (missing file = fresh buffer, see Buffer::open).
    std::string path;
    if (argc >= 2) path = argv[1];
    Editor editor;
    return editor.run(path);
}
