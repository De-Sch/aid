#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "aid/auth/PasswordHasher.h"
#include "aid/plumbing/Error.h"

using aid::auth::PasswordHasher;
using aid::plumbing::ErrorCode;

TEST(PasswordHasher, InitializeIsIdempotent) {
    // Calling twice must not abort. The body of initialize() guards on
    // std::once_flag so this is a contract test, not a stress test.
    PasswordHasher::initialize();
    PasswordHasher::initialize();
    SUCCEED();
}

TEST(PasswordHasher, HashAcceptsValidPasswordRoundTrip) {
    auto h = PasswordHasher::hash("hunter2");
    ASSERT_TRUE(h.has_value()) << h.error().message;
    EXPECT_FALSE(h->empty());
    EXPECT_TRUE(PasswordHasher::verify("hunter2", *h));
}

TEST(PasswordHasher, VerifyRejectsWrongPassword) {
    auto h = PasswordHasher::hash("hunter2");
    ASSERT_TRUE(h.has_value()) << h.error().message;
    EXPECT_FALSE(PasswordHasher::verify("hunter1", *h));
}

TEST(PasswordHasher, HashProducesDifferentOutputForSameInput) {
    auto a = PasswordHasher::hash("same-input");
    auto b = PasswordHasher::hash("same-input");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_NE(*a, *b) << "salt should randomize encoded hash";
}

TEST(PasswordHasher, HashRejectsEmptyPlaintext) {
    auto r = PasswordHasher::hash("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("empty"), std::string::npos);
}

TEST(PasswordHasher, VerifyRejectsEmptyStoredHash) {
    EXPECT_FALSE(PasswordHasher::verify("anything", ""));
}

TEST(PasswordHasher, VerifyRejectsMalformedStoredHash) {
    EXPECT_FALSE(PasswordHasher::verify("anything", "not-a-real-hash"));
}

TEST(PasswordHasher, DummyHashIsStable) {
    const auto& a = PasswordHasher::dummyHash();
    const auto& b = PasswordHasher::dummyHash();
    EXPECT_EQ(&a, &b) << "dummyHash should return the same static reference";
}

TEST(PasswordHasher, DummyHashRejectsArbitraryPasswords) {
    // The dummy hash is not a magic accept-all. Any user-supplied
    // password verifies false against it (with the astronomically small
    // exception of guessing the dummy plaintext, which is fine — it's
    // not a real account).
    const auto& dummy = PasswordHasher::dummyHash();
    EXPECT_FALSE(PasswordHasher::verify("password", dummy));
    EXPECT_FALSE(PasswordHasher::verify("hunter2", dummy));
}

TEST(PasswordHasher, NeedsRehashReturnsFalseForCurrentParams) {
    auto h = PasswordHasher::hash("p");
    ASSERT_TRUE(h.has_value());
    EXPECT_FALSE(PasswordHasher::needsRehash(*h));
}

TEST(PasswordHasher, NeedsRehashReturnsTrueForCorruptedHash) {
    EXPECT_TRUE(PasswordHasher::needsRehash(""));
    EXPECT_TRUE(PasswordHasher::needsRehash("not-a-hash"));
}
