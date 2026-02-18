/**
 * @file OpenProject.cpp
 * @brief OpenProject API integration for work package (ticket) management
 *
 * Architecture Overview:
 * This file implements the OpenProject plugin for ticket management in the AID
 * system. It provides comprehensive work package operations including creation,
 * updates, queries, and lifecycle management for call-based ticketing.
 *
 * OpenProject API Details:
 * - Uses HAL+JSON format for API responses
 * - Requires HTTP Basic Auth with API key (apikey:TOKEN)
 * - Base URL: /api/v3/ (configured in config.json)
 * - Work packages represent tickets in OpenProject
 *
 * Core Components:
 * 1. OpenProjectProjectApi: Project management operations (minimal usage)
 * 2. OpenProjectWorkPackageApi: Main ticket system implementation
 * 3. OpenProjectWorkPackage: Ticket data structure with JSON conversion
 *
 * Ticket Lifecycle:
 * - New: Initial state for incoming calls
 * - In Progress: Call accepted by user
 * - Closed/Tested/Rejected: Call completed with resolution
 *
 * Status Transitions:
 * OpenProject enforces workflow rules:
 * - Direct New → Closed transitions NOT allowed
 * - Must go through: New → In Progress → Closed
 * - Uses optimistic locking (lockVersion) for concurrent updates
 *
 * Dashboard Integration:
 * - Aggregates tickets from user's projects and assignments
 * - Detects active (ongoing) calls from description field
 * - Sorts by status priority: New > In Progress > Closed
 *
 * Custom Fields (configured per OpenProject instance):
 * - configCallId: Call identifier for linking
 * - configCallerNumber: Caller's phone number
 * - configCalledNumber: Dialed phone number
 * - configCallStartTimestamp: Call start time
 * - configCallEndTimestamp: Call end time
 *
 * Workarounds and Quirks:
 * - Some callers pass ticket IDs to getTicketByPhoneNumber() - handled via
 * tryGetTicketById()
 * - OpenProject API has inconsistent case handling for usernames - tries both
 * original and lowercase
 * - Description field stores call history as text (multiple users can have call
 * entries)
 */
#include "OpenProject.h"
#include <boost/date_time/local_time/local_time.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>
#include <iostream>
#include <sstream>

#include "Logger/Logger.h"
#include "Models/Call.h"
#include "Models/Ticket.h"

#include "json.hpp"
#include <map>
#include <set>

//==============================================================================
// OpenProjectProjectApi - Project Management Operations
//==============================================================================

/** @brief Default constructor */
OpenProjectProjectApi::OpenProjectProjectApi() {}

/** @brief Destructor */
OpenProjectProjectApi::~OpenProjectProjectApi() {}

//==============================================================================
// OpenProjectProject - Project Data Structure
//==============================================================================

/** @brief Default constructor */
OpenProjectProject::OpenProjectProject() {}

/** @brief Destructor */
OpenProjectProject::~OpenProjectProject() {}

void OpenProjectProject::to_json(nlohmann::json &j,
                                 const OpenProjectProject &p) {
  j = nlohmann::json{{"customField1", p.customField1}};
}

//==============================================================================
// OpenProjectWorkPackageApi - Main Ticket System Implementation
//==============================================================================

//------------------------------------------------------------------------------
// Constructors and Destructor
//------------------------------------------------------------------------------

/** @brief Default constructor */
OpenProjectWorkPackageApi::OpenProjectWorkPackageApi() : TicketSystem() {}

/** @brief Destructor */
OpenProjectWorkPackageApi::~OpenProjectWorkPackageApi() {}

/**
 * @brief Configuration constructor - loads OpenProject API settings
 *
 * Configuration parameters loaded:
 * - libPath: Path to shared library
 * - ticketSystemName: System identifier
 * - typeCall: OpenProject type ID for Call tickets
 * - statusRejected: Status ID for rejected calls
 * - statusTested: Status ID for tested/resolved calls
 * - Base class loads: configUrl, configApiToken, configStatusNew,
 * configStatusInProgress, etc.
 *
 * @param config JSON configuration from config.json
 */
OpenProjectWorkPackageApi::OpenProjectWorkPackageApi(nlohmann::json &config)
    : TicketSystem(config) {
  bool err = false;
  logging::Logger::info("INFO: Try to load OpenProjectWorkPackageApi from: " +
                        getConfigValue(config, "libPath", string(""), err));

  // Only initialize OpenProject-specific config values
  configTicketSystemName =
      getConfigValue(config, "ticketSystemName", string(""), err);
  configTypeCall = getConfigValue(config, "typeCall", string(""), err);
  configStatusRejected =
      getConfigValue(config, "statusRejected", string(""), err);
  configStatusTested = getConfigValue(config, "statusTested", string(""), err);

  logging::Logger::info("OpenProjectWorkPackageApi loaded");
  logging::Logger::info("API Token: " +
                        configApiToken); // This now comes from base class
  logging::Logger::debug(
      "OpenProjectWorkPackageApi configStatusInProgress (from base): '" +
      configStatusInProgress + "'");

  if (err) {
    logging::Logger::error(
        "ERROR: Missing Config values for OpenProjectWorkPackageApi, template "
        "has been written.");
  } else {
    logging::Logger::info("OpenProjectWorkPackageApi loaded without issues.");
  }
}

//------------------------------------------------------------------------------
// HTTP Request Helpers
//------------------------------------------------------------------------------

/**
 * @brief URL-encodes query string for OpenProject API filters
 *
 * OpenProject filters use JSON array format that must be URL-encoded:
 * Example: [{"status":{"operator":"=","values":["1"]}}]
 *
 * @param query Raw filter query string
 * @return URL-encoded query string safe for use in URLs
 */
std::string
OpenProjectWorkPackageApi::encodeQuery(const std::string &query) const {
  string res;
  CURL *curl = curl_easy_init();
  if (curl) {
    char *output = curl_easy_escape(curl, query.c_str(), query.length());
    if (output) {
      res = output;
      curl_free(output);
    }
  }
  curl_easy_cleanup(curl);

  return res;
}

/**
 * @brief Parses JSON from curlpp request stream
 *
 * @param request curlpp Easy request object with response data
 * @return Parsed JSON object from response
 */
nlohmann::json OpenProjectWorkPackageApi::prepareJson(curlpp::Easy &request) {
  nlohmann::json requestJson;
  std::stringstream jsonStream;
  jsonStream << request;
  jsonStream >> requestJson;

  return requestJson;
}

/**
 * @brief Prepares curlpp request with OpenProject API authentication
 *
 * Sets:
 * - Content-Type: application/json header
 * - HTTP Basic Auth with API key (username "apikey", password is token)
 *
 * @param request curlpp Easy request object to configure
 */
void OpenProjectWorkPackageApi::prepareRequest(curlpp::Easy &request) const {
  // Standard OpenProject API request setup - adds authentication and content
  // type
  std::list<std::string> headers;
  headers.push_back("Content-Type: application/json");
  request.setOpt(
      new curlpp::options::UserPwd(std::string("apikey:") + configApiToken));
  request.setOpt(new curlpp::options::HttpHeader(headers));
}

/**
 * @brief Suppresses stdout during curlpp request execution
 *
 * Redirects cout to /dev/null to hide curlpp debug output,
 * then restores original cout after request completes.
 *
 * @param request curlpp Easy request to execute silently
 */
void sendRequest(curlpp::Easy &request) {
  auto old_buf = std::cout.rdbuf();

  ofstream null_stream("/dev/null");
  cout.rdbuf(null_stream.rdbuf());
  request.perform();
  cout.rdbuf(old_buf);
}

//------------------------------------------------------------------------------
// Work Package Query Methods
//------------------------------------------------------------------------------

/**
 * @brief Retrieves all work packages from OpenProject
 *
 * @return JSON array with all work packages (HAL+JSON format)
 */
nlohmann::json OpenProjectWorkPackageApi::getWorkPackage() {
  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  nlohmann::json requestJson;

  prepareRequest(request);
  request.setOpt(new curlpp::options::Url(configUrl + "work_packages/"));
  request.setOpt(new curlpp::options::CustomRequest("GET"));
  requestJson = prepareJson(request);

  return requestJson;
}

nlohmann::json
OpenProjectWorkPackageApi::getCallWorkPackagesByStatus(int statusFromConfig) {

  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  nlohmann::json responseJson;
  stringstream response;
  string query = R"([{"type":{"operator":"=","values"[")" +
                 configUnknownNumberSaveLocation + R"("]}}]
			&filters=[{"status":{"operator":"=","values"[")" +
                 to_string(statusFromConfig) + R"("]}}])";

  prepareRequest(request);
  request.setOpt(new curlpp::options::Url(
      configUrl + "work_packages?filters=" + encodeQuery(query)));
  request.setOpt(new curlpp::options::CustomRequest("GET"));
  request.setOpt(new curlpp::options::WriteStream(&response));
  request.perform();

  response.seekg(0);
  response >> responseJson;

  logging::Logger::debug("Querying: " + configUrl +
                         "work_packages?filters=" + query);
  logging::Logger::debug("PackageByStatus: " + responseJson.dump());
  return responseJson;
}

