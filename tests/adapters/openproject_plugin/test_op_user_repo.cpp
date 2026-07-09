#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "aid/adapters/openproject/internal/OpHttp.h"
#include "aid/adapters/openproject/internal/OpUserRepo.h"
#include "tests/adapters/openproject_plugin/fake_http_dispatcher.h"

using aid::adapters::openproject::OpHttp;
using aid::adapters::openproject::OpUserRepo;
using aid::test_support::FakeHttpDispatcher;
using aid::test_support::FakeSleeper;
using json = nlohmann::json;

namespace {

template <class T> T drainSync(aid::plumbing::Task<T>&& task) {
    EXPECT_TRUE(task.done());
    return task.await_resume();
}

// Representative HAL response: GET /api/v3/users?filters=… returns a
// collection with _embedded.elements containing matching users.
std::string usersCollectionWith(const std::string& login, int id) {
    json j;
    j["_embedded"]["elements"] = json::array();
    json one;
    one["login"] = login;
    one["id"] = id;
    one["_links"]["self"]["href"] = "/api/v3/users/" + std::to_string(id);
    j["_embedded"]["elements"].push_back(std::move(one));
    return j.dump();
}

std::string emptyCollection() {
    return R"({"_embedded":{"elements":[]}})";
}

} // namespace

TEST(OpUserRepo, ResolveLoginHitReturnsHandle) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, usersCollectionWith("alice", 9));
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto result = drainSync(users.resolveLogin("alice"));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->v, "alice");
}

TEST(OpUserRepo, ResolveLoginMissReturnsNullopt) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, emptyCollection());
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto result = drainSync(users.resolveLogin("ghost"));
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->has_value());
}

TEST(OpUserRepo, ResolveLoginQueryUsesFilterUrlEncoding) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, usersCollectionWith("alice", 9));
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    (void)drainSync(users.resolveLogin("alice"));
    ASSERT_EQ(d.calls().size(), 1U);
    const auto& path = d.calls()[0].path;
    EXPECT_NE(path.find("/api/v3/users?filters="), std::string::npos);
    // The filter JSON `{"login":{"operator":"=","values":["alice"]}}` must
    // be percent-encoded so quotes don't appear literally. We can't assert
    // the entire encoded blob (the unreserved set is alphanumeric) but the
    // double-quote escape (%22) must show up.
    EXPECT_NE(path.find("%22"), std::string::npos);
}

TEST(OpUserRepo, HrefForCachesAfterResolveLogin) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, usersCollectionWith("alice", 9));
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    (void)drainSync(users.resolveLogin("alice")); // primes cache

    // hrefFor must not issue a second HTTP request thanks to the cache
    // warmed in resolveLogin.
    const std::size_t before = d.calls().size();
    auto href = drainSync(users.hrefFor(aid::UserHandle{"alice"}));
    ASSERT_TRUE(href.has_value());
    EXPECT_EQ(*href, "/api/v3/users/9");
    EXPECT_EQ(d.calls().size(), before); // no new call
}

TEST(OpUserRepo, HrefForMissBubblesNotFound) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, emptyCollection());
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto href = drainSync(users.hrefFor(aid::UserHandle{"ghost"}));
    ASSERT_FALSE(href.has_value());
    EXPECT_EQ(href.error().code, aid::plumbing::ErrorCode::NotFound);
}

TEST(OpUserRepo, ProjectsForUserReturnsIdsFromTopLevelField) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    // First call: GET /users?login=... → href.
    d.enqueueResponse(200, usersCollectionWith("alice", 9));
    // Second call: GET /projects?principal=... → projects collection.
    json projects;
    projects["_embedded"]["elements"] = json::array();
    json p1, p2;
    p1["id"] = 11;
    p1["_links"]["self"]["href"] = "/api/v3/projects/11";
    p2["id"] = 12;
    p2["_links"]["self"]["href"] = "/api/v3/projects/12";
    projects["_embedded"]["elements"].push_back(std::move(p1));
    projects["_embedded"]["elements"].push_back(std::move(p2));
    d.enqueueResponse(200, projects.dump());

    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto ids = drainSync(users.projectsForUser(aid::UserHandle{"alice"}));
    ASSERT_TRUE(ids.has_value()) << ids.error().message;
    ASSERT_EQ(ids->size(), 2U);
    EXPECT_EQ((*ids)[0].v, "11");
    EXPECT_EQ((*ids)[1].v, "12");

    // The principal filter must carry the NUMERIC user id ("9"), not the
    // "/api/v3/users/9" href — OpenProject 400s on the href form.
    ASSERT_GE(d.calls().size(), 2U);
    const auto& projPath = d.calls()[1].path;
    EXPECT_NE(projPath.find("principal"), std::string::npos);
    EXPECT_NE(projPath.find('9'), std::string::npos);
    EXPECT_EQ(projPath.find("users"), std::string::npos)
        << "principal filter must use the numeric id, not the user href";
}

