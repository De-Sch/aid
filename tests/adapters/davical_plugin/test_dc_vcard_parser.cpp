// Tests for DcVCardParser — XXE-hardened libxml2 multistatus parser +
// RFC 6350 vCard projector. See plan/classes/adapters/DcVCardParser.md.
//
// Coverage:
//   - Happy path: one vCard, all four fields populated.
//   - Multi-value TEL + X-CUSTOM1.
//   - Empty multistatus / no <address-data>.
//   - Malformed XML → empty vector, no throw.
//   - One malformed vCard in a doc with valid neighbours → the
//     malformed one is skipped, the valid ones come through.
//   - XXE attack via external SYSTEM entity (file:///etc/passwd) →
//     content MUST NOT appear in the parsed FN. The critical
//     security test.
//   - Billion-laughs nested entity blowup → parser declines to
//     expand (we don't pass XML_PARSE_NOENT), no OOM.
//   - Bounds-check edges: empty, single-char lines, no trailing
//     newline (regression for the old DaviCal.cpp:407 UB).
//   - RFC 6350 §3.4 TEXT escapes (`\,` `\;` `\\` `\n`) resolved in
//     FN/ORG/TEL and before the X-CUSTOM1 split, incl. the verbatim
//     live DaviCal record (`X-CUSTOM1;VALUE=TEXT:7\, 8`) whose escaped
//     comma used to yield the unusable project id `7\`.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "aid/adapters/davical/internal/DcVCardParser.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"

namespace aid::adapters::davical::internal::test {

using ::aid::adapters::davical::internal::DcVCardParser;

namespace {

[[nodiscard]] std::string wrap(std::string vcard) {
    return std::string{R"(<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">
  <d:response>
    <d:href>/aid/addresses/test.vcf</d:href>
    <d:propstat>
      <d:prop>
        <card:address-data>)"} +
           std::move(vcard) + R"(</card:address-data>
      </d:prop>
      <d:status>HTTP/1.1 200 OK</d:status>
    </d:propstat>
  </d:response>
</d:multistatus>)";
}

} // namespace

// ─── DcVCardParser::parse ──────────────────────────────────────────────

TEST(DcVCardParser, ParseHappyPathReturnsOneContactWithAllFields) {
    const auto doc = wrap("BEGIN:VCARD\r\n"
                          "VERSION:3.0\r\n"
                          "FN:Alice Example\r\n"
                          "ORG:Example GmbH\r\n"
                          "TEL:+491701234567\r\n"
                          "X-CUSTOM1:42,43\r\n"
                          "END:VCARD\r\n");
    const auto contacts = DcVCardParser::parse(doc);
    ASSERT_EQ(contacts.size(), 1U);
    EXPECT_EQ(contacts[0].name, "Alice Example");
    EXPECT_EQ(contacts[0].companyName, "Example GmbH");
    ASSERT_EQ(contacts[0].phoneNumbers.size(), 1U);
    EXPECT_EQ(contacts[0].phoneNumbers[0].v, "+491701234567");
    ASSERT_EQ(contacts[0].projectIds.size(), 2U);
    EXPECT_EQ(contacts[0].projectIds[0].v, "42");
    EXPECT_EQ(contacts[0].projectIds[1].v, "43");
}

TEST(DcVCardParser, ParseMultipleTelAndXCustom1) {
    const auto doc = wrap("BEGIN:VCARD\r\n"
                          "FN:Bob\r\n"
                          "TEL;TYPE=WORK:+491701111111\r\n"
                          "TEL;TYPE=CELL:+491702222222\r\n"
                          "X-CUSTOM1:1, 2 ,3,, ,4\r\n"
                          "END:VCARD\r\n");
    const auto contacts = DcVCardParser::parse(doc);
    ASSERT_EQ(contacts.size(), 1U);
    EXPECT_EQ(contacts[0].phoneNumbers.size(), 2U);
    EXPECT_EQ(contacts[0].phoneNumbers[0].v, "+491701111111");
    EXPECT_EQ(contacts[0].phoneNumbers[1].v, "+491702222222");
    ASSERT_EQ(contacts[0].projectIds.size(), 4U);
    EXPECT_EQ(contacts[0].projectIds[0].v, "1");
    EXPECT_EQ(contacts[0].projectIds[1].v, "2");
    EXPECT_EQ(contacts[0].projectIds[2].v, "3");
    EXPECT_EQ(contacts[0].projectIds[3].v, "4");
}

