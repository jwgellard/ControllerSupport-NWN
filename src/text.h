// Game text for the overlay. Pure logic, so the offline harness can check it.
#pragma once

#include <cstddef>
#include <string>

namespace text {

// A game string (Windows-1252, maybe with <cRGB>...</c> colour tags or dialogue
// <StartAction>/<StartCheck>/<StartHighlight>...</Start> tags) as one line of UTF-8.
// When maxChars is set and the text is longer, a trailing "(...)" note is dropped
// first, then it is cut at a word with "...".
std::string Display(const char* raw, size_t maxChars = 0);

} // namespace text