TEST(OpUserRepo, ProjectsForUserEmptyCollectionReturnsEmptyVector) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, usersCollectionWith("alice", 9));
    d.enqueueResponse(200, emptyCollection());

    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto ids = drainSync(users.projectsForUser(aid::UserHandle{"alice"}));
    ASSERT_TRUE(ids.has_value());
    EXPECT_TRUE(ids->empty());
}

// ─── projectMembers ──────────────────────────────────────────────────────

namespace {

// A memberships collection (GET /api/v3/memberships?filters=…): each element
// names its principal by href ONLY — OpenProject does not embed the login here,
// so projectMembers must resolve each user principal in a follow-up GET.
std::string membershipsCollectionWith(const std::vector<std::string>& principalHrefs) {
    json j;
    j["_embedded"]["elements"] = json::array();
    for (const auto& href : principalHrefs) {
        json one;
        one["_links"]["principal"]["href"] = href;
        j["_embedded"]["elements"].push_back(std::move(one));
    }
    return j.dump();
}

// A single user resource (GET /api/v3/users/<id>) carrying the login.
std::string userResource(const std::string& login, int id) {
    json u;
    u["login"] = login;
    u["id"] = id;
    u["_links"]["self"]["href"] = "/api/v3/users/" + std::to_string(id);
    return u.dump();
}

} // namespace

TEST(OpUserRepo, ProjectMembersQueriesMembershipsAndResolvesLogins) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    // 1) the memberships collection (two user principals), then 2) one GET per
    // principal href to resolve its login. Scripted responses are served FIFO.
    d.enqueueResponse(200, membershipsCollectionWith({"/api/v3/users/9", "/api/v3/users/5"}));
    d.enqueueResponse(200, userResource("alice", 9));
    d.enqueueResponse(200, userResource("bob", 5));
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto members = drainSync(users.projectMembers(aid::ProjectId{"11"}));
    ASSERT_TRUE(members.has_value()) << members.error().message;
    ASSERT_EQ(members->size(), 2U);
    EXPECT_EQ((*members)[0].v, "alice");
    EXPECT_EQ((*members)[1].v, "bob");

    ASSERT_EQ(d.calls().size(), 3U);
    // The collection query must hit the memberships endpoint with an encoded
    // project filter — NOT the non-existent /projects/{id}/members route.
    EXPECT_EQ(d.calls()[0].method, "GET");
    EXPECT_NE(d.calls()[0].path.find("/api/v3/memberships?filters="), std::string::npos);
    EXPECT_EQ(d.calls()[0].path.find("/projects/"), std::string::npos)
        << "must not use the non-existent /api/v3/projects/<id>/members route";
    EXPECT_NE(d.calls()[0].path.find("%22"), std::string::npos); // filter JSON is encoded
    // Then each principal href is fetched verbatim to read its login.
    EXPECT_EQ(d.calls()[1].path, "/api/v3/users/9");
    EXPECT_EQ(d.calls()[2].path, "/api/v3/users/5");
}

TEST(OpUserRepo, ProjectMembersCachesPerProject) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, membershipsCollectionWith({"/api/v3/users/9"}));
    d.enqueueResponse(200, userResource("alice", 9));
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto first = drainSync(users.projectMembers(aid::ProjectId{"11"}));
    ASSERT_TRUE(first.has_value());
    const std::size_t after = d.calls().size();

    // Second lookup of the same project must be served from cache — no new
    // HTTP call (and no further scripted responses are needed).
    auto second = drainSync(users.projectMembers(aid::ProjectId{"11"}));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->size(), 1U);
    EXPECT_EQ(d.calls().size(), after) << "cached project must not round-trip again";
}

TEST(OpUserRepo, ProjectMembersSkipsNonUserPrincipals) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    // A group principal has no login to notify and must NOT trigger a resolve
    // GET; only the user principal is fetched and returned.
    d.enqueueResponse(200, membershipsCollectionWith({"/api/v3/groups/3", "/api/v3/users/9"}));
    d.enqueueResponse(200, userResource("alice", 9));
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto members = drainSync(users.projectMembers(aid::ProjectId{"12"}));
    ASSERT_TRUE(members.has_value()) << members.error().message;
    ASSERT_EQ(members->size(), 1U);
    EXPECT_EQ((*members)[0].v, "alice");
    EXPECT_EQ(d.calls().size(), 2U) << "group principal must not be resolved";
}

