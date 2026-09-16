#pragma once
#include <string>

// Kitty text sizing protocol.
// See https://github.com/kovidgoyal/kitty/blob/master/docs/text-sizing-protocol.rst
//
// Escape format:  ESC ] _text_size_code ; metadata ; text BEL
//   ESC ]      = 0x1b 0x5d
//   metadata   = colon-separated key=value pairs
//   BEL        = 0x07
//
// Metadata keys:
//   s  overall scale, integer 1..7 (text rendered in s*w by s cells)
//   w  width in scaled cells, 0..7 (0 = auto)
//   n  fractional numerator, 0..15
//   d  fractional denominator, 0..15 (must be > n when non-zero)
//   v  vertical alignment (fractional only): 0 top, 1 bottom, 2 centered
//   h  horizontal alignment (fractional only): 0 left, 1 right, 2 centered

namespace textsizing {

// Render `text` at the given size. Only non-default metadata keys are emitted.
// s in 1..7; w in 0..7; n/d fractional (d > n when both non-zero).
std::string render(const std::string& text, int s = 1, int w = 0,
                   int n = 0, int d = 0, int v = 0, int h = 0);

// Build just the OSC escape code with the given metadata string and text.
std::string osc(const std::string& metadata, const std::string& text);

enum class Support { None, Width, Scale };

// Detect whether the terminal supports the width and/or scale parts of the
// protocol. Requires the terminal to be in raw mode and stdin readable.
// See "Detecting if the terminal supports this protocol" in the spec.
Support detect();

} // namespace textsizing