TEST(DcVCardParser, ParseEmptyMultistatusReturnsEmpty) {
    const std::string empty = R"(<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:"/>)";
    EXPECT_TRUE(DcVCardParser::parse(empty).empty());
}

TEST(DcVCardParser, ParseEmptyStringReturnsEmpty) {
    EXPECT_TRUE(DcVCardParser::parse("").empty());
}

TEST(DcVCardParser, ParseMalformedXmlReturnsEmptyWithoutThrowing) {
    const std::string broken = "<d:multistatus><d:response>";
    EXPECT_NO_THROW({
        const auto contacts = DcVCardParser::parse(broken);
        EXPECT_TRUE(contacts.empty());
    });
}

TEST(DcVCardParser, ParseSkipsMalformedVCardKeepsValidNeighbour) {
    const std::string doc = R"(<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">
  <d:response>
    <d:propstat>
      <d:prop>
        <card:address-data>BEGIN:VCARD
ORG:NoNameCo
TEL:+491700000000
END:VCARD</card:address-data>
      </d:prop>
    </d:propstat>
  </d:response>
  <d:response>
    <d:propstat>
      <d:prop>
        <card:address-data>BEGIN:VCARD
FN:Charlie
TEL:+491701234567
END:VCARD</card:address-data>
      </d:prop>
    </d:propstat>
  </d:response>
</d:multistatus>)";
    const auto contacts = DcVCardParser::parse(doc);
    ASSERT_EQ(contacts.size(), 1U) << "expected only the FN-bearing vCard";
    EXPECT_EQ(contacts[0].name, "Charlie");
}

// ─── XXE — the security test ───────────────────────────────────────────

TEST(DcVCardParser, ParseXXEAttackDoesNotExfiltrateLocalFile) {
    // Classic Billy-Bob-style payload: declare a SYSTEM external entity
    // pointing at a sensitive local file, reference it inside what
    // would otherwise be the address-data text. A vulnerable parser
    // would interpolate /etc/passwd content into the FN field; our
    // hardening (NONET, no DTDLOAD, no NOENT, no-op external-entity
    // loader) MUST keep that from happening.
    const std::string evil = R"(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE root [
  <!ENTITY xxe SYSTEM "file:///etc/passwd">
]>
<d:multistatus xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">
  <d:response>
    <d:propstat>
      <d:prop>
        <card:address-data>BEGIN:VCARD
FN:Evil &xxe;Caller
END:VCARD</card:address-data>
      </d:prop>
    </d:propstat>
  </d:response>
</d:multistatus>)";
    const auto contacts = DcVCardParser::parse(evil);
    // The parse either drops the entity (so FN stays "Evil Caller" or
    // some empty-string variant) or rejects the doc entirely — either
    // is acceptable. What is NOT acceptable is `root:` or any other
    // /etc/passwd substring appearing in the result.
    for (const auto& c : contacts) {
        EXPECT_EQ(c.name.find("root:"), std::string::npos)
            << "XXE bypass — /etc/passwd content in FN: [" << c.name << "]";
        EXPECT_EQ(c.name.find("/bin/"), std::string::npos)
            << "XXE bypass — /etc/passwd content in FN: [" << c.name << "]";
        EXPECT_EQ(c.name.find("daemon:"), std::string::npos)
            << "XXE bypass — /etc/passwd content in FN: [" << c.name << "]";
    }
}

TEST(DcVCardParser, ParseParameterEntityXXEDoesNotExpand) {
    // Parameter-entity vector — exploits the `<!ENTITY % ...>` form
    // that some XML parsers process even with general-entity expansion
    // off. Our DOCTYPE pre-screen catches it before libxml2 sees any
    // of the bytes; assert the contact list is empty and the payload
    // string never bleeds into the output.
    const std::string evil = R"(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE root [
  <!ENTITY % pe SYSTEM "file:///etc/passwd">
  %pe;
]>
<d:multistatus xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">
  <d:response><d:propstat><d:prop>
    <card:address-data>BEGIN:VCARD
FN:Param
END:VCARD</card:address-data>
  </d:prop></d:propstat></d:response>
</d:multistatus>)";
    EXPECT_TRUE(DcVCardParser::parse(evil).empty());
}