TEST(OpUserRepo, ProjectMembersSkipsUserPrincipalWithoutLogin) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    // A user principal whose resource exposes no login (e.g. hidden/locked) is
    // skipped rather than turned into a numeric-id handle (which would never
    // match the session login the WS hub keys on).
    d.enqueueResponse(200, membershipsCollectionWith({"/api/v3/users/9"}));
    d.enqueueResponse(200, R"({"id":9,"_type":"User"})"); // no login field
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto members = drainSync(users.projectMembers(aid::ProjectId{"12"}));
    ASSERT_TRUE(members.has_value()) << members.error().message;
    EXPECT_TRUE(members->empty());
}

TEST(OpUserRepo, ProjectMembersEmptyCollectionReturnsEmptyVector) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    d.enqueueResponse(200, emptyCollection());
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    auto members = drainSync(users.projectMembers(aid::ProjectId{"13"}));
    ASSERT_TRUE(members.has_value());
    EXPECT_TRUE(members->empty());
}

// ─── refreshMembership ─────────────────────────────────────────────────────

namespace {

// A batched memberships collection (GET /api/v3/memberships?filters=[project in
// (…)]) where each element names BOTH its project and its principal by href —
// refreshMembership reads the project href to bucket each principal. `total` is
// the authoritative HAL match count the pagination loop stops on.
std::string
membershipsBatch(const std::vector<std::pair<std::string, std::string>>& projectAndPrincipalHrefs,
                 long long total) {
    json j;
    j["_embedded"]["elements"] = json::array();
    for (const auto& pr : projectAndPrincipalHrefs) {
        json one;
        one["_links"]["project"]["href"] = pr.first;
        one["_links"]["principal"]["href"] = pr.second;
        j["_embedded"]["elements"].push_back(std::move(one));
    }
    j["total"] = total;
    return j.dump();
}

// Prime membersCache_ for project 11 with the given user logins (one membership
// element + one resolve GET per login), so a subsequent refreshMembership has a
// cached baseline to diff against. Returns after the cache is warm.
void primeProject11(FakeHttpDispatcher& d, OpUserRepo& users,
                    const std::vector<std::pair<std::string, int>>& logins) {
    std::vector<std::string> hrefs;
    for (const auto& lg : logins)
        hrefs.push_back("/api/v3/users/" + std::to_string(lg.second));
    d.enqueueResponse(200, membershipsCollectionWith(hrefs));
    for (const auto& lg : logins)
        d.enqueueResponse(200, userResource(lg.first, lg.second));
    auto primed = drainSync(users.projectMembers(aid::ProjectId{"11"}));
    ASSERT_TRUE(primed.has_value()) << primed.error().message;
}

} // namespace

TEST(OpUserRepo, RefreshMembershipNoCacheReturnsEmptyWithoutFetch) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    // Nothing has been resolved yet → nothing to reconcile, and no HTTP call.
    auto deltas = drainSync(users.refreshMembership());
    ASSERT_TRUE(deltas.has_value()) << deltas.error().message;
    EXPECT_TRUE(deltas->empty());
    EXPECT_TRUE(d.calls().empty());
}

TEST(OpUserRepo, RefreshMembershipDetectsAddedMember) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    primeProject11(d, users, {{"alice", 9}});

    // Fresh batch: alice (cached resolve) plus a new member bob.
    d.enqueueResponse(200, membershipsBatch({{"/api/v3/projects/11", "/api/v3/users/9"},
                                             {"/api/v3/projects/11", "/api/v3/users/5"}},
                                            2));
    d.enqueueResponse(200, userResource("bob", 5)); // bob is new → one resolve GET

    auto deltas = drainSync(users.refreshMembership());
    ASSERT_TRUE(deltas.has_value()) << deltas.error().message;
    ASSERT_EQ(deltas->size(), 1U);
    EXPECT_EQ((*deltas)[0].project.v, "11");
    ASSERT_EQ((*deltas)[0].added.size(), 1U);
    EXPECT_EQ((*deltas)[0].added[0].v, "bob");
    EXPECT_TRUE((*deltas)[0].removed.empty());
}

TEST(OpUserRepo, RefreshMembershipDetectsRemovedMember) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    primeProject11(d, users, {{"alice", 9}, {"bob", 5}});

    // Fresh batch: only alice remains (both resolves are cache hits → 0 GETs).
    d.enqueueResponse(200, membershipsBatch({{"/api/v3/projects/11", "/api/v3/users/9"}}, 1));

    auto deltas = drainSync(users.refreshMembership());
    ASSERT_TRUE(deltas.has_value()) << deltas.error().message;
    ASSERT_EQ(deltas->size(), 1U);
    EXPECT_EQ((*deltas)[0].project.v, "11");
    EXPECT_TRUE((*deltas)[0].added.empty());
    ASSERT_EQ((*deltas)[0].removed.size(), 1U);
    EXPECT_EQ((*deltas)[0].removed[0].v, "bob");
}

