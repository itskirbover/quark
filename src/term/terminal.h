#pragma once
#include <string>
#include <termios.h>

namespace terminal {

// RAII: enters raw mode on construction, restores on destruction.
struct RawMode {
    RawMode();
    ~RawMode();
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;
    termios saved_{};
    bool active_{false};
};

struct Size { int rows = 0, cols = 0; };

// Current terminal size (rows x cols) via ioctl.
Size size();

// Alternate screen buffer handling.
void enter_alt_screen();
void leave_alt_screen();

// Returns true if stdin has data available within timeout_ms (<=0 means poll 0).
bool stdin_ready(int timeout_ms);

// Read a single byte from stdin (blocking). Returns -1 on EOF/error.
int read_byte();

} // namespace terminal
