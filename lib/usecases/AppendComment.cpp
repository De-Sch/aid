#include "aid/usecases/AppendComment.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aid/plumbing/ActionResult.h"
#include "aid/plumbing/Error.h"
#include "aid/ports/TicketStore.h"
#include "aid/ports/UiNotifier.h"
#include "aid/usecases/TicketDeltaEmitter.h"
#include "aid/value-types/Ticket.h"

namespace aid::usecases {

using aid::plumbing::ActionResult;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;

namespace {

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t b = 0;
    while (b < s.size() && isSpace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

} // namespace

AppendComment::AppendComment(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui)
    : ts_(ts), ui_(ui) {
}

Task<Result<ActionResult>> AppendComment::run(aid::TicketId id, std::string text,
                                              aid::UserHandle viewer) {
    // `text` is taken by value so the coroutine frame owns the backing
    // storage: `trimmed` is a view into it and is read after the first
    // co_await.
    const auto trimmed = trim(text);
    if (trimmed.empty()) {
        co_return ActionResult{false, "COMMENT_SAVE", id, std::string{"empty comment"}};
    }

    auto fetched = co_await ts_.fetchById(id);
    if (!fetched.has_value()) {
        if (fetched.error().code == ErrorCode::NotFound) {
            co_return ActionResult{false, "COMMENT_SAVE", id, std::string{"ticket not found"}};
        }
        co_return aid::plumbing::unexpected{fetched.error()};
    }

    // Append the comment as a pure delta on the fresh ticket inside save()
    // (re-applied on every 409), so a concurrent same-ticket comment/edit is not
    // clobbered — two operators commenting at once must both
    // survive in `description`.
    // The reducer is hoisted to a named local to dodge gcc-12's coroutine
    // frame-lifetime bug (a temporary in the co_await operand is double-destroyed).
    const std::string comment{trimmed};
    const aid::ports::TicketReducer reducer = [comment](aid::Ticket t) {
        t.description += "\n";
        t.description += comment;
        return t;
    };
    auto saved = co_await ts_.save(id, reducer);
    if (!saved.has_value()) {
        co_return aid::plumbing::unexpected{saved.error()};
    }

    ActionResult ar{true, "COMMENT_SAVE", id, std::nullopt};
    ui_.notifyActionResult(viewer, ar);
    // Live delta from the post-save ticket (now carries the new comment).
    TicketDeltaEmitter emitter{ts_, ui_};
    (void)co_await emitter.emitTicketDelta(std::move(*saved));
    co_return ar;
}

} // namespace aid::usecases
