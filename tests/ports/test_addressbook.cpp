#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/AddressBook.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"

namespace {

using aid::Contact;
using aid::PhoneNumber;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::ports::AddressBook;

static_assert(std::is_abstract_v<AddressBook>);
static_assert(std::has_virtual_destructor_v<AddressBook>);
static_assert(!std::is_copy_constructible_v<AddressBook>);
static_assert(!std::is_move_constructible_v<AddressBook>);

class StubAddressBook final : public AddressBook {
public:
    PhoneNumber canonicalize(PhoneNumber raw) const noexcept override {
        if (raw.v == "0301234567") {
            return PhoneNumber{"+49301234567"};
        }
        return PhoneNumber{};
    }
    Task<Result<std::optional<Contact>>> lookup(PhoneNumber) override {
        co_return Result<std::optional<Contact>>{std::nullopt};
    }
    Task<Result<void>> ping() override { co_return Result<void>{}; }
};

TEST(AddressBookPort, CanonicalizeRunsThroughBasePointer) {
    std::unique_ptr<AddressBook> book = std::make_unique<StubAddressBook>();
    EXPECT_EQ(book->canonicalize(PhoneNumber{"0301234567"}).v, "+49301234567");
    EXPECT_TRUE(book->canonicalize(PhoneNumber{"<unknown>"}).empty());
}

TEST(AddressBookPort, LookupUnknownReturnsNullopt) {
    std::unique_ptr<AddressBook> book = std::make_unique<StubAddressBook>();
    auto task = book->lookup(PhoneNumber{"+49301234567"});
    ASSERT_TRUE(task.done());
    auto result = task.await_resume();
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().has_value());
}

} // namespace
