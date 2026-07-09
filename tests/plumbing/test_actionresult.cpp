#include <gtest/gtest.h>

#include "aid/plumbing/ActionResult.h"

using aid::plumbing::ActionResult;

TEST(ActionResult, SuccessShape) {
    ActionResult r{true, "COMMENT_SAVE", aid::TicketId{"4711"}, std::nullopt};
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("COMMENT_SAVE", r.op);
    EXPECT_EQ("4711", r.ticketId.v);
    EXPECT_FALSE(r.message.has_value());
}

TEST(ActionResult, FailureShape) {
    ActionResult r{false, "TICKET_CLOSE", aid::TicketId{"4712"},
                   std::string{"lockVersion exhausted"}};
    EXPECT_FALSE(r.ok);
    EXPECT_EQ("TICKET_CLOSE", r.op);
    EXPECT_EQ("4712", r.ticketId.v);
    ASSERT_TRUE(r.message.has_value());
    EXPECT_EQ("lockVersion exhausted", *r.message);
}