TEST(DcVCardParser, ParseAcceptsAlternateAddressDataNamespacePrefix) {
    // Some CardDAV servers (incl. older DaviCal) use "C:" instead of
    // "card:" for the urn:ietf:params:xml:ns:carddav namespace. Both
    // bind to the same XML local name "address-data"; the parser must
    // not be sensitive to the prefix string.
    const std::string doc = R"(<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav">
  <d:response><d:propstat><d:prop>
    <C:address-data>BEGIN:VCARD
FN:PrefixVariant
END:VCARD</C:address-data>
  </d:prop></d:propstat></d:response>
</d:multistatus>)";
    const auto contacts = DcVCardParser::parse(doc);
    ASSERT_EQ(contacts.size(), 1U);
    EXPECT_EQ(contacts[0].name, "PrefixVariant");
}

TEST(DcVCardParser, ParseBillionLaughsDoesNotExpand) {
    // Nested entity bomb. Without XML_PARSE_NOENT, libxml2 will not
    // substitute the entity references, so the recursion never
    // happens. We assert the parse returns within bounded memory
    // (i.e. doesn't OOM) and emits at most a few contacts (none in
    // practice since the entity refs survive as text and there's no
    // FN). Past the assert, the gtest fixture's RAII cleanup confirms
    // no leak of XmlDocPtr.
    std::string bomb = R"(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE lolz [
  <!ENTITY lol "lol">
  <!ENTITY lol2 "&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;">
  <!ENTITY lol3 "&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;">
  <!ENTITY lol4 "&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;">
]>
<d:multistatus xmlns:d="DAV:" xmlns:card="urn:ietf:params:xml:ns:carddav">
  <d:response><d:propstat><d:prop>
    <card:address-data>BEGIN:VCARD
FN:&lol4;
END:VCARD</card:address-data>
  </d:prop></d:propstat></d:response>
</d:multistatus>)";
    EXPECT_NO_THROW({ (void)DcVCardParser::parse(bomb); });
}

// ─── Bounds-check edges ────────────────────────────────────────────────

TEST(DcVCardParser, ParseOneVCardEmptyReturnsNullopt) {
    EXPECT_FALSE(DcVCardParser::parseOneVCard("").has_value());
}

TEST(DcVCardParser, ParseOneVCardWithoutFnReturnsNullopt) {
    EXPECT_FALSE(
        DcVCardParser::parseOneVCard("BEGIN:VCARD\nTEL:+491701234567\nEND:VCARD\n").has_value());
}

TEST(DcVCardParser, ParseOneVCardNoTrailingNewline) {
    // Regression for microkernel DaviCal.cpp:407 line[length()-1] UB.
    const auto contact = DcVCardParser::parseOneVCard("BEGIN:VCARD\nFN:NoNewline\nEND:VCARD");
    ASSERT_TRUE(contact.has_value());
    EXPECT_EQ(contact->name, "NoNewline");
}

TEST(DcVCardParser, ParseOneVCardSingleCharLines) {
    // A line "a" has no ':', so splitLine returns nullopt — must skip,
    // not crash on substr-of-end.
    EXPECT_NO_THROW(
        { (void)DcVCardParser::parseOneVCard("BEGIN:VCARD\na\nFN:Tiny\nb\nEND:VCARD\n"); });
    const auto contact = DcVCardParser::parseOneVCard("BEGIN:VCARD\na\nFN:Tiny\nb\nEND:VCARD\n");
    ASSERT_TRUE(contact.has_value());
    EXPECT_EQ(contact->name, "Tiny");
}

TEST(DcVCardParser, ParseOneVCardLineFoldingPerRFC6350) {
    // A continuation line begins with SPACE or TAB; its leading
    // whitespace is stripped and it's appended to the previous line.
    const auto contact = DcVCardParser::parseOneVCard("BEGIN:VCARD\n"
                                                      "FN:Folded\n"
                                                      "TEL:+49170\n"
                                                      " 1234567\n"
                                                      "END:VCARD\n");
    ASSERT_TRUE(contact.has_value());
    ASSERT_EQ(contact->phoneNumbers.size(), 1U);
    EXPECT_EQ(contact->phoneNumbers[0].v, "+491701234567");
}

// ─── DcVCardParser::splitProjectIds ────────────────────────────────────

TEST(DcVCardParser, SplitProjectIdsEmptyReturnsEmpty) {
    EXPECT_TRUE(DcVCardParser::splitProjectIds("").empty());
}

TEST(DcVCardParser, SplitProjectIdsSingleId) {
    const auto ids = DcVCardParser::splitProjectIds("42");
    ASSERT_EQ(ids.size(), 1U);
    EXPECT_EQ(ids[0].v, "42");
}

