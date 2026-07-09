#include "aid/adapters/davical/internal/DcVCardParser.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlstring.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"

namespace aid::adapters::davical::internal {

namespace {

// RAII wrappers for the small slice of libxml2 surface this parser uses —
// formerly xml_raii.h, folded here as this .cpp is (and was) the only
// consumer. libxml2 is a PRIVATE link dep of aid_davical_internals, so these
// symbols never leave the .so.
struct XmlDocDeleter {
    void operator()(xmlDoc* d) const noexcept {
        if (d != nullptr) {
            xmlFreeDoc(d);
        }
    }
};
using XmlDocPtr = std::unique_ptr<xmlDoc, XmlDocDeleter>;

// libxml2 helpers (xmlNodeGetContent, xmlGetProp) return xmlChar* (unsigned
// char*) that must be released with xmlFree, its internal free.
struct XmlCharDeleter {
    void operator()(xmlChar* p) const noexcept {
        if (p != nullptr) {
            xmlFree(p);
        }
    }
};
using XmlCharPtr = std::unique_ptr<xmlChar, XmlCharDeleter>;

// XXE-hardening initialiser. Three layers of defence:
//
//   1. xmlInitParser() called exactly once per process (thread-safe).
//   2. xmlSetExternalEntityLoader() overridden to a no-op that returns
//      nullptr for every lookup — even if a malicious vCard sneaks in
//      a SYSTEM external entity, libxml2 cannot fetch its content.
//   3. Parse options at xmlReadMemory time disable network access,
//      DTD loading, and entity expansion.
//
// Per .claude/rules/plugins.md we never call xmlCleanupParser() — the
// plugin is loaded once at main() and held until process exit; libxml2's
// global state is fine to leak.
xmlParserInputPtr noopExternalEntityLoader(const char* /*url*/, const char* /*id*/,
                                           xmlParserCtxtPtr /*ctxt*/) noexcept {
    return nullptr;
}

void initHardenedXml() {
    static std::once_flag once;
    std::call_once(once, [] {
        xmlInitParser();
        xmlSetExternalEntityLoader(noopExternalEntityLoader);
    });
}

constexpr int kHardenedParseOptions =
    XML_PARSE_NONET |   // No network fetches under any circumstance
    XML_PARSE_NOCDATA | // Treat CDATA as text (single representation)
    XML_PARSE_NODICT;   // Don't intern strings into the parser dict;
                        // makes the doc fully self-contained.

// Deliberately absent: XML_PARSE_DTDLOAD (load external DTD),
// XML_PARSE_NOENT (substitute entities), XML_PARSE_DTDATTR
// (load DTD-specified attribute defaults), XML_PARSE_DTDVALID
// (DTD-driven validation). Any of these enabled would re-open the
// XXE vector that initHardenedXml + NONET are trying to close.

// ─── Tree traversal helpers ────────────────────────────────────────────

// Returns true if `node` is an element whose local (namespace-stripped)
// name equals `name`. CardDAV servers use different prefixes ("C:",
// "card:") for the same urn:ietf:params:xml:ns:carddav namespace, so we
// match on the local name; the only thing we care about content-wise
// is the <address-data> element, whose local name is stable.
[[nodiscard]] bool isElement(const xmlNode* node, const char* name) noexcept {
    if (node == nullptr || node->type != XML_ELEMENT_NODE || node->name == nullptr) {
        return false;
    }
    return std::strcmp(reinterpret_cast<const char*>(node->name), name) == 0;
}

// Recursive walk: for every element with the given local name, push
// its text content into `out`. xmlNodeGetContent guarantees a
// null-terminated xmlChar* (libxml2 contract; the implicit
// std::string(const char*) ctor relies on that to stop at the right
// byte). libxml2 also caps element-tree depth at ~256 by default
// (XML_PARSE_HUGE off), so the recursion is bounded.
void collectTextOf(const xmlNode* node, const char* name, std::vector<std::string>& out) {
    for (const xmlNode* cur = node; cur != nullptr; cur = cur->next) {
        if (isElement(cur, name)) {
            XmlCharPtr txt{xmlNodeGetContent(const_cast<xmlNode*>(cur))};
            if (txt) {
                out.emplace_back(reinterpret_cast<const char*>(txt.get()));
            }
        }
        if (cur->children != nullptr) {
            collectTextOf(cur->children, name, out);
        }
    }
}

// ─── vCard line operations (RFC 6350 §3.2 line folding) ────────────────

// Unfold a vCard: any line beginning with SPACE or TAB is a
// continuation of the previous line (the leading whitespace is
// stripped). Output is one logical line per vector entry.
[[nodiscard]] std::vector<std::string> unfoldLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    auto flush = [&] {
        if (!current.empty()) {
            lines.push_back(std::move(current));
            current.clear();
        }
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto eol = text.find('\n', i);
        const auto end = (eol == std::string_view::npos) ? text.size() : eol;
        std::string_view raw = text.substr(i, end - i);
        // Strip trailing '\r' if CRLF.
        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }
        if (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) {
            // Continuation: append after the leading whitespace.
            current.append(raw.data() + 1, raw.size() - 1);
        } else {
            flush();
            current.assign(raw);
        }
        i = (eol == std::string_view::npos) ? text.size() : eol + 1;
    }
    flush();
    return lines;
}

