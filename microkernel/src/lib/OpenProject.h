// OpenProject API integration header
// Provides classes and structures for interfacing with OpenProject ticket
// system Handles work package creation, updates, and queries for call
// management
#pragma once
#include <curlpp/Easy.hpp>
#include <map>
#include <set>
#include <string>

#include "ConfigError.h"
#include "DaviCal.h"
#include "Models/Ticket.h"
#include "Systems/TicketSystem.h"
#include "json_fwd.hpp"

struct OpenProjectProject;
// API class for OpenProject project management operations
class OpenProjectProjectApi {
  void prepareRequest(curlpp::Easy &request);
  void prepareJson(curlpp::Easy &request, nlohmann::json &json);

public:
  OpenProjectProjectApi();
  ~OpenProjectProjectApi();

  nlohmann::json getProjects();
  nlohmann::json getProjectById(int id);
  nlohmann::json postProject(nlohmann::json &json);
  void patchProject(OpenProjectProject &op);
};

struct DoliProject;
// Description structure for project metadata
struct Description {
  std::string raw = "";
  NLOHMANN_DEFINE_TYPE_INTRUSIVE(Description, raw);
};
// OpenProject project data structure with API capabilities
struct OpenProjectProject : public OpenProjectProjectApi {

  string id;
  ////
  string callId;
  string caller;
  string user;
  ////
  int customField1;          // doliId
  float customField2 = 0.0F; // budget
  string identifier;
  string name;
  bool active = false;
  Description description;
  nlohmann::json statusExplanation;
  string createdAt; // "%Y-%m-%d %H:%M:%S"
  string updatedAt;

  OpenProjectProject();
  ~OpenProjectProject();

  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(OpenProjectProject, id,
                                              customField2, identifier, name,
                                              active, description,
                                              statusExplanation, createdAt,
                                              updatedAt);

  void to_json(nlohmann::json &j, const OpenProjectProject &p);
  DoliProject openProjectToDoliProject(OpenProjectProject &op);
  void postDoliIdToOpenProject(nlohmann::json &);
  // doliResponseFromPost, OpenProjectProject& op);
  OpenProjectProject doliProjectToOpenProject(DoliProject &doli);

  // 	nlohmann::json openProjectToDoliProject(OpenProjectProject project)
  // 	{
  // 		DoliProject re;
  // 		return
  // 	};
};

struct OpenProjectWorkPackage;
// Main API class for OpenProject work package (ticket) management
// Handles ticket creation, updates, queries, and call integration
// Implements TicketSystem interface for call management workflow
class OpenProjectWorkPackageApi : public TicketSystem {
  void prepareRequest(curlpp::Easy &request) const;
  nlohmann::json prepareJson(curlpp::Easy &request);
  string encodeQuery(const string &query) const;
  bool addressToNewTicket(DaviCal &card);

  // Helper methods for closeTicket() - handle OpenProject's two-step status
  // transition requirement
  bool needsIntermediateTransition(const string &currentStatus,
                                   const string &targetStatus) const;
  string determineIntermediateStatus(const string &currentStatus,
                                     const string &targetStatus) const;
  bool executeStatusTransition(const string &ticketId, const string &statusHref,
                               int lockVersion);
  bool updateTicketLockVersion(OpenProjectWorkPackage &ticket,
                               const nlohmann::json &response);

  // Helper methods for getDashboardInformation() - handle dashboard data
  // aggregation
  string getUserIdFromPayload(istream &payload, const string &urlParams,
                              int &userId) const;
  set<string> getUserProjectIds(int userId) const;
  nlohmann::json getCallTicketsForProjects() const;
  nlohmann::json getAssignedCallTickets(int userId) const;
  map<int, nlohmann::json>
  mergeAndDeduplicateTickets(const nlohmann::json &projectTickets,
                             const nlohmann::json &assignedTickets,
                             const set<string> &userProjects) const;
  nlohmann::json
  buildTicketJson(const map<int, nlohmann::json> &uniqueTickets) const;
  nlohmann::json detectActiveCall(const nlohmann::json &result,
                                  const string &userName) const;
  void sortTicketsByStatus(nlohmann::json &result) const;

  // Helper methods for getTicketByPhoneNumber() - handle ID vs phone number
  // confusion workaround
  Ticket *tryGetTicketById(const string &possibleId);
  Ticket *searchTicketsByPhoneNumber(const string &phoneNumber);

  // Helper methods for getTicketById() - simplify direct ticket retrieval
  nlohmann::json fetchWorkPackageById(const string &id);
  nlohmann::json wrapSingleTicketResponse(const nlohmann::json &ticketJson);

  // Helper methods for getLatestTicketInProjectByName() - simplify ticket
  // search by name
  string buildTicketSearchByNameFilter(const string &projectId,
                                       const string &ticketName) const;
  nlohmann::json executeTicketSearchQuery(const string &filterQuery,
                                          const string &sortByQuery) const;