nlohmann::json OpenProjectWorkPackageApi::getRunningWorkPackagesByPhoneNumber(
    string phoneNumber) {
  try {
    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    std::stringstream response;
    nlohmann::json responseJson;

    std::string query = R"(
			&filters=[{"type":{"operator":"=","values"[")" +
                        configUnknownNumberSaveLocation + R"("]}}]
			&filters=[{"status":{"operator":"!","values"[")" +
                        configStatusInProgress + R"(]}}])
			&filters=[{"customField7":{"operator":"=","values":[")" +
                        phoneNumber + R"("]}}])";
    encodeQuery(query);

    prepareRequest(request);
    request.setOpt(
        new curlpp::options::Url(configUrl + "work_packages?" + query));
    request.setOpt(new curlpp::options::CustomRequest("GET"));
    request.setOpt(new curlpp::options::WriteStream(&response));
    request.perform();

    response.seekg(0);
    response >> responseJson;

    return responseJson;
  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    throw std::runtime_error("getRunningWorkPackagesByPhoneNumber failed");
  }
}

//------------------------------------------------------------------------------
// Work Package Creation and Update Methods
//------------------------------------------------------------------------------

/**
 * @brief Creates a new work package (ticket) in OpenProject via POST
 *
 * API endpoint: POST /api/v3/projects/{projectId}/work_packages
 * Used when no existing ticket is found for incoming calls.
 *
 * @param json Work package data in OpenProject HAL+JSON format
 * @param projectId OpenProject project ID where ticket should be created
 * @return JSON response with created work package data (includes ID)
 * @throws std::runtime_error on curlpp or JSON parsing errors
 */
nlohmann::json OpenProjectWorkPackageApi::postWorkPackage(nlohmann::json json,
                                                          string projectId) {
  try {
    logging::Logger::debug("postWorkPackage(nlohmann::json json, string "
                           "projectId) started\nDEBUG: " +
                           json.dump(2));
    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    std::stringstream response;
    std::string value = json.dump();

    prepareRequest(request);
    request.setOpt(new curlpp::options::Url(configUrl + "projects/" +
                                            projectId + "/work_packages"));
    request.setOpt(new curlpp::options::PostFields(value));
    request.setOpt(new curlpp::options::PostFieldSize(value.length()));
    request.setOpt(new curlpp::options::WriteStream(&response));
    logging::Logger::debug("Post Value: " + value);
    request.perform();
    logging::Logger::debug("Postoperation done");
    json.clear();
    response.seekg(0);
    response >> json;
    logging::Logger::debug("nlohmann::json" + json.dump(2));
    logging::Logger::debug("postWorkPackage(nlohmann::json json, string "
                           "projectId) done.\nAnswer: " +
                           json.dump(2));
    return json;
  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    throw std::runtime_error("postWorkPackage failed");
  }
}

// Updates an existing work package (ticket) in OpenProject via PATCH request
// Used for updating tickets with new callId, status changes, or additional data
// Critical for duplicate prevention - updates existing tickets instead of
// creating new ones
void OpenProjectWorkPackageApi::patchWorkPackage(
    OpenProjectWorkPackage &package, const string &id) {
  try {
    logging::Logger::debug("patchWorkPackage(OpenProjectWorkPackage& package, "
                           "const string& id) started");
    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    std::stringstream response;
    nlohmann::json packageJson = package.to_json();
    std::string value = packageJson.dump();
    logging::Logger::debug(packageJson.dump(2));
    prepareRequest(request);
    request.setOpt(new curlpp::options::Url(configUrl + "work_packages/" + id));
    request.setOpt(new curlpp::options::PostFields(value));
    request.setOpt(new curlpp::options::PostFieldSize(value.length()));
    request.setOpt(new curlpp::options::CustomRequest("PATCH"));
    request.setOpt(new curlpp::options::WriteStream(&response));
    request.perform();

    nlohmann::json responseJson;
    response.seekg(0);
    response >> responseJson;

    logging::Logger::debug(responseJson.dump());
    if (responseJson["_type"] == "Error")
      throw runtime_error(responseJson.dump());
  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    throw std::runtime_error("patchWorkPackage failed");
  }
}

//------------------------------------------------------------------------------
// User Management Methods
//------------------------------------------------------------------------------

/**
 * @brief Gets OpenProject user API href by username
 *
 * @param userName Username to lookup
 * @return API href string (e.g., "/api/v3/users/5")
 */
string OpenProjectWorkPackageApi::getUserHref(string &userName) {
  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  stringstream response;
  nlohmann::json responseJson;

  string query =
      R"([{"login":{"operator":"=","values":[")" + userName + R"("]}}])";
  //    [{"login":{"operator":"=","values":["

  encodeQuery(query);
  prepareRequest(request);
  request.setOpt(
      new curlpp::options::Url(configUrl + "users?filters=" + query));
  request.setOpt(new curlpp::options::CustomRequest("GET"));
  request.setOpt(new curlpp::options::WriteStream(&response));
  request.perform();

  response.seekg(0);
  response >> responseJson;
  if (!responseJson.contains("_embedded") ||
      !responseJson["_embedded"].contains("elements") ||
      responseJson["_embedded"]["elements"].empty() ||
      !responseJson["_embedded"]["elements"][0].contains("id") ||
      !responseJson["_embedded"]["elements"][0]["id"].contains("self") ||
      !responseJson["_embedded"]["elements"][0]["id"]["self"].contains(
          "href")) {
    return "";
  }
  string result =
      responseJson["_embedded"]["elements"][0]["id"]["self"]["href"];

  return result;
}

// OpenProject API has inconsistent case handling - some users work with
// original case, others need lowercase This function tries both approaches to
// handle all username variations reliably
int OpenProjectWorkPackageApi::getUserId(const string &name) const {
  try {
    // First try with original case
    int result = getUserIdWithCase(name, false);
    if (result != -1) {
      return result;
    }

    // If not found, try with lowercase
    logging::Logger::info("getUserId: User '" + name +
                          "' not found with original case, trying lowercase");
    return getUserIdWithCase(name, true);
  } catch (exception &e) {
    logging::Logger::error("getUserId failed for user '" + name +
                           "': " + string(e.what()));
    return -1; // Return invalid ID
  }
}

int OpenProjectWorkPackageApi::getUserIdWithCase(const string &name,
                                                 bool useLowercase) const {
  try {
    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    stringstream response;
    nlohmann::json responseJson;

    string searchName = name;
    if (useLowercase) {
      std::transform(searchName.begin(), searchName.end(), searchName.begin(),
                     [](unsigned char c) { return std::tolower(c); });
    }

    string query =
        R"([{"login":{"operator":"=","values":[")" + searchName + R"("]}}])";

    prepareRequest(request);
    request.setOpt(new curlpp::options::Url(
        configUrl + "users?filters=" + encodeQuery(query)));
    request.setOpt(new curlpp::options::CustomRequest("GET"));
    request.setOpt(new curlpp::options::WriteStream(&response));
    request.perform();

    response.seekg(0);
    string responseStr = response.str();

    if (responseStr.empty()) {
      logging::Logger::debug(
          "getUserIdWithCase: Empty response from API for user: " + searchName +
          (useLowercase ? " (lowercase)" : " (original case)"));
      return -1; // Return invalid ID
    }

    responseJson = nlohmann::json::parse(responseStr);

    if (!responseJson.contains("_embedded") ||
        !responseJson["_embedded"].contains("elements") ||
        responseJson["_embedded"]["elements"].empty()) {
      logging::Logger::debug(
          "getUserIdWithCase: No user found for: " + searchName +
          (useLowercase ? " (lowercase)" : " (original case)"));
      return -1; // Return invalid ID
    }

    logging::Logger::info("getUserId: Found user '" + name + "' using " +
                          (useLowercase ? "lowercase" : "original case") +
                          " search");
    if (responseJson["_embedded"]["elements"][0].contains("id")) {
      return responseJson["_embedded"]["elements"][0]["id"];
    }
    return -1;
  } catch (exception &e) {
    logging::Logger::debug("getUserIdWithCase failed for user '" + name +
                           "': " + string(e.what()));
    return -1; // Return invalid ID
  }
}

nlohmann::json OpenProjectWorkPackageApi::getTicketsByAssignee(int id) {
  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  stringstream response;
  nlohmann::json responseJson;
  // openproject/mApi.v3/work_packages?filters=[{"assignee":{"operator": "=",
  // "values":["5"]}}]

  string query = R"([{"assignee":{"operator": "=", "values":[")" +
                 to_string(id) + R"("]}}])";

  prepareRequest(request);
  request.setOpt(new curlpp::options::Url(
      configUrl + "work_packages?filters=" + encodeQuery(query)));
  request.setOpt(new curlpp::options::CustomRequest("GET"));
  request.setOpt(new curlpp::options::WriteStream(&response));
  request.perform();

  response.seekg(0);
  response >> responseJson;

  return responseJson;
}

string OpenProjectWorkPackageApi::getUserName(const Call &call) {
  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  stringstream response;
  nlohmann::json responseJson;

  string query =
      R"([{"login":{"operator":"=","values":[")" + call.user + R"("]}}])";

  encodeQuery(query);
  prepareRequest(request);
  request.setOpt(
      new curlpp::options::Url(configUrl + "users?filters=" + query));
  request.setOpt(new curlpp::options::CustomRequest("GET"));
  request.setOpt(new curlpp::options::WriteStream(&response));
  request.perform();

  response.seekg(0);
  response >> responseJson;
  if (!responseJson.contains("_embedded") ||
      !responseJson["_embedded"].contains("elements") ||
      responseJson["_embedded"]["elements"].empty() ||
      !responseJson["_embedded"]["elements"][0].contains("_links") ||
      !responseJson["_embedded"]["elements"][0]["_links"].contains("self") ||
      !responseJson["_embedded"]["elements"][0]["_links"]["self"].contains(
          "title")) {
    return "";
  }
  string result =
      responseJson["_embedded"]["elements"][0]["_links"]["self"]["title"];

  return result;
}

string OpenProjectWorkPackageApi::getAssigneeTitle(Ticket *ticket) {
  try {
    OpenProjectWorkPackage *package =
        dynamic_cast<OpenProjectWorkPackage *>(ticket);
    if (!package) {
      logging::Logger::error("getAssigneeTitle: Invalid ticket type");
      return "";
    }

    // 		if(package->fields.assignee.href.empty()) {
    // 			logging::Logger::debug("getAssigneeTitle: No assignee href found in
    // ticket"); 			return "";
    // 		}

    // Extract user ID from href like "/api/v3/users/5"
    size_t lastSlash = package->fields.assignee.href.find_last_of('/');
    if (lastSlash == string::npos) {
      logging::Logger::error(
          "getAssigneeTitle: Invalid assignee href format: " +
          package->fields.assignee.href);
      return "";
    }
    getTicketByCallId(package->callId);

    string userIdStr = package->fields.assignee.href.substr(lastSlash + 1);

    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    stringstream response;
    nlohmann::json responseJson;

    prepareRequest(request);
    request.setOpt(new curlpp::options::Url(configUrl + "users/" + userIdStr));
    request.setOpt(new curlpp::options::CustomRequest("GET"));
    request.setOpt(new curlpp::options::WriteStream(&response));
    request.perform();

    response.seekg(0);
    response >> responseJson;

    if (!responseJson.contains("login") || responseJson["login"].is_null()) {
      logging::Logger::error(
          "getAssigneeTitle: No login field found for user ID: " + userIdStr);
      return "";
    }

    string name = responseJson["firstName"];
    logging::Logger::debug("getAssigneeTitle: Found login '" + name +
                           "' for user ID: " + userIdStr);
    return name;

  } catch (exception &e) {
    logging::Logger::error("getAssigneeTitle failed: " + string(e.what()));
    return "";
  }
}

//------------------------------------------------------------------------------
// Core Ticket Lifecycle Methods
//------------------------------------------------------------------------------

/**
 * @brief Creates a new ticket when no existing ticket is found for incoming
 * calls
 *
 * Ticket initialization:
 * - Sets subject from company name, person name, or phone number (fallback
 * priority)
 * - Sets status to "New" (configStatusNew)
 * - Sets type to "Call" (configTypeCall)
 * - Assigns to user if call.user is set
 * - Places in project from addressInformation or unknown number location
 *
 * @param adrSystem Address information from CardDAV lookup
 * @param call Call object with phone numbers and user
 * @return New OpenProjectWorkPackage ticket ready for saving
 */