[[nodiscard]] std::string toUpper(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

// Each unfolded vCard line is `NAME[;PARAM=VAL[;...]]:VALUE`. Splits
// the name (uppercased so callers can do case-insensitive matching)
// from the value. Returns nullopt on a line without ':'.
struct VCardLine {
    std::string nameUpper;
    std::string_view value;
};

[[nodiscard]] std::optional<VCardLine> splitLine(std::string_view line) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto head = line.substr(0, colon);
    // Drop any params after the first ';' in the head.
    const auto semi = head.find(';');
    if (semi != std::string_view::npos) {
        head = head.substr(0, semi);
    }
    return VCardLine{toUpper(head), line.substr(colon + 1)};
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// RFC 6350 §3.4 — inside a TEXT value, "\\", "\," and "\;" stand for the
// literal character and "\n" / "\N" for a newline. An address-book client
// that treats a property as ONE text value escapes every comma the operator
// typed, so "7, 8" reaches us stored as `7\, 8` (observed live in DaviCal:
// `X-CUSTOM1;VALUE=TEXT:7\, 8`). Without this pass the backslash survives
// into the ProjectId and OpenProject rejects the work-package query with
// 400 InvalidQuery ("Project filter has invalid values") — the whole call
// then fails and the caller gets no ticket at all.
//
// Unknown escapes (e.g. `\q`) are passed through verbatim, backslash and
// all: this is data we did not write, and losing a byte is worse than
// keeping one we don't understand.
[[nodiscard]] std::string unescapeText(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            // Also covers a trailing lone backslash — emitted as-is, never
            // read past the end.
            out.push_back(s[i]);
            continue;
        }
        const char next = s[i + 1];
        switch (next) {
        case '\\':
        case ',':
        case ';':
            out.push_back(next);
            ++i;
            break;
        case 'n':
        case 'N':
            out.push_back('\n');
            ++i;
            break;
        default:
            out.push_back('\\');
            break;
        }
    }
    return out;
}

} // namespace

