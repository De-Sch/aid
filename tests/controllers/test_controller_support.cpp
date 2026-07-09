#include <drogon/HttpTypes.h>
#include <gtest/gtest.h>

#include "aid/controllers/ControllerSupport.h"
#include "aid/plumbing/Error.h"

namespace {

using aid::controllers::httpStatusForError;
using aid::plumbing::ErrorCode;

// The canonical domain-error -> HTTP-status table. If a new ErrorCode is added,
// httpStatusForError won't compile (no default in its switch) — add the code
// here and there together.
TEST(HttpStatusForError, MapsEveryCodeToItsStatus) {
    EXPECT_EQ(httpStatusForError(ErrorCode::InvalidInput), drogon::k400BadRequest);
    EXPECT_EQ(httpStatusForError(ErrorCode::NotFound), drogon::k404NotFound);
    EXPECT_EQ(httpStatusForError(ErrorCode::Conflict), drogon::k409Conflict);
    EXPECT_EQ(httpStatusForError(ErrorCode::Conflict409), drogon::k409Conflict);
    EXPECT_EQ(httpStatusForError(ErrorCode::LockVersionExhausted), drogon::k409Conflict);
    EXPECT_EQ(httpStatusForError(ErrorCode::Unprocessable422), drogon::k422UnprocessableEntity);
    EXPECT_EQ(httpStatusForError(ErrorCode::Unauthenticated), drogon::k401Unauthorized);
    EXPECT_EQ(httpStatusForError(ErrorCode::Forbidden), drogon::k403Forbidden);
    EXPECT_EQ(httpStatusForError(ErrorCode::TooManyRequests), drogon::k429TooManyRequests);
    EXPECT_EQ(httpStatusForError(ErrorCode::UpstreamUnavailable), drogon::k502BadGateway);
    EXPECT_EQ(httpStatusForError(ErrorCode::UpstreamTimeout), drogon::k504GatewayTimeout);
    EXPECT_EQ(httpStatusForError(ErrorCode::WalWriteFailed), drogon::k500InternalServerError);
    EXPECT_EQ(httpStatusForError(ErrorCode::WalSyncFailed), drogon::k500InternalServerError);
    EXPECT_EQ(httpStatusForError(ErrorCode::PluginAbiMismatch), drogon::k500InternalServerError);
    EXPECT_EQ(httpStatusForError(ErrorCode::InvariantViolation), drogon::k500InternalServerError);
    EXPECT_EQ(httpStatusForError(ErrorCode::Unknown), drogon::k500InternalServerError);
}

} // namespace
