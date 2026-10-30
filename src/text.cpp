#include "text.h"

#include <cstring>

namespace text {
namespace {

// Windows-1252 0x80-0x9f: the few that turn up in game text, as ASCII; the rest dropped.
const char* Cp1252High(unsigned char c)
{
    switch (c) {
    case 0x85: return "...";
    case 0x91: case 0x92: return "'";
    case 0x93: case 0x94: return "\"";
    case 0x96: case 0x97: return "-";
    default: return "";
    }
}

// Length of the markup tag at raw[i], or 0: <c + three colour bytes + >, </c>, and the
// dialogue tags <StartAction>, <StartCheck>, <StartHighlight> and </Start>.
size_t TagLength(const char* raw, size_t i, size_t n)
{
    if (raw[i] != '<')
        return 0;
    if (i + 5 < n && raw[i + 1] == 'c' && raw[i + 5] == '>')
        return 6;
    static const char* const kTags[] = {"</c>", "</Start>", "<StartAction>", "<StartCheck>", "<StartHighlight>"};
    for (const char* t : kTags)
        if (strncmp(raw + i, t, strlen(t)) == 0)
            return strlen(t);
    return 0;
}

// Plain characters on one line: tags gone, control characters and runs of spaces
// folded to one space, no leading or trailing space.
std::string Plain(const char* raw)
{
    std::string out;
    size_t n = strlen(raw);
    for (size_t i = 0; i < n;) {
        if (size_t tag = TagLength(raw, i, n)) {
            i += tag;
            continue;
        }
        unsigned char c = static_cast<unsigned char>(raw[i++]);
        if (c < 0x20 || c == ' ' || c == 0xa0) {
            if (!out.empty() && out.back() != ' ')
                out += ' ';
        } else {
            out += static_cast<char>(c);
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

} // namespace

std::string Display(const char* raw, size_t maxChars)
{
    std::string plain = raw ? Plain(raw) : "";
    if (maxChars >= 4 && plain.size() > maxChars) {
        size_t note = plain.rfind(" (");
        if (plain.back() == ')' && note != std::string::npos && note > 0)
            plain.erase(note);
    }
    if (maxChars >= 4 && plain.size() > maxChars) {
        size_t cut = maxChars - 3;
        size_t space = plain.rfind(' ', cut);
        if (space != std::string::npos && space > cut / 2)
            cut = space;
        plain = plain.substr(0, cut);
        while (!plain.empty() && (plain.back() == ' ' || plain.back() == ',' || plain.back() == ':'))
            plain.pop_back();
        plain += "...";
    }
    std::string utf8;
    for (unsigned char c : plain) {
        if (c < 0x80)
            utf8 += static_cast<char>(c);
        else if (c < 0xa0)
            utf8 += Cp1252High(c);
        else { // 0xa1-0xff match Latin-1
            utf8 += static_cast<char>(0xc0 | (c >> 6));
            utf8 += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return utf8;
}

} // namespace text