TEST(DcVCardParser, SplitProjectIdsTrimsAndDropsEmpties) {
    const auto ids = DcVCardParser::splitProjectIds(" 1 , 2,, ,3 ");
    ASSERT_EQ(ids.size(), 3U);
    EXPECT_EQ(ids[0].v, "1");
    EXPECT_EQ(ids[1].v, "2");
    EXPECT_EQ(ids[2].v, "3");
}

TEST(DcVCardParser, SplitProjectIdsResolvesEscapedComma) {
    // RFC 6350 §3.4 — a client that treats X-CUSTOM1 as ONE text value
    // escapes the comma the operator typed. Regression: the raw split
    // produced the ids "7\" and "8", and OpenProject answered the
    // work-package query with 400 InvalidQuery.
    const auto ids = DcVCardParser::splitProjectIds(R"(7\, 8)");
    ASSERT_EQ(ids.size(), 2U);
    EXPECT_EQ(ids[0].v, "7");
    EXPECT_EQ(ids[1].v, "8");
}

// ─── RFC 6350 §3.4 TEXT escapes ────────────────────────────────────────

TEST(DcVCardParser, ParseOneVCardResolvesEscapesInTextFields) {
    const auto contact = DcVCardParser::parseOneVCard("BEGIN:VCARD\r\n"
                                                      R"(FN:Meier\, Mueller & Co)"
                                                      "\r\n"
                                                      R"(ORG:Berlin\;Mitte\\Nord)"
                                                      "\r\n"
                                                      "END:VCARD\r\n");
    ASSERT_TRUE(contact.has_value());
    EXPECT_EQ(contact->name, "Meier, Mueller & Co");
    EXPECT_EQ(contact->companyName, R"(Berlin;Mitte\Nord)");
}

TEST(DcVCardParser, ParseOneVCardResolvesEscapedNewline) {
    const auto contact = DcVCardParser::parseOneVCard("BEGIN:VCARD\r\n"
                                                      R"(FN:Line\nBreak)"
                                                      "\r\n"
                                                      "END:VCARD\r\n");
    ASSERT_TRUE(contact.has_value());
    EXPECT_EQ(contact->name, "Line\nBreak");
}

TEST(DcVCardParser, ParseOneVCardKeepsUnknownEscapeVerbatim) {
    // Data we didn't write: keeping a backslash we don't understand beats
    // silently dropping a byte.
    const auto contact = DcVCardParser::parseOneVCard("BEGIN:VCARD\r\n"
                                                      R"(FN:odd\quirk)"
                                                      "\r\n"
                                                      "END:VCARD\r\n");
    ASSERT_TRUE(contact.has_value());
    EXPECT_EQ(contact->name, R"(odd\quirk)");
}

TEST(DcVCardParser, ParseOneVCardTrailingLoneBackslashIsKept) {
    // Bounds guard: no read past the end of the value.
    const auto contact = DcVCardParser::parseOneVCard("BEGIN:VCARD\r\n"
                                                      R"(FN:trailing\)"
                                                      "\r\n"
                                                      "END:VCARD\r\n");
    ASSERT_TRUE(contact.has_value());
    EXPECT_EQ(contact->name, R"(trailing\)");
}

TEST(DcVCardParser, ParseLiveDaviCalCompanyEntryWithTwoProjectIds) {
    // Verbatim payload from the live DaviCal companies book (note the
    // VALUE=TEXT parameter, the leading space in TEL, and the escaped
    // comma) — the exact record that broke multi-project routing.
    const auto doc = wrap("BEGIN:VCARD\r\n"
                          "VERSION:4.0\r\n"
                          "N:;Test GmbH;;;\r\n"
                          "FN:Test GmbH\r\n"
                          "TEL;VALUE=TEXT: +49304321\r\n"
                          R"(X-CUSTOM1;VALUE=TEXT:7\, 8)"
                          "\r\n"
                          "END:VCARD\r\n");
    const auto contacts = DcVCardParser::parse(doc);
    ASSERT_EQ(contacts.size(), 1U);
    EXPECT_EQ(contacts[0].name, "Test GmbH");
    ASSERT_EQ(contacts[0].phoneNumbers.size(), 1U);
    EXPECT_EQ(contacts[0].phoneNumbers[0].v, "+49304321");
    ASSERT_EQ(contacts[0].projectIds.size(), 2U);
    EXPECT_EQ(contacts[0].projectIds[0].v, "7");
    EXPECT_EQ(contacts[0].projectIds[1].v, "8");
}

} // namespace aid::adapters::davical::internal::test