Ticket *OpenProjectWorkPackageApi::createNewTicket(
    const AddressSystem::addressInformation &adrSystem, const Call &call) {
  logging::Logger::debug("createNewTicket() started");

  OpenProjectWorkPackage *package = new OpenProjectWorkPackage(*this);
  package->callId = formatCallId(call.callId);
  package->calledNumber = call.dialedPhoneNumber;
  package->callerNumber = call.phoneNumber;
  package->fields.status.href = configStatusNew;
  // Set subject - prefer company name, fallback to person name, or use phone
  // number
  if (!adrSystem.companyName.empty()) {
    package->subject = adrSystem.companyName;
  } else if (!adrSystem.name.empty()) {
    package->subject = adrSystem.name;
  } else {
    package->subject = call.phoneNumber; // Fallback to phone number
  }

  // Final fallback if still empty
  if (package->subject.empty()) {
    package->subject = "Eingehender Anruf von " + call.phoneNumber;
  }

  // Set title to same as subject for new tickets
  package->title = package->subject;
  package->fields.type.href = configTypeCall;

  if (!call.user.empty()) {
    string tempUser = call.user;
    package->userInformation = call.user;
    package->fields.assignee.href = getUserId(tempUser);
  }

  if (adrSystem.projectIds.size() > 0 && !adrSystem.projectIds[0].empty())
    package->ticketLocationId = adrSystem.projectIds[0];
  else
    package->ticketLocationId = configUnknownNumberSaveLocation;

  return package;
}

// Saves ticket data to OpenProject via PATCH request
// Converts ticket to WorkPackage format and updates via API
// Handles both new ticket creation and existing ticket updates
bool OpenProjectWorkPackageApi::saveTicket(Ticket *ticket) {
  logging::Logger::debug("saveTicket(Ticket* ticket) started)");
  OpenProjectWorkPackage *package =
      dynamic_cast<OpenProjectWorkPackage *>(ticket);
  assert(package);
  try {

    package->TicketToPackage();
    logging::Logger::debug("ID: " + ticket->id);
    nlohmann::json uploadData = package->to_json();
    logging::Logger::debug("saveTicket Data: " + uploadData.dump(2));
    if (package->id.empty())
      postWorkPackage(uploadData, package->ticketLocationId);
    else
      patchWorkPackage(*package, package->id);
    logging::Logger::debug(
        "saveTicket(Ticket* ticket) successfull\nDEBUG: Ticket location ID: " +
        package->ticketLocationId);
    return true;

  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    throw std::runtime_error("saveTicket failed");
  }
}

// Helper: Checks if an intermediate status transition is needed
// OpenProject workflow requires going through "In Progress" when moving from
// "New" to closed states
bool OpenProjectWorkPackageApi::needsIntermediateTransition(
    const string &currentStatus, const string &targetStatus) const {
  // Direct transition from "New" (status ID 1) to closed states requires
  // intermediate "In Progress" step This is enforced by OpenProject's workflow
  // configuration
  return currentStatus == "1";
}

// Helper: Determines which intermediate status to use for two-step transitions
// Returns "In Progress" status href for New -> Closed transitions
string OpenProjectWorkPackageApi::determineIntermediateStatus(
    const string &currentStatus, const string &targetStatus) const {
  // For New -> Closed transitions, OpenProject requires going through "In
  // Progress" first
  return configUrl + "statuses/" + configStatusInProgress;
}

// Helper: Executes a single status transition via PATCH request
// Returns true on success, false on failure
// Updates are done using optimistic locking with lockVersion
bool OpenProjectWorkPackageApi::executeStatusTransition(
    const string &ticketId, const string &statusHref, int lockVersion) {
  try {
    // Build PATCH request payload with lockVersion for optimistic locking
    nlohmann::json patchData = {
        {"lockVersion", lockVersion},
        {"_links", {{"status", {{"href", statusHref}}}}}};

    logging::Logger::debug("executeStatusTransition PATCH Data: " +
                           patchData.dump(2));

    // Execute PATCH request
    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    std::stringstream response;
    std::string patchPayload = patchData.dump();

    prepareRequest(request);
    request.setOpt(
        new curlpp::options::Url(configUrl + "work_packages/" + ticketId));
    request.setOpt(new curlpp::options::PostFields(patchPayload));
    request.setOpt(new curlpp::options::PostFieldSize(patchPayload.length()));
    request.setOpt(new curlpp::options::CustomRequest("PATCH"));
    request.setOpt(new curlpp::options::WriteStream(&response));

    logging::Logger::debug(
        "executeStatusTransition: Executing PATCH request to: " + statusHref);
    request.perform();

    // Parse and validate response
    string responseStr = response.str();
    logging::Logger::debug("executeStatusTransition Response: " + responseStr);

    if (responseStr.empty()) {
      logging::Logger::error(
          "executeStatusTransition: Empty response from API");
      return false;
    }

    try {
      nlohmann::json responseJson = nlohmann::json::parse(responseStr);
      if (!responseJson.contains("id")) {
        logging::Logger::error(
            "executeStatusTransition: No ID in response - transition failed");
        return false;
      }
      logging::Logger::info(
          "executeStatusTransition: Successfully transitioned ticket to: " +
          statusHref);
      return true;
    } catch (const nlohmann::json::exception &e) {
      logging::Logger::error(
          "executeStatusTransition: Failed to parse response JSON: " +
          string(e.what()));
      return false;
    }

  } catch (curlpp::RuntimeError &e) {
    logging::Logger::error("executeStatusTransition Curlpp error: " +
                           string(e.what()));
    return false;
  } catch (curlpp::LogicError &e) {
    logging::Logger::error("executeStatusTransition Curlpp error: " +
                           string(e.what()));
    return false;
  } catch (...) {
    logging::Logger::error("executeStatusTransition: Unknown error");
    return false;
  }
}

// Helper: Updates ticket's lockVersion from API response
// Critical for optimistic locking - each PATCH increments lockVersion
// Returns true if lockVersion was successfully updated
bool OpenProjectWorkPackageApi::updateTicketLockVersion(
    OpenProjectWorkPackage &ticket, const nlohmann::json &response) {
  if (!response.contains("id")) {
    logging::Logger::error(
        "updateTicketLockVersion: Invalid response - no ID field");
    return false;
  }

  if (response.contains("lockVersion")) {
    ticket.lockVersion = to_string(response["lockVersion"].get<int>());
    logging::Logger::debug("updateTicketLockVersion: Updated lockVersion to: " +
                           ticket.lockVersion);
    return true;
  }

  logging::Logger::error("updateTicketLockVersion: No lockVersion in response");
  return false;
}

// Closes a ticket by updating its status using OpenProject PATCH API
// Implements two-step status transition required by OpenProject workflow:
// 1. New tickets cannot transition directly to Closed - must go through In
// Progress first
// 2. Uses optimistic locking (lockVersion) to prevent concurrent modification
// conflicts
bool OpenProjectWorkPackageApi::closeTicket(Ticket *ticket,
                                            const string &status) {
  logging::Logger::debug(
      "closeTicket(Ticket* ticket, string& status) started for ticket: " +
      ticket->id);
  OpenProjectWorkPackage *package =
      dynamic_cast<OpenProjectWorkPackage *>(ticket);
  assert(package);

  try {
    // Determine the target status ID based on the status string using config
    // values
    string targetStatusHref;
    if (status == "closed") {
      targetStatusHref = configUrl + "statuses/" + configStatusClosed;
    } else if (status == "resolved" || status == "tested") {
      targetStatusHref = configUrl + "statuses/" + configStatusTested;
    } else if (status == "rejected") {
      targetStatusHref = configUrl + "statuses/" + configStatusRejected;
    } else {
      targetStatusHref =
          configUrl + "statuses/" + configStatusClosed; // Default to closed
    }

    logging::Logger::debug("closeTicket: Target status href: " +
                           targetStatusHref);
    logging::Logger::debug("closeTicket: Current ticket status: " +
                           package->status);

    // STEP 1: Check if intermediate transition is needed (New -> In Progress ->
    // Closed) OpenProject workflow doesn't allow direct New -> Closed
    // transitions
    if (needsIntermediateTransition(package->status, targetStatusHref)) {
      logging::Logger::info("closeTicket: Two-step transition required - "
                            "moving through 'In Progress' first");

      // Determine intermediate status (typically "In Progress")
      string intermediateStatusHref =
          determineIntermediateStatus(package->status, targetStatusHref);
      logging::Logger::debug("closeTicket: Intermediate status href: " +
                             intermediateStatusHref);

      // Execute first transition: New -> In Progress
      if (!executeStatusTransition(package->id, intermediateStatusHref,
                                   stoi(package->lockVersion))) {
        logging::Logger::error(
            "closeTicket: First transition (New -> In Progress) failed");
        return false;
      }

      // CRITICAL: Refresh lockVersion after first transition
      // Each PATCH increments lockVersion - must use latest value for next
      // request
      curlpp::Cleanup myCleanup;
      curlpp::Easy request;
      stringstream response;

      prepareRequest(request);
      request.setOpt(
          new curlpp::options::Url(configUrl + "work_packages/" + package->id));
      request.setOpt(new curlpp::options::CustomRequest("GET"));
      request.setOpt(new curlpp::options::WriteStream(&response));
      request.perform();

      response.seekg(0);
      try {
        nlohmann::json responseJson = nlohmann::json::parse(response);
        if (!updateTicketLockVersion(*package, responseJson)) {
          logging::Logger::error("closeTicket: Failed to update lockVersion "
                                 "after first transition");
          return false;
        }
        package->status = intermediateStatusHref;
      } catch (const nlohmann::json::exception &e) {
        logging::Logger::error(
            "closeTicket: Failed to parse refresh response: " +
            string(e.what()));
        return false;
      }
    }

    // STEP 2: Execute final transition to target status
    logging::Logger::info("closeTicket: Moving ticket to final status: " +
                          status);

    if (!executeStatusTransition(package->id, targetStatusHref,
                                 stoi(package->lockVersion))) {
      logging::Logger::error("closeTicket: Final transition to " + status +
                             " failed");
      return false;
    }

    // Update ticket's local state to reflect the change
    package->status = targetStatusHref;

    // Refresh lockVersion from final transition
    curlpp::Cleanup myCleanup2;
    curlpp::Easy finalRequest;
    stringstream finalResponse;

    prepareRequest(finalRequest);
    finalRequest.setOpt(
        new curlpp::options::Url(configUrl + "work_packages/" + package->id));
    finalRequest.setOpt(new curlpp::options::CustomRequest("GET"));
    finalRequest.setOpt(new curlpp::options::WriteStream(&finalResponse));
    finalRequest.perform();

    finalResponse.seekg(0);
    try {
      nlohmann::json finalResponseJson = nlohmann::json::parse(finalResponse);
      updateTicketLockVersion(*package, finalResponseJson);
    } catch (const nlohmann::json::exception &e) {
      logging::Logger::error(
          "closeTicket: Failed to parse final refresh response: " +
          string(e.what()));
      // Not critical - transition already succeeded
    }

    logging::Logger::info("closeTicket: Successfully closed ticket " +
                          package->id);
    return true;

  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("closeTicket Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("closeTicket Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("closeTicket JSON parse Error: ") + e.what());
  } catch (...) {
    throw std::runtime_error("closeTicket failed");
  }
}

bool checkMembersForMoving(Ticket *ticket) {
  try {
    stoi(ticket->id);
    stoi(ticket->ticketLocationId);
    stoi(ticket->lockVersion);

    return true;
  } catch (...) {
    cerr << "Invalid Id, ticketLocationId or lockVersion" << endl;
    return false;
  }
}