std::vector<aid::Contact> DcVCardParser::parse(std::string_view xmlMultistatus) {
    if (xmlMultistatus.empty()) {
        return {};
    }

    // DoS pre-screen, not the primary XXE barrier. Real CardDAV
    // multistatus bodies never carry a DOCTYPE; refusing inputs that
    // contain `<!DOCTYPE` cuts off three concrete-but-distinct hazards
    // before libxml2 sees the buffer:
    //   - billion-laughs nested-entity bombs (regression: without
    //     this, libxml2 v2.13.5 segfaulted on the corresponding
    //     test).
    //   - parameter-entity (`<!ENTITY % …>`) constructs that some
    //     libxml2 paths still process even with NOENT off.
    //   - cosmetic noise from "lowercase doctype" style probes — the
    //     literal `<!DOCTYPE` is case-sensitive per XML spec, so a
    //     well-formed exploit must use this exact spelling.
    // The actual XXE-exfiltration barrier is the two layers below
    // (NONET + no-DTDLOAD + no-NOENT + no-op external-entity loader).
    // This pre-screen catches "DoS by malformed-but-still-parsed-by-
    // libxml2" cases that the parse options alone don't.
    if (xmlMultistatus.find("<!DOCTYPE") != std::string_view::npos) {
        return {};
    }

    initHardenedXml();

    // xmlReadMemory takes int sizes; clamp to be safe (the multistatus
    // body comes from CardDAV, which won't be petabytes, but better a
    // bounded conversion than a UB sign-conversion warning).
    if (xmlMultistatus.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    XmlDocPtr doc{xmlReadMemory(xmlMultistatus.data(), static_cast<int>(xmlMultistatus.size()),
                                /*URL=*/"davical://multistatus", /*encoding=*/nullptr,
                                kHardenedParseOptions)};
    if (!doc) {
        return {};
    }
    const xmlNode* root = xmlDocGetRootElement(doc.get());
    if (root == nullptr) {
        return {};
    }

    std::vector<std::string> blobs;
    collectTextOf(root, "address-data", blobs);

    std::vector<aid::Contact> out;
    out.reserve(blobs.size());
    for (const auto& v : blobs) {
        if (auto c = parseOneVCard(v)) {
            out.push_back(std::move(*c));
        }
    }
    return out;
}

std::optional<aid::Contact> DcVCardParser::parseOneVCard(std::string_view vCardText) {
    if (vCardText.empty()) {
        return std::nullopt;
    }
    const auto lines = unfoldLines(vCardText);
    if (lines.empty()) {
        return std::nullopt;
    }

    aid::Contact c;
    bool seenFn = false;
    for (const auto& line : lines) {
        auto parsed = splitLine(line);
        if (!parsed) {
            continue;
        }
        // `raw` keeps the vCard escapes; `value` has them resolved. Exactly
        // one unescape pass per value: X-CUSTOM1 hands the RAW text to
        // splitProjectIds (which unescapes itself, so it stays correct when
        // called directly), everything else consumes the resolved text.
        const auto raw = std::string{trim(parsed->value)};
        const auto value = unescapeText(raw);
        if (parsed->nameUpper == "FN") {
            c.name = value;
            seenFn = true;
        } else if (parsed->nameUpper == "ORG") {
            c.companyName = value;
        } else if (parsed->nameUpper == "TEL") {
            if (!value.empty()) {
                c.phoneNumbers.push_back(aid::PhoneNumber{value});
            }
        } else if (parsed->nameUpper == "X-CUSTOM1") {
            for (auto id : splitProjectIds(raw)) {
                c.projectIds.push_back(std::move(id));
            }
        }
    }

    // RFC 6350: FN is REQUIRED. Without it, the contact has no display
    // name and we treat it as malformed (graceful nullopt, not throw).
    if (!seenFn) {
        return std::nullopt;
    }
    // DaviCalAdapter stamps Contact.kind (Person or Company) based on
    // which CardDAV book the parse came from; leave the default here.
    return c;
}

std::vector<aid::ProjectId> DcVCardParser::splitProjectIds(std::string_view xCustom1) {
    // Resolve vCard escapes FIRST, then split on every comma. That order
    // makes an escaped comma a SEPARATOR rather than a literal inside one
    // id: strict RFC 6350 would read `7\, 8` as the single value "7, 8",
    // which can never name an OpenProject project, whereas the operator
    // plainly meant projects 7 and 8. Project ids never contain a comma,
    // so nothing is lost by taking intent over the letter here.
    const std::string resolved = unescapeText(xCustom1);
    const std::string_view text{resolved};

    std::vector<aid::ProjectId> out;
    std::size_t i = 0;
    while (i <= text.size()) {
        const auto comma = text.find(',', i);
        const auto end = (comma == std::string_view::npos) ? text.size() : comma;
        auto piece = trim(text.substr(i, end - i));
        if (!piece.empty()) {
            out.push_back(aid::ProjectId{std::string{piece}});
        }
        if (comma == std::string_view::npos) {
            break;
        }
        i = comma + 1;
    }
    return out;
}

} // namespace aid::adapters::davical::internal
