#include <iostream>
#include <string>

#include "Editor.hpp"

int main(int argc, char** argv) {
    std::string path;
    if (argc >= 2) {
        path = argv[1];
    } else {
        std::cerr << "Usage: quark <file.md>\n";
        return 2;
    }
    Editor editor;
    return editor.run(path);
}