bool OpenProjectWorkPackageApi::moveTicket(Ticket *ticket) {
  try {
    if (!checkMembersForMoving(ticket))
      return false;

    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    stringstream response;
    nlohmann::json requestJson = {{"id", stoi(ticket->id)},
                                  {"lockVersion", stoi(ticket->lockVersion)}};

    nlohmann::json moveJson = {
        {"_type", "WorkPackage"},
        {"_links",
         {{"project",
           {{"href", "/mApi.v3/projects/" + ticket->ticketLocationId}}}}}};

    requestJson.update(moveJson);

    string requestJsonAsString = requestJson.dump();
    prepareRequest(request);
    request.setOpt(
        new curlpp::options::Url(configUrl + "work_packages/" + ticket->id));
    request.setOpt(new curlpp::options::PostFields(requestJsonAsString));
    request.setOpt(
        new curlpp::options::PostFieldSize(requestJsonAsString.length()));
    request.setOpt(new curlpp::options::CustomRequest("PATCH"));
    request.perform();

    return true;

  } catch (curlpp::RuntimeError &e) {
    cerr << string("Curlpp error ") + e.what() << endl;
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    cerr << string("Curlpp error: ") + e.what() << endl;
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    cerr << string("JSON parse Error") + e.what() << endl;
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    cerr << "moveTicket failed" << endl;
    throw std::runtime_error("moveTicket failed");
  }

  return false;
}

// Helper method: Extract username from URL path and get user ID
// Parses the URL parameter to extract username and looks up their OpenProject
// user ID
string OpenProjectWorkPackageApi::getUserIdFromPayload(istream &payload,
                                                       const string &urlParams,
                                                       int &userId) const {
  logging::Logger::debug("getUserIdFromPayload: Parsing URL params");
  logging::Logger::debug(urlParams);
  string test;
  payload >> test;
  logging::Logger::debug(test);

  // Extract username from URL path (e.g., "/dashboard/username" -> "username")
  size_t last_slash_pos = urlParams.find_last_of('/');
  string name = urlParams.substr(last_slash_pos + 1);
  logging::Logger::debug("getUserIdFromPayload: Extracted username: " + name);

  // Lookup user ID for filtering API calls
  userId = getUserId(name);
  if (userId == -1) {
    logging::Logger::error("getUserIdFromPayload: Failed to get user ID for: " +
                           name);
  }

  return name;
}

// Helper method: Get all project IDs where user is a member
// Queries OpenProject projects API with principal filter to find user's
// accessible projects
set<string> OpenProjectWorkPackageApi::getUserProjectIds(int userId) const {
  logging::Logger::debug(
      "getUserProjectIds: Getting projects where user is a member");

  // Build query to find projects by user ID
  string memberProjectsQuery = R"([{"principal":{"operator":"=","values":[")" +
                               to_string(userId) + R"("]}}])";

  // Execute API request to get user's member projects
  curlpp::Cleanup myCleanup;
  curlpp::Easy memberProjectsRequest;
  stringstream memberProjectsResponse;

  prepareRequest(memberProjectsRequest);
  memberProjectsRequest.setOpt(new curlpp::options::Url(
      configUrl + "projects?filters=" + encodeQuery(memberProjectsQuery)));
  memberProjectsRequest.setOpt(new curlpp::options::CustomRequest("GET"));
  memberProjectsRequest.setOpt(
      new curlpp::options::WriteStream(&memberProjectsResponse));
  memberProjectsRequest.perform();

  nlohmann::json memberProjects = nlohmann::json::parse(memberProjectsResponse);

  // Extract project IDs from response (used for filtering tickets by project
  // membership)
  set<string> userProjects;
  if (memberProjects.contains("_embedded") &&
      memberProjects["_embedded"].contains("elements")) {
    for (auto &project : memberProjects["_embedded"]["elements"]) {
      if (!project["id"].is_null()) {
        string projectId = to_string(project["id"]);
        userProjects.insert(projectId);
        logging::Logger::debug("getUserProjectIds: User is member of project " +
                               projectId + " (" +
                               project["name"].get<string>() + ")");
      }
    }
  }

  logging::Logger::debug("getUserProjectIds: Found " +
                         to_string(userProjects.size()) + " member projects");
  return userProjects;
}

// Helper method: Get all Call tickets with "New" or "In Progress" status
// Queries OpenProject work packages API for Call-type tickets in active states
nlohmann::json OpenProjectWorkPackageApi::getCallTicketsForProjects() const {
  logging::Logger::debug("getCallTicketsForProjects: Getting Call tickets with "
                         "New or In Progress status");

  // Build query for Call tickets with active status (New=1 or In Progress)
  string projectQuery = R"([{"status":{"operator":"=","values":["1",")" +
                        configStatusInProgress +
                        R"("]}},{"type":{"operator":"=","values":[")" +
                        configTypeCall + R"("]}}])";

  // Execute API request to get all Call tickets
  curlpp::Cleanup myCleanup;
  curlpp::Easy projectRequest;
  stringstream projectResponse;

  prepareRequest(projectRequest);
  projectRequest.setOpt(new curlpp::options::Url(
      configUrl + "work_packages?filters=" + encodeQuery(projectQuery)));
  projectRequest.setOpt(new curlpp::options::CustomRequest("GET"));
  projectRequest.setOpt(new curlpp::options::WriteStream(&projectResponse));
  projectRequest.perform();

  nlohmann::json projectTickets = nlohmann::json::parse(projectResponse);
  logging::Logger::debug(
      "getCallTicketsForProjects: Retrieved project Call tickets");

  return projectTickets;
}

// Helper method: Get Call tickets directly assigned to specific user
// Queries OpenProject for Call tickets assigned to user regardless of project
// membership
nlohmann::json
OpenProjectWorkPackageApi::getAssignedCallTickets(int userId) const {
  logging::Logger::debug(
      "getAssignedCallTickets: Getting Call tickets assigned to user " +
      to_string(userId));

  // Build query for Call tickets assigned to this user with active status
  string assignedQuery =
      R"([{"status":{"operator":"=","values":["1",")" + configStatusInProgress +
      R"("]}},{"type":{"operator":"=","values":[")" + configTypeCall +
      R"("]}},{"assignee":{"operator":"=","values":[")" + to_string(userId) +
      R"("]}}])";

  // Execute API request to get user's assigned Call tickets
  curlpp::Cleanup myCleanup;
  curlpp::Easy assignedRequest;
  stringstream assignedResponse;

  prepareRequest(assignedRequest);
  assignedRequest.setOpt(new curlpp::options::Url(
      configUrl + "work_packages?filters=" + encodeQuery(assignedQuery)));
  assignedRequest.setOpt(new curlpp::options::CustomRequest("GET"));
  assignedRequest.setOpt(new curlpp::options::WriteStream(&assignedResponse));
  assignedRequest.perform();

  nlohmann::json assignedTickets = nlohmann::json::parse(assignedResponse);
  logging::Logger::debug(
      "getAssignedCallTickets: Retrieved assigned Call tickets");

  return assignedTickets;
}

// Helper method: Merge and deduplicate tickets from multiple sources
// Combines project tickets (filtered by user membership) and assigned tickets
// Uses map to automatically deduplicate - same ticket can appear in both
// sources
map<int, nlohmann::json> OpenProjectWorkPackageApi::mergeAndDeduplicateTickets(
    const nlohmann::json &projectTickets, const nlohmann::json &assignedTickets,
    const set<string> &userProjects) const {
  logging::Logger::debug(
      "mergeAndDeduplicateTickets: Merging and deduplicating results");

  // Map automatically handles deduplication by ticket ID
  map<int, nlohmann::json> uniqueTickets;

  // Add project tickets (only from user's member projects for proper access
  // control)
  if (projectTickets.contains("_embedded") &&
      projectTickets["_embedded"].contains("elements")) {
    for (auto &ticket : projectTickets["_embedded"]["elements"]) {
      if (!ticket["_links"].contains("project") ||
          !ticket["_links"]["project"].contains("href"))
        continue;
      string projectHref = ticket["_links"]["project"]["href"];
      size_t lastSlash = projectHref.find_last_of('/');
      string projectId = projectHref.substr(lastSlash + 1);

      // Only include tickets from projects where user is a member
      if (userProjects.count(projectId) > 0) {
        int ticketId = ticket["id"];
        uniqueTickets[ticketId] = ticket;
        logging::Logger::debug(
            "mergeAndDeduplicateTickets: Added project ticket ID " +
            to_string(ticketId) + " from project " + projectId);
      } else {
        int ticketId = ticket["id"];
        logging::Logger::debug(
            "mergeAndDeduplicateTickets: Skipped ticket ID " +
            to_string(ticketId) + " from non-member project " + projectId);
      }
    }
  }

  // Add assigned tickets (these override project tickets if duplicate)
  if (assignedTickets.contains("_embedded") &&
      assignedTickets["_embedded"].contains("elements")) {
    for (auto &ticket : assignedTickets["_embedded"]["elements"]) {
      int ticketId = ticket["id"];
      if (uniqueTickets.count(ticketId) > 0) {
        logging::Logger::debug(
            "mergeAndDeduplicateTickets: Updated assigned ticket ID " +
            to_string(ticketId) + " (was from project)");
      } else {
        logging::Logger::debug(
            "mergeAndDeduplicateTickets: Added assigned ticket ID " +
            to_string(ticketId));
      }
      uniqueTickets[ticketId] = ticket; // Add or overwrite
    }
  }

  logging::Logger::debug("mergeAndDeduplicateTickets: Total unique tickets: " +
                         to_string(uniqueTickets.size()));
  return uniqueTickets;
}

// Helper method: Build JSON structure from deduplicated tickets
// Transforms OpenProject API response format to dashboard-friendly JSON
nlohmann::json OpenProjectWorkPackageApi::buildTicketJson(
    const map<int, nlohmann::json> &uniqueTickets) const {
  logging::Logger::debug("buildTicketJson: Building final JSON structure");

  nlohmann::json result = {{"tickets", nlohmann::json::array()}};

  // Transform each ticket to simplified dashboard format
  for (auto &pair : uniqueTickets) {
    auto &x = pair.second;

    // Extract status ID from href for sorting (e.g., "/api/v3/statuses/1" ->
    // "1")
    string statusIdStr = "";
    if (x.contains("_links") && x["_links"].contains("status") &&
        x["_links"]["status"].contains("href")) {
      string statusId = x["_links"]["status"]["href"];
      size_t lastSlash = statusId.find_last_of('/');
      statusIdStr = statusId.substr(lastSlash + 1);
    }

    // Build project URL slug (e.g., "My Project" -> "my-project")
    string cut = "";
    if (x.contains("_links") && x["_links"].contains("project") &&
        x["_links"]["project"].contains("title") &&
        !x["_links"]["project"]["title"].is_null()) {
      cut = x["_links"]["project"]["title"];
      std::replace(cut.begin(), cut.end(), ' ', '-');
      std::transform(cut.begin(), cut.end(), cut.begin(),
                     [](unsigned char c) { return std::tolower(c); });
    }

    // Create simplified ticket JSON for dashboard
    nlohmann::json temp = {
        {"href", configProjectWebBaseUrl + cut + "/work_packages/" +
                     to_string(x["id"])},
        {"id", x["id"]},
        {"title", (!x.contains("subject") || x["subject"].is_null())
                      ? ""
                      : x["subject"]},
        {"updatedAt", (!x.contains("updatedAt") || x["updatedAt"].is_null())
                          ? ""
                          : x["updatedAt"]},
        {"assignee", (!x.contains("assignee") || x["assignee"].is_null() ||
                      !x["assignee"].contains("title"))
                         ? nullptr
                         : x["assignee"]["title"]},
        {"description",
         (!x.contains("description") || x["description"].is_null())
             ? ""
             : (x["description"].contains("raw") &&
                        !x["description"]["raw"].is_null()
                    ? x["description"]["raw"]
                    : "")},
        {"status", (x.contains("_links") && x["_links"].contains("status") &&
                    x["_links"]["status"].contains("title") &&
                    !x["_links"]["status"]["title"].is_null())
                       ? x["_links"]["status"]["title"]
                       : ""},
        {"statusId", statusIdStr},
        {"callId", (!x.contains(configCallId) || x[configCallId].is_null())
                       ? nullptr
                       : x[configCallId]},
        {"callerNumber",
         (!x.contains(configCallerNumber) || x[configCallerNumber].is_null())
             ? nullptr
             : x[configCallerNumber]},
        {"calledNumber",
         (!x.contains(configCalledNumber) || x[configCalledNumber].is_null())
             ? nullptr
             : x[configCalledNumber]}};

    result["tickets"].push_back(temp);
  }

  logging::Logger::debug("buildTicketJson: Built JSON with " +
                         to_string(result["tickets"].size()) + " tickets");
  return result;
}