  // Helper methods for getLatestCallTicketInProject() - simplify Call ticket
  // search
  string buildCallTicketSearchFilter(const string &projectId) const;

  template <typename T>
  T getConfigValue(nlohmann::json &config, const char *param,
                   const T &defaultVal, bool &hasError);

public:
  // Config data

  OpenProjectWorkPackageApi();
  OpenProjectWorkPackageApi(nlohmann::json &config);
  ~OpenProjectWorkPackageApi() override;

  string configTicketSystemName;
  string configTypeCall;
  string configStatusRejected;
  string configStatusTested;

  nlohmann::json getWorkPackage();
  nlohmann::json getCallWorkPackagesByStatus(int statusFromConfig);
  nlohmann::json getRunningWorkPackagesByPhoneNumber(string phoneNumber);
  nlohmann::json postWorkPackage(nlohmann::json json, string id);
  void patchWorkPackage(OpenProjectWorkPackage &pkg, const string &id);

  Ticket *getTicketByCallId(string &callId) override;
  Ticket *getTicketByCallIdContains(string &callId) override;
  Ticket *getTicketById(string &id) override;
  Ticket *getTicketByPhoneNumber(string &phoneNumber) override;
  Ticket *getLatestCallTicketInProject(const string &projectId) const override;
  Ticket *
  getLatestTicketInProjectByName(const string &projectId,
                                 const string &ticketName) const override;
  nlohmann::json getTicketsByAssignee(int id);
  string getUser(const Call &call);
  string getCurrentTickets() override;
  int getUserId(const string &name) const;
  int getUserIdWithCase(const string &name, bool useLowercase) const;
  string getUserName(const Call &call);
  string getUserHref(string &userName) override;
  string getAssigneeTitle(Ticket *ticket) override;
  bool checkIfUserExists(const string &name) const override;

  // CORE functions
  Ticket *createNewTicket(const AddressSystem::addressInformation &system,
                          const Call &call) override;
  bool saveTicket(Ticket *ticket) override;
  bool moveTicket(Ticket *ticket) override;
  bool closeTicket(Ticket *ticket, const string &status) override;
  Ticket *getRunningTicketByName(string &name) override;
  string getDashboardInformation(istream &payload, string &urlParams) override;
};

// Structure for ticket assignee information
struct Assignee {
  std::string href;
  std::string title;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE(Assignee, href);
};

// Structure for ticket status (New, In Progress, Closed)
struct Status {
  std::string href;
  std::string title;

  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Status, href);
};

// Structure for ticket type (Call, Task, etc.)
struct Type {
  std::string href;
  std::string title;

  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Type, href);
};

// Structure for adding comments to tickets using OpenProject Formattable format
struct AddComment {
  std::string href;
  std::string method;
  std::string format = "markdown"; // OpenProject format (markdown/plain)
  std::string raw;                 // The actual comment content

  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AddComment, format, raw);
};

// Collection of all linked resources for a work package
struct Links {
  Assignee assignee;
  Type type;
  Status status;
  AddComment addComment;
  // 	friend void to_json(nlohmann::json& json, const Links& self);
  // 	friend void from_json(const nlohmann::json& json, Links& self);
};

// OpenProject work package implementation of Ticket interface
// Handles conversion between call data and OpenProject work package format
// Manages ticket lifecycle for incoming calls, accepted calls, and hangups
struct OpenProjectWorkPackage : public Ticket {
  const OpenProjectWorkPackageApi &mApi;
  string subject;
  string _type;
  Links fields;

  OpenProjectWorkPackage(nlohmann::json &config,
                         const OpenProjectWorkPackageApi &api);
  OpenProjectWorkPackage(const OpenProjectWorkPackageApi &api);
  ~OpenProjectWorkPackage();

  bool toTicketFromApiResponse(istream &response) override;
  bool setTicketForAcceptedCall(Call &call) override;
  void TicketToPackage();
  bool getTicketFromJson(nlohmann::json &response);

  // NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(OpenProjectWorkPackage, id,
  // subject, _type, description, createdAt, updatedAt, callId, caller,
  // callStart, callEnd, fields);

  // Die beiden müssen überarbeitet werden.

  bool getCallLength() override;
  nlohmann::json to_json();

private:
  string getDateTimeNow() override;

  // Helper methods for getTicketFromJson() - ticket selection and parsing
  void parseTicketFields(const nlohmann::json &ticketJson);
  int getStatusPriority(const string &statusId) const;
  bool shouldUpgradeTicket(const string &currentStatusId,
                           const string &candidateStatusId) const;

  // Helper methods for to_json() - JSON field and link construction
  void addBasicFieldsToJson(nlohmann::json &result) const;
  void addLinksToJson(nlohmann::json &result) const;
};
