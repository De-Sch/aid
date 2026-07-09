#include <gtest/gtest.h>

#include <string>

#include "aid/domain/CommentText.h"

namespace {

using aid::domain::CommentText;

// OpenProject renders a bare "\n" as a line break (hardbreaks), so a typed
// single Enter needs no help and must be passed through untouched.
TEST(CommentText, SingleNewline_PassedThroughUntouched) {
    EXPECT_EQ(CommentText::toMarkdown("line one\nline two"), "line one\nline two");
}

TEST(CommentText, SingleLine_Unchanged) {
    EXPECT_EQ(CommentText::toMarkdown("please call back"), "please call back");
}

TEST(CommentText, Empty_Unchanged) {
    EXPECT_EQ(CommentText::toMarkdown(""), "");
}

TEST(CommentText, ManyLines_NoneGainMarkup) {
    EXPECT_EQ(CommentText::toMarkdown("a\nb\nc"), "a\nb\nc");
}

// The one case markdown cannot express: an empty line. "\n\n" alone would only
// start a new paragraph, which renders as spacing the cursor cannot enter, so
// the blank line becomes the "<br>" paragraph OpenProject's own editor writes.
TEST(CommentText, BlankLine_BecomesEmptyLineParagraph) {
    EXPECT_EQ(CommentText::toMarkdown("above\n\nbelow"), "above\n\n<br>\n\nbelow");
}

TEST(CommentText, TwoBlankLines_BecomeTwoEmptyLineParagraphs) {
    EXPECT_EQ(CommentText::toMarkdown("above\n\n\nbelow"), "above\n\n<br>\n\n<br>\n\nbelow");
}

TEST(CommentText, MixedBreaksAndBlankLines) {
    EXPECT_EQ(CommentText::toMarkdown("a\nb\n\nc"), "a\nb\n\n<br>\n\nc");
}

// A <textarea> hands us LF, but a paste or a non-browser client may send CRLF.
TEST(CommentText, Crlf_NormalisedToLf) {
    EXPECT_EQ(CommentText::toMarkdown("line one\r\nline two"), "line one\nline two");
}

TEST(CommentText, CrlfBlankLine_BecomesEmptyLineParagraph) {
    EXPECT_EQ(CommentText::toMarkdown("above\r\n\r\nbelow"), "above\n\n<br>\n\nbelow");
}

TEST(CommentText, BareCr_NormalisedToLf) {
    EXPECT_EQ(CommentText::toMarkdown("line one\rline two"), "line one\nline two");
}

// Whatever the operator types is content, not markup.
TEST(CommentText, OtherCharacters_NotEscaped) {
    EXPECT_EQ(CommentText::toMarkdown("a <b> & # c\nd"), "a <b> & # c\nd");
}

} // namespace