// Helper method: Detect active (ongoing) call for user from tickets
// Active call = In Progress ticket with callId where user's last "Call start"
// lacks "Call End"
nlohmann::json
OpenProjectWorkPackageApi::detectActiveCall(const nlohmann::json &result,
                                            const string &userName) const {
  logging::Logger::debug("detectActiveCall: Detecting active calls for user: " +
                         userName);

  nlohmann::json callInformation = nullptr;

  // Search through tickets for active call
  for (auto &ticket : result["tickets"]) {
    string ticketStatusId = ticket["statusId"].is_string()
                                ? ticket["statusId"].get<string>()
                                : to_string(ticket["statusId"].get<int>());

    // Only check In Progress tickets with callId
    if (ticketStatusId == configStatusInProgress &&
        !ticket["callId"].is_null()) {
      string description =
          ticket["description"].is_null() ? "" : ticket["description"];
      string callId = ticket["callId"];

      // Active call detection: Find LAST "CurrentUser: Call start:" line
      // without "Call End" Uses rfind to get most recent call start for this
      // user
      string userCallPattern = userName + ": Call start:";
      size_t lastStartPos =
          description.rfind(userCallPattern); // Find LAST occurrence

      if (lastStartPos != string::npos) {
        // Extract just this user's call line to check for completion
        size_t lineEnd = description.find('\n', lastStartPos);
        if (lineEnd == string::npos)
          lineEnd = description.length();
        string userCallLine =
            description.substr(lastStartPos, lineEnd - lastStartPos);

        // If line missing "Call End", call is still active
        if (userCallLine.find("Call End") == string::npos) {

          // Extract project info for active call
          string projectTitle = ticket["title"];
          string ticketId = to_string((int)ticket["id"]);
          string callerNumber =
              ticket["callerNumber"].is_null() ? "" : ticket["callerNumber"];

          // Use default project ID (AddressSystem data is combined later by
          // UiController)
          string projectIds = configUnknownNumberSaveLocation;
          logging::Logger::debug(
              "detectActiveCall: Using default project ID: " + projectIds);

          callInformation = {{"ticketId", ticketId},
                             {"callId", callId},
                             {"projectIds", projectIds},
                             {"projectTitle", projectTitle},
                             {"callerNumber", callerNumber},
                             {"calledNumber", ticket["calledNumber"].is_null()
                                                  ? ""
                                                  : ticket["calledNumber"]}};

          logging::Logger::debug(
              "detectActiveCall: Active call found - Ticket: " + ticketId +
              ", CallId: " + callId);
          break; // Use first active call found
        }
      }
    }
  }

  return callInformation;
}

// Helper method: Sort tickets by status priority (New first, then In Progress)
// Within each status group, sort by most recent update time
void OpenProjectWorkPackageApi::sortTicketsByStatus(
    nlohmann::json &result) const {
  logging::Logger::debug("sortTicketsByStatus: Sorting tickets");

  auto &tickets = result["tickets"];

  // Sort tickets: New (status=1) first, then In Progress
  // Within same status group, sort by updatedAt (most recent first)
  std::sort(tickets.begin(), tickets.end(),
            [this](const nlohmann::json &a, const nlohmann::json &b) {
              string statusA = a["statusId"];
              string statusB = b["statusId"];

              // New tickets (status=1) come first (highest priority for
              // dashboard)
              if (statusA == "1" && statusB != "1")
                return true;
              if (statusA != "1" && statusB == "1")
                return false;

              // Within same status group, sort by updatedAt (most recent first)
              return a["updatedAt"] > b["updatedAt"];
            });

  logging::Logger::debug("sortTicketsByStatus: Sorted " +
                         to_string(tickets.size()) + " tickets");
}

//------------------------------------------------------------------------------
// Dashboard Data Aggregation
//------------------------------------------------------------------------------

/**
 * @brief Aggregates dashboard data for a specific user
 *
 * Complex multi-phase process:
 * 1. Extract username from URL, lookup user ID
 * 2. Get all projects where user is a member
 * 3. Get all Call tickets with New or In Progress status
 * 4. Get Call tickets directly assigned to user
 * 5. Merge and deduplicate (same ticket can appear in both sources)
 * 6. Filter by user's project membership for proper access control
 * 7. Build simplified JSON for dashboard consumption
 * 8. Detect active (ongoing) calls from ticket descriptions
 * 9. Sort by status priority (New first, then In Progress)
 *
 * Result JSON structure:
 * {
 *   "tickets": [ { id, title, status, callId, callerNumber, ... }, ... ],
 *   "callInformation": { ticketId, callId, projectIds, callerNumber, ... } |
 * null
 * }
 *
 * @param payload Request payload (unused, username from URL)
 * @param urlParams URL path containing username (e.g., "/dashboard/username")
 * @return JSON string with tickets array and active call information
 */
string OpenProjectWorkPackageApi::getDashboardInformation(istream &payload,
                                                          string &urlParams) {
  // Complex dashboard aggregation - combines multiple API calls to build user's
  // ticket view Multi-step process: 1) Get user projects 2) Get project tickets
  // 3) Get assigned tickets 4) Merge & deduplicate
  logging::Logger::debug(
      "getDashboardInformation: Starting dashboard data collection");

  // STEP 1: Extract username and get user ID from payload
  int userId;
  string name = getUserIdFromPayload(payload, urlParams, userId);
  if (userId == -1) {
    return R"({"tickets":[]})";
  }

  // STEP 2: Get all projects where user is a member (determines visibility)
  set<string> userProjects = getUserProjectIds(userId);

  // STEP 3: Get all Call tickets (New + In Progress) from OpenProject
  nlohmann::json projectTickets = getCallTicketsForProjects();

  // STEP 4: Get Call tickets directly assigned to user (ensures assigned
  // tickets shown even without project membership)
  nlohmann::json assignedTickets = getAssignedCallTickets(userId);

  // STEP 5: Merge and deduplicate tickets by ID
  map<int, nlohmann::json> uniqueTickets =
      mergeAndDeduplicateTickets(projectTickets, assignedTickets, userProjects);

  // STEP 6: Build final JSON structure
  nlohmann::json result = buildTicketJson(uniqueTickets);

  // STEP 7: Detect active (ongoing) calls for this user
  nlohmann::json callInformation = detectActiveCall(result, name);

  // STEP 8: Sort tickets by status priority (New first, then In Progress)
  sortTicketsByStatus(result);

  // STEP 9: Add callInformation to final result and return
  result["callInformation"] = callInformation;

  string resultString = result.dump(2);
  logging::Logger::debug(
      "getDashboardInformation: Returning merged JSON with " +
      to_string(result["tickets"].size()) + " tickets and callInformation");
  return resultString;
}

//------------------------------------------------------------------------------
// Ticket Lookup Methods
//------------------------------------------------------------------------------

/**
 * @brief Finds ticket by call ID using OpenProject custom field filter
 *
 * Uses "~" (contains) operator for flexible matching.
 *
 * @param callId Call identifier to search for
 * @return Ticket pointer if found, nullptr otherwise
 * @throws std::runtime_error on curlpp or JSON errors
 */
Ticket *OpenProjectWorkPackageApi::getTicketByCallId(string &callId) {
  try {
    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    stringstream response;
    nlohmann::json responseJson;

    string query = R"([{")" + configCallId +
                   R"(":{"operator":"~","values":[")" + callId + R"("]}}])";

    prepareRequest(request);
    request.setOpt(new curlpp::options::Url(
        configUrl + "work_packages?filters=" + encodeQuery(query)));
    request.setOpt(new curlpp::options::CustomRequest("GET"));
    request.setOpt(new curlpp::options::WriteStream(&response));
    request.perform();

    response.seekg(0);
    response >> responseJson;
    unique_ptr<OpenProjectWorkPackage> package(
        new OpenProjectWorkPackage(*this));
    logging::Logger::debug(
        "DEBUG: configStatusInProgress in getTicketByCallId: '" +
        configStatusInProgress + "'");
    if (!package->getTicketFromJson(responseJson)) {
      return nullptr;
    };
    logging::Logger::debug(configUrl + "work_packages?filters=" + query);
    logging::Logger::debug(responseJson.dump());
    return package.release();

  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    throw std::runtime_error("getTicketeByCallId failed");
  }
}

bool OpenProjectWorkPackageApi::checkIfUserExists(const string &name) const {
  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  stringstream response;
  nlohmann::json responseJson;
  string ln = name;
  std::transform(ln.begin(), ln.end(), ln.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  string query =
      R"([{ "login": { "operator": "=", "values": [")" + ln + R"("] } }])";

  prepareRequest(request);
  request.setOpt(new curlpp::options::Url(
      configUrl + "users?filters=" + encodeQuery(query)));
  request.setOpt(new curlpp::options::CustomRequest("GET"));
  request.setOpt(new curlpp::options::WriteStream(&response));
  request.perform();

  response.seekg(0);
  responseJson = nlohmann::json::parse(response);

  return responseJson["_embedded"]["elements"].size() > 0;
}

Ticket *OpenProjectWorkPackageApi::getTicketByCallIdContains(string &callId) {
  try {
    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    stringstream response;
    nlohmann::json responseJson;

    // Use "~" operator for contains instead of "=" for exact match
    string query = R"([{")" + configCallId +
                   R"(":{"operator":"~","values":[")" + callId + R"("]}}])";

    prepareRequest(request);
    request.setOpt(new curlpp::options::Url(
        configUrl + "work_packages?filters=" + encodeQuery(query)));
    request.setOpt(new curlpp::options::CustomRequest("GET"));
    request.setOpt(new curlpp::options::WriteStream(&response));
    request.perform();

    response.seekg(0);
    response >> responseJson;
    unique_ptr<OpenProjectWorkPackage> package(
        new OpenProjectWorkPackage(*this));
    logging::Logger::debug(
        "DEBUG: configStatusInProgress in getTicketByCallIdContains: '" +
        configStatusInProgress + "'");
    if (!package->getTicketFromJson(responseJson)) {
      return nullptr;
    };
    logging::Logger::debug(configUrl + "work_packages?filters=" + query);
    logging::Logger::debug(responseJson.dump());
    return package.release();

  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    throw std::runtime_error("getTicketeByCallIdContains failed");
  }
}

