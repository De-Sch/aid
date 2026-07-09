#include "aid/domain/CommentText.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace aid::domain {

namespace {

// An empty paragraph, the only markdown spelling of a line you can put a cursor
// into. Emitted between the two newlines that already delimit the blank line.
constexpr std::string_view kEmptyLine = "<br>\n\n";

// Normalise CRLF and bare CR to LF. A <textarea> hands us LF, but a paste or a
// non-browser client may not, and a stray CR would end up inside the stored
// markdown.
[[nodiscard]] std::string toLf(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') {
                ++i;
            }
            out += '\n';
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

std::string CommentText::toMarkdown(std::string_view typed) {
    const std::string text = toLf(typed);

    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '\n') {
            out += text[i];
            ++i;
            continue;
        }

        // Consume the whole run of newlines at once: its length decides how many
        // blank lines the operator typed. n == 1 is a plain line break, which
        // OpenProject already renders as one; n >= 2 leaves n-1 blank lines,
        // each of which needs its own "<br>" paragraph to survive as a line the
        // cursor can enter.
        std::size_t n = 0;
        while (i < text.size() && text[i] == '\n') {
            ++n;
            ++i;
        }

        if (n == 1) {
            out += '\n';
            continue;
        }
        out += "\n\n";
        for (std::size_t blank = 0; blank + 1 < n; ++blank) {
            out += kEmptyLine;
        }
    }
    return out;
}

} // namespace aid::domain
