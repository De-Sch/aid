#include <gtest/gtest.h>

#include <string>

#include "aid/plumbing/Result.h"

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::errorCodeName;
using aid::plumbing::Result;
using aid::plumbing::unexpected;

TEST(Result, ValueConstructionReportsHasValue) {
    Result<int> r = 42;
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(42, *r);
    EXPECT_EQ(42, r.value());
}

TEST(Result, ErrorConstructionReportsNoValue) {
    Result<int> r = unexpected<Error>{Error{ErrorCode::NotFound, "ticket 4711", std::nullopt}};
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(ErrorCode::NotFound, r.error().code);
    EXPECT_EQ("ticket 4711", r.error().message);
    EXPECT_FALSE(r.error().correlationId.has_value());
}

TEST(Result, CorrelationIdRoundTrips) {
    Result<int> r =
        unexpected<Error>{Error{ErrorCode::Conflict409, "lockVersion", std::string{"cid-abc-123"}}};
    ASSERT_FALSE(r.has_value());
    ASSERT_TRUE(r.error().correlationId.has_value());
    EXPECT_EQ("cid-abc-123", *r.error().correlationId);
}

TEST(Result, VoidSpecializationSuccess) {
    Result<void> r;
    EXPECT_TRUE(r.has_value());
}

TEST(Result, VoidSpecializationFailure) {
    Result<void> r =
        unexpected<Error>{Error{ErrorCode::UpstreamTimeout, "openproject", std::nullopt}};
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(ErrorCode::UpstreamTimeout, r.error().code);
}

TEST(Result, BoolConversionMatchesHasValue) {
    Result<int> ok = 1;
    Result<int> err = unexpected<Error>{Error{ErrorCode::Unknown, "x", std::nullopt}};
    EXPECT_TRUE(static_cast<bool>(ok));
    EXPECT_FALSE(static_cast<bool>(err));
}

TEST(Error, AllErrorCodesHaveNonEmptyName) {
    constexpr ErrorCode all[] = {
        ErrorCode::InvalidInput,
        ErrorCode::NotFound,
        ErrorCode::Conflict409,
        ErrorCode::LockVersionExhausted,
        ErrorCode::UpstreamUnavailable,
        ErrorCode::UpstreamTimeout,
        ErrorCode::Unauthenticated,
        ErrorCode::Forbidden,
        ErrorCode::WalWriteFailed,
        ErrorCode::WalSyncFailed,
        ErrorCode::PluginAbiMismatch,
        ErrorCode::InvariantViolation,
        ErrorCode::Unknown,
    };
    for (ErrorCode c : all) {
        EXPECT_FALSE(errorCodeName(c).empty());
    }
}

TEST(Error, ErrorCodeNameIsStableForKnownValues) {
    EXPECT_EQ(std::string_view{"NotFound"}, errorCodeName(ErrorCode::NotFound));
    EXPECT_EQ(std::string_view{"Conflict409"}, errorCodeName(ErrorCode::Conflict409));
    EXPECT_EQ(std::string_view{"LockVersionExhausted"},
              errorCodeName(ErrorCode::LockVersionExhausted));
}