// Fetch a single work package from OpenProject API by ID
// Returns the raw JSON response from the API
nlohmann::json
OpenProjectWorkPackageApi::fetchWorkPackageById(const string &id) {
  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  stringstream response;
  nlohmann::json responseJson;

  // Make direct API call to work_packages endpoint
  prepareRequest(request);
  request.setOpt(new curlpp::options::Url(configUrl + "work_packages/" + id));
  request.setOpt(new curlpp::options::CustomRequest("GET"));
  request.setOpt(new curlpp::options::WriteStream(&response));
  request.perform();

  response.seekg(0);
  response >> responseJson;

  return responseJson;
}

// Wrap a single ticket JSON in the collection format expected by
// getTicketFromJson OpenProject's direct endpoint returns a single object, but
// parser expects array format
nlohmann::json OpenProjectWorkPackageApi::wrapSingleTicketResponse(
    const nlohmann::json &ticketJson) {
  return {{"_embedded", {{"elements", nlohmann::json::array({ticketJson})}}}};
}

Ticket *OpenProjectWorkPackageApi::getTicketById(string &id) {
  logging::Logger::debug("getTicketById started for ID: " + id);

  try {
    // Fetch ticket from API
    nlohmann::json responseJson = fetchWorkPackageById(id);

    // Check for API error response
    if (responseJson.contains("_type") && responseJson["_type"] == "Error") {
      logging::Logger::debug("getTicketById: API returned error: " +
                             responseJson.dump());
      return nullptr;
    }

    // Create ticket object and parse response
    OpenProjectWorkPackage *package = new OpenProjectWorkPackage(*this);

    // Wrap single ticket response in collection format for parser compatibility
    nlohmann::json wrappedResponse = wrapSingleTicketResponse(responseJson);

    if (!package->getTicketFromJson(wrappedResponse)) {
      logging::Logger::debug("getTicketById: Failed to parse ticket from JSON");
      delete package;
      return nullptr;
    }

    logging::Logger::debug("getTicketById: Successfully found ticket ID: " +
                           package->id);
    return package;

  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error: ") + e.what());
  } catch (...) {
    throw std::runtime_error("getTicketById failed");
  }
}

// Helper: Try to get ticket by ID (workaround for callers passing ticket ID
// instead of phone number) Some callers incorrectly pass ticket IDs to this
// method, so we detect and handle that case Returns ticket if input is a valid
// numeric ID, nullptr otherwise
Ticket *OpenProjectWorkPackageApi::tryGetTicketById(const string &possibleId) {
  // Check if input looks like a ticket ID (all digits, no "+" phone prefix)
  bool isTicketId = true;
  for (char c : possibleId) {
    if (!isdigit(c)) {
      isTicketId = false;
      break;
    }
  }

  if (!isTicketId || possibleId.find("+") != string::npos) {
    return nullptr; // Not a ticket ID, caller should search by phone number
  }

  logging::Logger::debug(
      "tryGetTicketById: Input looks like ticket ID, calling direct API: " +
      possibleId);

  try {
    // Use direct work package endpoint for ticket ID lookup
    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    stringstream response;
    nlohmann::json responseJson;

    logging::Logger::debug("tryGetTicketById: Making direct API call: " +
                           configUrl + "work_packages/" + possibleId);
    prepareRequest(request);
    request.setOpt(
        new curlpp::options::Url(configUrl + "work_packages/" + possibleId));
    request.setOpt(new curlpp::options::CustomRequest("GET"));
    request.setOpt(new curlpp::options::WriteStream(&response));
    request.perform();

    response.seekg(0);
    response >> responseJson;

    logging::Logger::debug("tryGetTicketById: Direct API response: " +
                           responseJson.dump());

    // Check if we got an error response (ticket doesn't exist)
    if (responseJson.contains("_type") && responseJson["_type"] == "Error") {
      logging::Logger::debug("tryGetTicketById: Direct API returned error: " +
                             responseJson.dump());
      return nullptr;
    }

    OpenProjectWorkPackage *package = new OpenProjectWorkPackage(*this);

    // For direct work package API, wrap response for getTicketFromJson
    nlohmann::json wrappedResponse = {
        {"_embedded", {{"elements", nlohmann::json::array({responseJson})}}}};

    if (!package->getTicketFromJson(wrappedResponse)) {
      logging::Logger::debug(
          "tryGetTicketById: Failed to parse ticket from direct API JSON");
      delete package;
      return nullptr;
    }

    logging::Logger::debug(
        "tryGetTicketById: Successfully found ticket via direct API, ID: " +
        package->id);
    return package;

  } catch (const exception &e) {
    logging::Logger::debug("tryGetTicketById: Exception: " + string(e.what()));
    return nullptr;
  }
}

// Helper: Search for tickets by phone number in OpenProject
// Queries for tickets with matching caller phone number and active status (New
// or In Progress) Returns the most recent matching ticket, or nullptr if no
// match found
Ticket *OpenProjectWorkPackageApi::searchTicketsByPhoneNumber(
    const string &phoneNumber) {
  try {
    logging::Logger::debug(
        "searchTicketsByPhoneNumber: Searching for phone number: " +
        phoneNumber);

    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    stringstream response;
    nlohmann::json responseJson;

    // Build query: search by caller phone number with New or In Progress
    // status, sorted by newest first
    string filterQuery =
        R"([{")" + configCallerNumber + R"(":{"operator":"=","values":[")" +
        phoneNumber + R"("]}},{"status":{"operator":"=","values":[")" +
        configStatusInProgress + R"(",")" + configStatusNew + R"("]}}])";
    string sortByQuery = R"([["id", "desc"]])";

    logging::Logger::info(configUrl + "work_packages?filters=" + filterQuery +
                          "&sortBy=" + sortByQuery);

    prepareRequest(request);
    request.setOpt(new curlpp::options::Url(
        configUrl + "work_packages?filters=" + encodeQuery(filterQuery) +
        "&sortBy=" + encodeQuery(sortByQuery)));
    request.setOpt(new curlpp::options::CustomRequest("GET"));
    request.setOpt(new curlpp::options::WriteStream(&response));
    request.perform();

    response.seekg(0);
    string answerTest;
    while (getline(response, answerTest)) {
    }

    logging::Logger::debug("searchTicketsByPhoneNumber: API response: " +
                           answerTest);

    // Parse JSON from response string
    try {
      responseJson = nlohmann::json::parse(answerTest);
    } catch (const nlohmann::json::parse_error &e) {
      logging::Logger::error(
          "JSON parse error in searchTicketsByPhoneNumber: " +
          string(e.what()));
      return nullptr;
    }

    OpenProjectWorkPackage *package = new OpenProjectWorkPackage(*this);
    if (!package->getTicketFromJson(responseJson)) {
      delete package;
      return nullptr;
    }

    logging::Logger::debug("searchTicketsByPhoneNumber: Found ticket ID: " +
                           package->id);
    return package;

  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    throw std::runtime_error("searchTicketsByPhoneNumber failed");
  }
}

// Getting json with all tickets with matching caller phonenumbers and where
// ticket set to "New" or "In Progress". Ticket gets the latest match. Searches
// for existing tickets by caller phone number with "New" or "In Progress"
// status Used for duplicate prevention to find existing tickets instead of
// creating new ones WORKAROUND: Some callers confuse ticket ID with phone
// number, so we try both approaches
Ticket *OpenProjectWorkPackageApi::getTicketByPhoneNumber(string &phoneNumber) {
  try {
    // Basic validation: reject empty phone numbers
    if (phoneNumber.empty()) {
      logging::Logger::debug("getTicketByPhoneNumber: phoneNumber is empty!");
      logging::Logger::error("getTicketByPhoneNumber: phoneNumber is empty!");
      return nullptr;
    }

    // WORKAROUND: Try as ticket ID first
    // Some callers pass ticket IDs to this method instead of calling
    // getTicketById() This maintains backward compatibility with existing
    // incorrect usage
    Ticket *ticket = tryGetTicketById(phoneNumber);
    if (ticket) {
      logging::Logger::info(
          "getTicketByPhoneNumber: Found ticket by ID (workaround applied)");
      return ticket;
    }

    // Not a ticket ID - normalize phone number format
    // Remove leading "+" from international format for OpenProject query
    string normalizedPhoneNumber = phoneNumber;
    if (normalizedPhoneNumber.find("+") != string::npos) {
      normalizedPhoneNumber = normalizedPhoneNumber.substr(1);
    }

    logging::Logger::debug(
        "getTicketByPhoneNumber: Searching by phone number: " +
        normalizedPhoneNumber);

    // Search for tickets by phone number
    return searchTicketsByPhoneNumber(normalizedPhoneNumber);

  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    throw std::runtime_error("getTicketByPhoneNumber failed");
  }
}

// Build OpenProject filter query to search for Call tickets in a project
// Searches for tickets with type "Call", status "New" or "In Progress"
string OpenProjectWorkPackageApi::buildCallTicketSearchFilter(
    const string &projectId) const {
  return R"([{"project":{"operator":"=","values":[")" + projectId +
         R"("]}},{"type":{"operator":"=","values":[")" + configTypeCall +
         R"("]}},{"status":{"operator":"=","values":[")" + configStatusNew +
         R"(",")" + configStatusInProgress + R"("]}}])";
}

Ticket *OpenProjectWorkPackageApi::getLatestCallTicketInProject(
    const string &projectId) const {
  // Validate input
  if (projectId.empty()) {
    logging::Logger::error("getLatestCallTicketInProject: projectId is empty");
    return nullptr;
  }

  logging::Logger::debug("Searching for Call tickets in project: " + projectId);

  try {
    // Build and execute search query
    string filterQuery = buildCallTicketSearchFilter(projectId);
    string sortByQuery = R"([["id", "desc"]])"; // Sort by newest first
    nlohmann::json responseJson =
        executeTicketSearchQuery(filterQuery, sortByQuery);

    // Parse response into ticket object
    OpenProjectWorkPackage *package = new OpenProjectWorkPackage(*this);
    if (!package->getTicketFromJson(responseJson)) {
      logging::Logger::info("No Call tickets found in project " + projectId);
      delete package;
      return nullptr;
    }

    // Set project ID for the ticket
    package->projectId = projectId;
    logging::Logger::info("Found latest Call ticket ID: " + package->id +
                          " in project: " + projectId);
    return package;

  } catch (const nlohmann::json::parse_error &e) {
    logging::Logger::error("JSON parse error: " + string(e.what()));
    return nullptr;
  } catch (const exception &e) {
    logging::Logger::error("getLatestCallTicketInProject failed: " +
                           string(e.what()));
    return nullptr;
  }
}

