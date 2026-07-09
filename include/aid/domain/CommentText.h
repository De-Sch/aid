#pragma once

#include <string>
#include <string_view>

namespace aid::domain {

// Convert freshly typed dashboard comment text into the markdown dialect
// OpenProject stores in `work_package.description` (`{"format":"markdown"}`),
// so a comment renders in the work package exactly as it was typed.
//
// Only ONE thing needs translating. OpenProject's renderer has hardbreaks on --
// a bare "\n" already renders as a line break, verified against its own
// /api/v3/render/markdown:
//
//     "a\nb"              ->  <p>a<br>b</p>            (tight break)
//     "a\n\nb"            ->  <p>a</p><p>b</p>         (two paragraphs)
//     "a\n\n<br>\n\nb"    ->  <p>a</p><br><p>b</p>     (a real empty line)
//
// -- so single newlines are left alone. But markdown has no way to spell an
// EMPTY line: "\n\n" only starts a new paragraph, which renders as spacing you
// cannot put a cursor into. A blank line the operator typed must therefore
// become "<br>", which is precisely what OpenProject's own CKEditor writes for
// an empty paragraph. Text authored on either side then round-trips through the
// other unchanged, and the dashboard's comment view reads 1:1 with the work
// package (see `commentDisplayLines` in ui/src/lib/format.js).
class CommentText {
public:
    // Normalise CRLF/CR to LF, then give every blank line an explicit "<br>"
    // paragraph. Single newlines and all other characters pass through
    // untouched -- the operator's text is content, not markup, and is not
    // markdown-escaped.
    //
    // Run this ONCE, on freshly typed composer input. Never run it on a value
    // read back from OpenProject: `parseFromHal` returns `description.raw`
    // verbatim and AppendComment's reducer appends to that value, so a second
    // pass would wrap the "<br>" paragraphs it already contains in further ones.
    [[nodiscard]] static std::string toMarkdown(std::string_view typed);
};

} // namespace aid::domain