TEST(OpUserRepo, RefreshMembershipUnchangedProducesNoDelta) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    primeProject11(d, users, {{"alice", 9}});

    // Identical set → no delta. alice resolves from cache, so the only call here
    // is the batched memberships GET.
    d.enqueueResponse(200, membershipsBatch({{"/api/v3/projects/11", "/api/v3/users/9"}}, 1));

    const std::size_t before = d.calls().size();
    auto deltas = drainSync(users.refreshMembership());
    ASSERT_TRUE(deltas.has_value()) << deltas.error().message;
    EXPECT_TRUE(deltas->empty());
    EXPECT_EQ(d.calls().size(), before + 1U) << "warm logins must not re-resolve";
}

TEST(OpUserRepo, RefreshMembershipPaginatesAcrossPages) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    primeProject11(d, users, {{"alice", 9}});

    // total=2 forces a second page: page 1 carries alice (cached), page 2 carries
    // a new member carol — so the page-2 contents must be folded into the diff.
    d.enqueueResponse(200, membershipsBatch({{"/api/v3/projects/11", "/api/v3/users/9"}}, 2));
    d.enqueueResponse(200, membershipsBatch({{"/api/v3/projects/11", "/api/v3/users/7"}}, 2));
    d.enqueueResponse(200, userResource("carol", 7)); // carol is new → resolve GET

    const std::size_t before = d.calls().size();
    auto deltas = drainSync(users.refreshMembership());
    ASSERT_TRUE(deltas.has_value()) << deltas.error().message;
    ASSERT_EQ(deltas->size(), 1U);
    ASSERT_EQ((*deltas)[0].added.size(), 1U);
    EXPECT_EQ((*deltas)[0].added[0].v, "carol");

    // Two batched memberships GETs (offset=1 and offset=2) plus carol's resolve.
    ASSERT_GE(d.calls().size(), before + 2U);
    EXPECT_NE(d.calls()[before].path.find("offset=1"), std::string::npos);
    EXPECT_NE(d.calls()[before + 1].path.find("offset=2"), std::string::npos);
}

TEST(OpUserRepo, RefreshMembershipSkipsGroupPrincipal) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    primeProject11(d, users, {{"alice", 9}});

    // The fresh batch adds a GROUP principal alongside alice. A group carries no
    // login to notify, so it must be skipped — neither resolved nor added.
    d.enqueueResponse(200, membershipsBatch({{"/api/v3/projects/11", "/api/v3/users/9"},
                                             {"/api/v3/projects/11", "/api/v3/groups/3"}},
                                            2));

    auto deltas = drainSync(users.refreshMembership());
    ASSERT_TRUE(deltas.has_value()) << deltas.error().message;
    EXPECT_TRUE(deltas->empty()) << "a group principal must not register as an added member";
    for (const auto& c : d.calls())
        EXPECT_EQ(c.path.find("/api/v3/groups/3"), std::string::npos)
            << "group principal must not be resolved";
}

TEST(OpUserRepo, RefreshMembershipFetchErrorIsTreatedAsNoChange) {
    FakeHttpDispatcher d;
    FakeSleeper s;
    OpHttp http(d, "http://op.example.com", "t", s.sleeper());
    OpUserRepo users(http);

    primeProject11(d, users, {{"alice", 9}});

    // The batched fetch fails. The SAFETY GUARD must turn this into "no change":
    // success result, no delta, and the cache left intact.
    d.enqueueError(aid::plumbing::ErrorCode::UpstreamUnavailable, "boom");

    auto deltas = drainSync(users.refreshMembership());
    ASSERT_TRUE(deltas.has_value()) << "a fetch error must not surface as an error";
    EXPECT_TRUE(deltas->empty()) << "a fetch error must never be read as a removal";

    // Cache preserved: re-reading project 11 is served from cache with no new
    // HTTP call — the failed refresh must not have evicted alice.
    const std::size_t after = d.calls().size();
    auto members = drainSync(users.projectMembers(aid::ProjectId{"11"}));
    ASSERT_TRUE(members.has_value());
    ASSERT_EQ(members->size(), 1U);
    EXPECT_EQ((*members)[0].v, "alice");
    EXPECT_EQ(d.calls().size(), after) << "cache must survive a failed refresh";
}