// Build OpenProject filter query to search for tickets by name in a project
// Searches for tickets with matching subject, status "New" or "In Progress"
string OpenProjectWorkPackageApi::buildTicketSearchByNameFilter(
    const string &projectId, const string &ticketName) const {
  return R"([{"project":{"operator":"=","values":[")" + projectId +
         R"("]}},{"subject":{"operator":"~","values":[")" + ticketName +
         R"("]}},{"status":{"operator":"=","values":[")" + configStatusNew +
         R"(",")" + configStatusInProgress + R"("]}}])";
}

// Execute a ticket search query with given filters and sorting
// Returns the parsed JSON response from the API
nlohmann::json OpenProjectWorkPackageApi::executeTicketSearchQuery(
    const string &filterQuery, const string &sortByQuery) const {
  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  stringstream response;

  // Build and execute the search query
  prepareRequest(request);
  request.setOpt(new curlpp::options::Url(
      configUrl + "work_packages?filters=" + encodeQuery(filterQuery) +
      "&sortBy=" + encodeQuery(sortByQuery)));
  request.setOpt(new curlpp::options::CustomRequest("GET"));
  request.setOpt(new curlpp::options::WriteStream(&response));
  request.perform();

  // Parse the response
  response.seekg(0);
  string responseText;
  while (getline(response, responseText)) {
  }

  return nlohmann::json::parse(responseText);
}

Ticket *OpenProjectWorkPackageApi::getLatestTicketInProjectByName(
    const string &projectId, const string &ticketName) const {
  // Validate input parameters
  if (projectId.empty()) {
    logging::Logger::error(
        "getLatestTicketInProjectByName: projectId is empty");
    return nullptr;
  }
  if (ticketName.empty()) {
    logging::Logger::error(
        "getLatestTicketInProjectByName: ticketName is empty");
    return nullptr;
  }

  logging::Logger::debug("Searching for ticket by name '" + ticketName +
                         "' in project: " + projectId);

  try {
    // Build search filter and execute query
    string filterQuery = buildTicketSearchByNameFilter(projectId, ticketName);
    string sortByQuery = R"([["id", "desc"]])"; // Sort by newest first
    nlohmann::json responseJson =
        executeTicketSearchQuery(filterQuery, sortByQuery);

    // Parse response into ticket object
    OpenProjectWorkPackage *package = new OpenProjectWorkPackage(*this);
    if (!package->getTicketFromJson(responseJson)) {
      logging::Logger::info("No tickets found by name '" + ticketName +
                            "' in project " + projectId);
      delete package;
      return nullptr;
    }

    logging::Logger::info("Found latest ticket ID: " + package->id +
                          " in project: " + projectId);
    return package;

  } catch (const nlohmann::json::parse_error &e) {
    logging::Logger::error("JSON parse error: " + string(e.what()));
    return nullptr;
  } catch (const exception &e) {
    logging::Logger::error("getLatestTicketInProjectByName failed: " +
                           string(e.what()));
    return nullptr;
  }
}

Ticket *OpenProjectWorkPackageApi::getRunningTicketByName(string &callerName) {
  try {
    logging::Logger::debug("getRunningTicketByName started");
    if (callerName.empty()) {
      logging::Logger::debug(
          "getRunningTicketByName(string& callerName) callerNumber is empty!");
      logging::Logger::error(
          "getRunningTicketByName(string& callerName) callerNumber is empty!");
      return nullptr;
    }

    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    stringstream response;
    nlohmann::json responseJson;
    // Search for tickets with both "New" and "In Progress" status
    string query = R"([{"status":{"operator":"=","values":[")" +
                   configStatusNew + R"(",")" + configStatusInProgress +
                   R"("]}},{")" + configCallerNumber +
                   R"(":{"operator":"=","values":[")" + callerName + R"("]}}])";

    logging::Logger::debug("Querying: " + configUrl +
                           "work_packages?filters=" + query);

    prepareRequest(request);
    request.setOpt(new curlpp::options::Url(
        configUrl + "work_packages?filters=" + encodeQuery(query)));
    request.setOpt(new curlpp::options::CustomRequest("GET"));
    request.setOpt(new curlpp::options::WriteStream(&response));

    request.perform();

    response.seekg(0);
    response >> responseJson;
    logging::Logger::info(responseJson.dump(2));

    OpenProjectWorkPackage *package = new OpenProjectWorkPackage(*this);
    if (!package->getTicketFromJson(responseJson))
      return nullptr;

    return package;
  } catch (...) {
    logging::Logger::debug("getRunningTicketByName failed");
    return nullptr;
  }
}

string OpenProjectWorkPackageApi::getCurrentTickets() {
  try {
    curlpp::Cleanup myCleanup;
    curlpp::Easy request;
    stringstream response;
    nlohmann::json responseJson;

    string filterQuery = R"([{"status":{"operator":"=","values":[")" +
                         configStatusInProgress + "\" , \"" + configStatusNew +
                         R"("]}}])";
    string sortByQuery = R"([["id", "desc"]])";
    encodeQuery(filterQuery);
    encodeQuery(sortByQuery);

    string q = R"([{"status":{"operator":"=","values":["1"]}}])";
    encodeQuery(q);

    prepareRequest(request);
    request.setOpt(
        new curlpp::options::Url(configUrl + "work_packages?filters=" + q));
    // 		request.setOpt(new
    // curlpp::options::Url(configUrl+"work_packages?filters="+filterQuery+"&sortBy="+sortByQuery));
    request.setOpt(new curlpp::options::CustomRequest("GET"));
    request.setOpt(new curlpp::options::WriteStream(&response));

    sendRequest(request);

    response.seekg(0);
    response >> responseJson;

    return responseJson.dump(2);

  } catch (curlpp::RuntimeError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (curlpp::LogicError &e) {
    throw runtime_error(string("Curlpp error: ") + e.what());
  } catch (nlohmann::json::exception &e) {
    throw runtime_error(string("JSON parse Error") + e.what());
  } catch (...) {
    throw std::runtime_error("getTicketeByCallId failed");
  }
}

template <typename T>
T OpenProjectWorkPackageApi::getConfigValue(nlohmann::json &config,
                                            const char *param,
                                            const T &defaultVal,
                                            bool &hasError) {
  try {
    return config[param];
  } catch (nlohmann::json::exception &e) {
    config[param] = defaultVal;
    hasError = true;
    return defaultVal;
  }
}

//==============================================================================
// OpenProjectWorkPackage - Ticket Data Structure with JSON Conversion
//==============================================================================

//------------------------------------------------------------------------------
// Constructors and Destructor
//------------------------------------------------------------------------------

/**
 * @brief Constructor with API reference
 *
 * @param api Reference to OpenProjectWorkPackageApi for configuration access
 */
OpenProjectWorkPackage::OpenProjectWorkPackage(
    const OpenProjectWorkPackageApi &api)
    : Ticket(api), mApi(api) {
  logging::Logger::debug("DEBUG: OpenProjectWorkPackage constructor - "
                         "api.configStatusInProgress: '" +
                         api.configStatusInProgress + "'");
}

OpenProjectWorkPackage::OpenProjectWorkPackage(
    nlohmann::json &config, const OpenProjectWorkPackageApi &api)
    : Ticket(config, api), mApi(api) {
  logging::Logger::info("Ticket Constructor startet");
  bool err = false;
  userInformation = mApi.configUser;

  if (!err)
    logging::Logger::info("OpenProjectWorkPackage loaded without issues");
  if (err)
    logging::Logger::error(
        "Missing config values for Ticket, template has been written!");
}

OpenProjectWorkPackage::~OpenProjectWorkPackage() {}

bool OpenProjectWorkPackage::toTicketFromApiResponse(istream &response) {
  nlohmann::json json;
  response >> json;

  bool success = getTicketFromJson(json);
  return success;
}

// Helper: Parse and extract all ticket fields from JSON element
// Populates this ticket's fields (id, callId, title, status, etc.) from
// OpenProject JSON
void OpenProjectWorkPackage::parseTicketFields(const nlohmann::json &j) {
  if (j["id"] != nullptr)
    id = to_string(j["id"]);
  if (j[mApi.configCallId] != nullptr)
    callId = j[mApi.configCallId];
  if (j["subject"] != nullptr)
    title = j["subject"];
  if (j[mApi.configCallerNumber] != nullptr)
    callerNumber = j[mApi.configCallerNumber];
  if (j[mApi.configCalledNumber] != nullptr)
    calledNumber = j[mApi.configCalledNumber];
  if (j["_links"].contains("status") &&
      j["_links"]["status"].contains("href") &&
      j["_links"]["status"]["href"] != nullptr) {
    string statusHref = j["_links"]["status"]["href"];
    // Extract ID from href like "/api/v3/statuses/1"
    size_t lastSlash = statusHref.find_last_of('/');
    if (lastSlash != string::npos) {
      status = statusHref.substr(lastSlash + 1);
    }
  }
  if (j["_links"].contains("assignee") &&
      j["_links"]["assignee"].contains("title") &&
      j["_links"]["assignee"]["title"] != nullptr) {
    userInformation = j["_links"]["assignee"]["title"];
    size_t pos = userInformation.find(' ');
    if (pos != std::string::npos) {
      userInformation = userInformation.substr(0, pos);
    }
  }
  if (j[mApi.configCallStartTimestamp] != nullptr)
    callStartTimestamp = j[mApi.configCallStartTimestamp];
  if (j["createdAt"] != nullptr)
    createdAt = j["createdAt"];
  if (j["lockVersion"] != nullptr)
    lockVersion = to_string(j["lockVersion"]);
  if (j.contains("description") && j["description"].contains("raw") &&
      j["description"]["raw"] != nullptr)
    description = j["description"]["raw"];
}

// Helper: Get status priority for ticket selection
// Returns: 3 for New (highest priority), 2 for In Progress, 1 for Closed/other
// (lowest) Used to prefer active tickets when multiple matches exist
int OpenProjectWorkPackage::getStatusPriority(const string &statusId) const {
  if (statusId == mApi.configStatusNew) {
    return 3; // New has highest priority - ideal for adding new calls
  } else if (statusId == mApi.configStatusInProgress) {
    return 2; // In Progress is second priority - ticket is active
  } else {
    return 1; // Closed or other status has lowest priority
  }
}

// Helper: Determine if candidate ticket should replace current selection
// Returns true if candidate has higher priority status (New > In Progress >
// Closed) Used for ticket selection when API returns multiple matches
bool OpenProjectWorkPackage::shouldUpgradeTicket(
    const string &currentStatusId, const string &candidateStatusId) const {
  int currentPriority = getStatusPriority(currentStatusId);
  int candidatePriority = getStatusPriority(candidateStatusId);

  // Upgrade to candidate if it has higher priority status
  return candidatePriority > currentPriority;
}

