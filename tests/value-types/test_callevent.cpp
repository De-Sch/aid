#include <gtest/gtest.h>

#include "aid/value-types/CallEvent.h"

namespace {

// Compile-time detectors used to pin wire-format quirks from calls.py.
template <class T>
concept HasDialed = requires(T t) {
    t.dialed;
};

template <class T>
concept HasUser = requires(T t) {
    t.user;
};

template <class T>
concept HasNewUser = requires(T t) {
    t.newUser;
};

} // namespace

TEST(CallEvent, IncomingCallShapeAndName) {
    aid::CallEvent e = aid::IncomingCall{
        .callid = aid::CallId{"1"},
        .remote = aid::PhoneNumber{"+491"},
        .dialed = aid::PhoneNumber{"+490"},
    };
    EXPECT_EQ(aid::eventName(e), "Incoming Call");
    EXPECT_EQ(aid::callidOf(e).v, "1");
}

TEST(CallEvent, OutgoingCallHasNoDialedField) {
    // calls.py:40 emits Outgoing without a `dialed` field. Pinned here so a
    // future drift attempt breaks the build.
    static_assert(!HasDialed<aid::OutgoingCall>,
                  "OutgoingCall must not have a dialed field (calls.py:40)");
    static_assert(HasUser<aid::OutgoingCall>, "OutgoingCall must have a user field (calls.py:40)");
}

TEST(CallEvent, OutgoingCallShapeAndName) {
    aid::CallEvent e = aid::OutgoingCall{
        .callid = aid::CallId{"2"},
        .remote = aid::PhoneNumber{"+491"},
        .user = aid::UserHandle{"alice"},
    };
    EXPECT_EQ(aid::eventName(e), "Outgoing Call");
    EXPECT_EQ(aid::callidOf(e).v, "2");
}

TEST(CallEvent, AcceptedCallUserIsOptionalAbsent) {
    // calls.py:34 omits `user` when ConnectedLineName == "<unknown>".
    aid::AcceptedCall ac{
        .callid = aid::CallId{"3"},
        .remote = aid::PhoneNumber{"+491"},
        .dialed = aid::PhoneNumber{"+490"},
        .user = std::nullopt,
    };
    EXPECT_FALSE(ac.user.has_value());

    aid::CallEvent e = ac;
    EXPECT_EQ(aid::eventName(e), "Accepted Call");
    EXPECT_EQ(aid::callidOf(e).v, "3");
}

TEST(CallEvent, AcceptedCallUserIsOptionalPresent) {
    aid::CallEvent e = aid::AcceptedCall{
        .callid = aid::CallId{"4"},
        .remote = aid::PhoneNumber{"+491"},
        .dialed = aid::PhoneNumber{"+490"},
        .user = aid::UserHandle{"alice"},
    };
    EXPECT_EQ(aid::eventName(e), "Accepted Call");
    EXPECT_EQ(aid::callidOf(e).v, "4");
}

TEST(CallEvent, TransferCallUsesNewUserNotUser) {
    // calls.py:43 emits `newuser` (not `user`). Both directions pinned.
    static_assert(!HasUser<aid::TransferCall>,
                  "TransferCall must not have a user field (calls.py:43)");
    static_assert(HasNewUser<aid::TransferCall>,
                  "TransferCall must have a newUser field (calls.py:43)");
}

TEST(CallEvent, TransferCallShapeAndName) {
    aid::CallEvent e = aid::TransferCall{
        .callid = aid::CallId{"5"},
        .newUser = aid::UserHandle{"bob"},
    };
    EXPECT_EQ(aid::eventName(e), "Transfer Call");
    EXPECT_EQ(aid::callidOf(e).v, "5");
}

TEST(CallEvent, HangupShapeAndName) {
    aid::CallEvent e = aid::HangupCall{
        .callid = aid::CallId{"6"},
        .remote = aid::PhoneNumber{"+491"},
    };
    EXPECT_EQ(aid::eventName(e), "Hangup");
    EXPECT_EQ(aid::callidOf(e).v, "6");
}

TEST(CallEvent, DefaultConstructedIsIncomingCall) {
    aid::CallEvent e;
    EXPECT_EQ(aid::eventName(e), "Incoming Call");
    EXPECT_TRUE(aid::callidOf(e).empty());
}

TEST(CallEvent, CallidOfRoutesPerVariant) {
    aid::IncomingCall inc;
    inc.callid = aid::CallId{"i"};
    aid::OutgoingCall out;
    out.callid = aid::CallId{"o"};
    aid::AcceptedCall acc;
    acc.callid = aid::CallId{"a"};
    aid::TransferCall tra;
    tra.callid = aid::CallId{"t"};
    aid::HangupCall han;
    han.callid = aid::CallId{"h"};

    EXPECT_EQ(aid::callidOf(aid::CallEvent{inc}).v, "i");
    EXPECT_EQ(aid::callidOf(aid::CallEvent{out}).v, "o");
    EXPECT_EQ(aid::callidOf(aid::CallEvent{acc}).v, "a");
    EXPECT_EQ(aid::callidOf(aid::CallEvent{tra}).v, "t");
    EXPECT_EQ(aid::callidOf(aid::CallEvent{han}).v, "h");
}
