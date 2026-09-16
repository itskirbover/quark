#pragma once
#include <string>
#include <vector>

namespace uni {

// Decode a UTF-8 string into code points. Invalid bytes are preserved as
// individual code points in the range 0x80..0xFF so round-tripping keeps them.
std::vector<uint32_t> decode(const std::string& s);

// Re-encode a sequence of code points back into a UTF-8 string.
std::string encode(const std::vector<uint32_t>& cps);

// Display width of a single code point in terminal cells.
int codepoint_width(uint32_t cp);

// Display width of a UTF-8 string in terminal cells.
int display_width(const std::string& s);

// Number of UTF-8 code points in a string.
size_t length(const std::string& s);

} // namespace uni