// Parses OpenProject API response JSON to populate ticket data
// Handles multiple tickets in response by selecting best match based on status
// priority Priority logic: New > In Progress > Closed (prefer active tickets
// for incoming calls)
bool OpenProjectWorkPackage::getTicketFromJson(nlohmann::json &response) {
  logging::Logger::debug("getTicketFromJson(nlohmann::json& response) started");
  try {
    // Validate response structure
    if (!response.contains("_embedded") ||
        !response["_embedded"].contains("elements")) {
      logging::Logger::debug(
          "getTicketFromJson: No _embedded.elements found in response");
      return false;
    }

    auto &elements = response["_embedded"]["elements"];
    if (elements.empty()) {
      logging::Logger::debug(
          "getTicketFromJson: No tickets found in elements array");
      return false;
    }

    logging::Logger::debug("getTicketFromJson: Found " +
                           std::to_string(elements.size()) + " tickets");

    // Select best ticket from multiple results using status priority
    // Status priority: New (3) > In Progress (2) > Closed (1)
    nlohmann::json *selectedTicket = nullptr;
    string selectedTicketStatus = "";
    string selectedTicketId = "";

    for (auto &ticket : elements) {
      if (ticket.is_null())
        continue;

      string ticketId = ticket["id"].is_null()
                            ? "unknown"
                            : std::to_string((int)ticket["id"]);
      string statusId = "";

      // Extract status ID from href (e.g., "/api/v3/statuses/1" -> "1")
      if (ticket["_links"].contains("status") &&
          ticket["_links"]["status"].contains("href") &&
          !ticket["_links"]["status"]["href"].is_null()) {
        string statusHref = ticket["_links"]["status"]["href"];
        size_t lastSlash = statusHref.find_last_of('/');
        if (lastSlash != string::npos) {
          statusId = statusHref.substr(lastSlash + 1);
        }
      }

      logging::Logger::debug("getTicketFromJson: Examining ticket ID " +
                             ticketId + " with status " + statusId);

      // Select first ticket or upgrade to better status match
      if (!selectedTicket) {
        selectedTicket = &ticket;
        selectedTicketStatus = statusId;
        selectedTicketId = ticketId;
        logging::Logger::debug("getTicketFromJson: Selected first ticket ID " +
                               ticketId + " with status " + statusId);
      } else if (shouldUpgradeTicket(selectedTicketStatus, statusId)) {
        // Upgrade to ticket with higher priority status
        selectedTicket = &ticket;
        selectedTicketStatus = statusId;
        selectedTicketId = ticketId;
        logging::Logger::info("getTicketFromJson: Upgraded to ticket ID " +
                              ticketId + " with higher priority status " +
                              statusId);
      }
    }

    if (!selectedTicket) {
      logging::Logger::debug(
          "getTicketFromJson: No valid ticket found in elements");
      return false;
    }

    logging::Logger::info("getTicketFromJson: Using ticket ID " +
                          selectedTicketId + " with status " +
                          selectedTicketStatus);

    // Parse all fields from selected ticket
    parseTicketFields(*selectedTicket);

    logging::Logger::debug(
        "getTicketFromJson(nlohmann::json& response) was successfull.");
    return true;
  } catch (...) {
    logging::Logger::debug(
        "getTicketFromJson(nlohmann::json& response) failed");
    logging::Logger::error(
        "getTIcketFromJson(nlohmann::json& response) failed!");
    return false;
  }
}

// Helper: Add common ticket fields to JSON (id, callId, subject, phone numbers)
// Used by to_json to populate basic ticket properties
void OpenProjectWorkPackage::addBasicFieldsToJson(
    nlohmann::json &result) const {
  if (!id.empty())
    result["id"] = std::stoi(id);
  result[mApi.configCallId] = callId;
  if (!subject.empty())
    result["subject"] = subject;
  result[mApi.configCallerNumber] = callerNumber;
  result[mApi.configCalledNumber] = calledNumber;
}

// Helper: Build and add link objects for status, type, and assignee
// Links reference OpenProject API endpoints with href format
void OpenProjectWorkPackage::addLinksToJson(nlohmann::json &result) const {
  // Status link: /api/v3/statuses/{statusId}
  if (!fields.status.href.empty()) {
    nlohmann::json status_obj;
    status_obj["href"] = "/api/v3/statuses/" + fields.status.href;
    result["_links"]["status"] = status_obj;
  }

  // Type link: /api/v3/types/{typeId}
  if (!fields.type.href.empty()) {
    nlohmann::json type_obj;
    type_obj["href"] = "/api/v3/types/" + fields.type.href;
    result["_links"]["type"] = type_obj;
  }

  // Assignee link: direct user reference
  if (!fields.assignee.href.empty()) {
    nlohmann::json assignee_obj;
    assignee_obj["href"] = fields.assignee.href;
    result["_links"]["assignee"] = assignee_obj;
  }
}

// Converts WorkPackage object to JSON format for OpenProject API calls
// Fixed to properly handle status and type hrefs with correct ID values
// Enhanced to include all necessary fields for ticket creation and updates
nlohmann::json OpenProjectWorkPackage::to_json() {
  try {
    logging::Logger::debug("to_json() started\nCallId: " + callId);
    nlohmann::json result;

    // Add all basic ticket properties
    addBasicFieldsToJson(result);

    // Add API links for relations
    addLinksToJson(result);

    // Add timestamps for call tracking
    result[mApi.configCallStartTimestamp] = callStartTimestamp;
    result[mApi.configCallEndTimestamp] = callEndTimestamp;

    // Add optimistic locking version
    if (!lockVersion.empty()) {
      result["lockVersion"] = std::stoi(lockVersion);
    }

    // Add description with raw format
    nlohmann::json description_obj;
    description_obj["raw"] = description;
    result["description"] = description_obj;

    logging::Logger::debug("OpenProjectWorkPackage::to_json() successful " +
                           result.dump(2));
    return result;

  } catch (nlohmann::json::exception &e) {
    logging::Logger::debug("to_json nlohmann::json::exception failed: " +
                           string(e.what()));
  } catch (...) {
    logging::Logger::debug("to_json failed");
  }
  return 0;
}

// Converts Ticket object data to WorkPackage fields for API submission
// Maps ticket status to proper href format for OpenProject API compatibility
void OpenProjectWorkPackage::TicketToPackage() {
  subject = title;
  //	fields.assignee.title = userInformation;
  fields.status.href = status;

  if (!callEndTimestamp.empty())
    fields.addComment.raw = description;
}

string OpenProjectWorkPackage::getDateTimeNow() {
  stringstream ss;

  boost::local_time::time_zone_ptr zone(
      new boost::local_time::posix_time_zone("UTC"));
  boost::local_time::local_date_time ldt =
      boost::local_time::local_sec_clock::local_time(zone);

  ss << ldt << endl;
  string dateAsString = ss.str();

  return dateAsString;
}

bool OpenProjectWorkPackage::getCallLength() {

  callEndTimestamp = getDateTimeNow();

  boost::local_time::time_zone_ptr zone(
      new boost::local_time::posix_time_zone("UTC"));
  boost::local_time::local_date_time ldtOne =
      boost::local_time::local_sec_clock::local_time(zone);
  boost::local_time::local_date_time ldtTwo =
      boost::local_time::local_sec_clock::local_time(zone);

  stringstream first;
  stringstream second;
  first << callStartTimestamp;
  second << callEndTimestamp;
  first >> ldtOne;
  second >> ldtTwo;

  auto timeDiff = ldtTwo - ldtOne;
  first.clear();
  first << timeDiff;
  first.seekg(0);

  callLength = first.str();
  if (!callLength.empty())
    return true;
  else {
    logging::Logger::error("getCallLength failed.");
    return false;
  }
}

bool OpenProjectWorkPackage::setTicketForAcceptedCall(Call &call) {
  try {
    logging::Logger::debug("setTicketForAcceptedCall(Call& call) started");

    if (call.user.empty())
      return false;

    int userId;
    userId = mApi.getUserId(call.user);
    if (userId == -1) {
      logging::Logger::error(
          "setTicketForAcceptedCall: Failed to get user ID for: " + call.user);
      return false;
    }

    string newAssigneeHref = "/api/v3/users/" + to_string(userId);

    // Check if there's already an assignee different from the current user
    if (!fields.assignee.href.empty() &&
        fields.assignee.href != newAssigneeHref) {
      logging::Logger::info(
          "setTicketForAcceptedCall: Existing assignee found: " +
          fields.assignee.href + ", adding additional user: " + call.user +
          " (ID: " + to_string(userId) + ")");

      // OpenProject doesn't support multiple assignees directly, but we track
      // the change The last user to accept becomes the primary assignee
      logging::Logger::info(
          "setTicketForAcceptedCall: Updating assignee from " +
          fields.assignee.href + " to " + newAssigneeHref +
          " (multiple users involved in this call)");
    } else if (fields.assignee.href == newAssigneeHref) {
      logging::Logger::info("setTicketForAcceptedCall: User " + call.user +
                            " is already the assignee");
    } else {
      logging::Logger::info(
          "setTicketForAcceptedCall: Setting initial assignee to user ID " +
          to_string(userId) + " for user: " + call.user);
    }

    fields.assignee.href = newAssigneeHref;
    //		fields.status.href = "/api/v3/statuses/" +
    //mApi.configStatusInProgress;
    logging::Logger::debug("setTicketForAcceptedCall(Call& call) success");
    return true;
  } catch (exception &e) {
    logging::Logger::error("setTicketForAcceptedCall failed: " +
                           string(e.what()));
    return false;
  } catch (...) {
    logging::Logger::debug("setTicketForAcceptedCall(Call& call) failed!");
    return false;
  }
}

//==============================================================================
// Plugin Factory Functions (C Linkage)
//==============================================================================

/**
 * @brief Plugin factory function - creates OpenProjectWorkPackageApi instance
 *
 * C linkage requirements:
 * - extern "C" prevents name mangling for dynamic library loading
 * - Microkernel uses dlsym() to locate this function by name
 * - Function signature must match TicketSystem* (*)(nlohmann::json&)
 *
 * Plugin lifecycle:
 * 1. Microkernel loads libOpenProject.so via dlopen()
 * 2. Microkernel finds createTicketSystem() via dlsym()
 * 3. Microkernel calls createTicketSystem(config) to instantiate plugin
 * 4. Plugin pointer returned as TicketSystem* base class
 * 5. Microkernel uses plugin via TicketSystem interface methods
 *
 * @param config JSON configuration from config.json
 * @return Pointer to new OpenProjectWorkPackageApi instance (as TicketSystem*
 * base class)
 */
extern "C" {
TicketSystem *createTicketSystem(nlohmann::json &config) {
  return new OpenProjectWorkPackageApi(config);
}
}

/**
 * @brief DLL factory function - creates OpenProjectWorkPackage ticket instance
 *
 * Used internally by OpenProject plugin for ticket object creation.
 *
 * @param config JSON configuration
 * @param mApi Reference to API for ticket operations
 * @return Pointer to new OpenProjectWorkPackage instance (as Ticket* base
 * class)
 */
extern "C" {
Ticket *createTicketFromDll(nlohmann::json &config,
                            OpenProjectWorkPackageApi &mApi) {
  return new OpenProjectWorkPackage(config, mApi);
}
}
