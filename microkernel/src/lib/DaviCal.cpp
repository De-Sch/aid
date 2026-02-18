/**
 * @file DaviCal.cpp
 * @brief DaviCal CardDAV implementation for phone number lookups
 *
 * Detailed Description:
 * This file implements the DaviCal plugin for CardDAV-based address lookups.
 * It provides phone number search functionality across CardDAV servers
 * (DaviCal, Nextcloud, etc.) with support for both direct dial and company
 * lookups.
 *
 * CardDAV Protocol Details:
 * - Uses HTTP REPORT method with XML addressbook-query requests
 * - Supports text-match filters with "equals" and "starts-with" predicates
 * - Parses XML responses containing vCard data (RFC 6350 format)
 * - Handles vCard properties: FN, ORG, TEL, X-CUSTOM1
 *
 * Lookup Strategy:
 * - Phase 1: Try direct dial book with exact phone match
 * - Phase 2: If no match, try company book with prefix match (truncate last 5
 * digits)
 * - Phase 3: If multiple matches, select best by longest common prefix
 *
 * Integration:
 * - Called by CallController during incoming call processing
 * - Provides contact data for ticket creation (name, company, projects)
 * - Supports German phone number normalization (0... → +49...)
 */
#include "DaviCal.h"
#include "Logger/Logger.h"
#include <Models/Call.h>

//==============================================================================
// Constructors and Destructor
//==============================================================================

/** @brief Default constructor - initializes empty DaviCal instance */
DaviCal::DaviCal() : AddressSystem() {}

/**
 * @brief Configuration constructor - loads CardDAV server settings
 *
 * Configuration parameters loaded from JSON:
 * - libPath: Path to shared library (for logging)
 * - addressSystemName: System identifier (for logging)
 * - Base class loads: configUser, configPassword, configBookDirectDial,
 * configBookCompanies
 *
 * Error handling:
 * - Missing parameters trigger error logging
 * - System continues with default values from getConfigValue()
 *
 * @param config JSON configuration from config.json
 */
DaviCal::DaviCal(nlohmann::json &config) : AddressSystem(config) {
  bool err = false;
  logging::Logger::info(
      "Try to load AddressSystem from: " +
      getConfigValue(config, "libPath", std::string(""), err) + "...");
  logging::Logger::info(
      "AddresSystem: " +
      getConfigValue(config, "addressSystemName", std::string(""), err) +
      " found and try to load.");
  // Config values now loaded by base AddressSystem constructor
  // configUser, configPassword, configBookDirectDial, configBookCompanies
  // loaded from config

  logging::Logger::info("Companies: " + configBookCompanies +
                        " Direct: " + configBookDirectDial);

  if (!err) {
    logging::Logger::info("AddressSystem loaded without issues.");
  } else {
    logging::Logger::error(
        "Missing Config values for AddressSystem, template has been written.");
  }
}

/** @brief Destructor - no special cleanup needed */
DaviCal::~DaviCal() {}

//==============================================================================
// Core Lookup Methods
//==============================================================================

/**
 * @brief Main lookup function - searches CardDAV for phone number matches
 *
 * Two-phase lookup strategy:
 * 1. Direct dial address book: exact phone number match
 * 2. Company address book: prefix match (if direct dial fails)
 *
 * Workflow:
 * 1. Query direct dial book with normalized phone number
 * 2. If empty, query company book (sets isCompany=true)
 * 3. If still empty, return false (no contact found)
 * 4. If matches found, select best match and populate addressInformation
 *
 * @param call Call object with phone number to lookup
 * @param fillThis Output structure populated with contact data (name, company,
 * projectIds)
 * @return true if contact found and populated, false otherwise
 */
bool DaviCal::getInformationByNumber(const Call &call,
                                     addressInformation &fillThis) const {
  logging::Logger::info("getInformationByNumber started");

  // Phase 1: Try direct dial address book (exact match)
  std::vector<std::string> vnCards =
      getVnCards(call, EABT_DIRECT_DIAL, fillThis);

  // Phase 2: Fallback to company address book (prefix match)
  if (vnCards.empty()) {
    vnCards = getVnCards(call, EABT_COMPANIES, fillThis);
    fillThis.isCompany = true;
  }

  // No contact found in either address book
  if (vnCards.empty())
    return false;

  // Phase 3: Select best match from results and populate
  getBestAddressMatch(call, vnCards, fillThis);
  logging::Logger::info("getInformationByNumber end successfully");
  return true;
}

//==============================================================================
// CardDAV Query Methods
//==============================================================================

/**
 * @brief Retrieves vCard data from CardDAV server by querying with phone number
 *
 * CardDAV XML response parsing:
 * 1. Navigate XML: multistatus → response elements
 * 2. For each response: propstat → prop → VC:address-data
 * 3. Extract vCard text from address-data element
 * 4. Collect all vCards into vector
 *
 * @param call Call object with phone number to search
 * @param type Address book type (EABT_DIRECT_DIAL or EABT_COMPANIES)
 * @param fillThis Output structure (sets isCompany flag if company book)
 * @return Vector of vCard strings from matching contacts
 */
std::vector<std::string>
DaviCal::getVnCards(const Call &call, AddressBookType type,
                    addressInformation &fillThis) const {
  logging::Logger::info("getVnCards started with the phonenumber: " +
                        call.phoneNumber);
  std::vector<std::string> vnCards;
  TiXmlDocument doc;
  doc.SetCondenseWhiteSpace(false);
  std::stringstream xml;
  if (type == EABT_DIRECT_DIAL)
    xml = getXmlOfAddressBook(call, configBookDirectDial);
  else
    xml = getXmlOfCompaniesBook(call, configBookCompanies);

  xml >> doc;
  TiXmlElement *root = doc.FirstChildElement("multistatus");
  TiXmlElement *child = root->FirstChildElement("response");

  // Navigate CardDAV XML: multistatus -> response -> propstat -> prop ->
  // address-data
  while (root) {
    if (child) {
      while (child) {
        TiXmlElement *addressData = child->FirstChildElement("propstat");
        if (addressData)
          addressData = addressData->FirstChildElement("prop");
        if (addressData) {
          // Extract vCard text from VC:address-data element
          addressData = addressData->FirstChildElement("VC:address-data");
          vnCards.push_back(addressData->GetText());
        }
        child = child->NextSiblingElement("response");
      }
    } else
      break;
    root = root->NextSiblingElement("multistatus");
  }
  logging::Logger::info("Vncards list size: " + std::to_string(vnCards.size()));
  if (vnCards.size() > 0)
    logging::Logger::info(vnCards[0]);
  logging::Logger::info("getVnCards succesfull");
  return vnCards;
}

//==============================================================================
// vCard Processing and Best Match Selection
//==============================================================================

/**
 * @brief Selects best contact from multiple vCard matches using longest prefix
 *
 * Selection algorithm:
 * 1. If only 1 match: use it directly
 * 2. If multiple matches: compare all phone numbers, select longest common
 * prefix
 *
 * Prefix matching logic:
 * - Compare caller number with each contact's phone numbers (TEL property)
 * - Find longest matching prefix (higher quality = better match)
 * - Select contact with highest quality match
 *
 * Example:
 * - Caller: +491111122222
 * - Contact A: +4911111222 (quality=12, full match)
 * - Contact B: +49111112 (quality=9, partial match)
 * - Result: Select Contact A (higher quality)
 *
 * @param call Call object with caller phone number
 * @param vnCards Vector of vCard strings from CardDAV (guaranteed non-empty)
 * @param fillThis Output structure populated with best match data
 */
void DaviCal::getBestAddressMatch(const Call &call, vector<string> &vnCards,
                                  addressInformation &fillThis) const {
  assert(!vnCards.empty() && "vncard should not be empty");
  string callerNumber = call.phoneNumber;

  // Single match - use it directly
  if (vnCards.size() == 1) {
    multiMapToAddress(vnCardToMultiMap(vnCards[0]), fillThis);
    return;
  }

  // Multiple matches - find best by longest common prefix
  vector<multimap<string, string>> maps;
  for (int i = 0; vnCards.size() > i; i++)
    maps.push_back(vnCardToMultiMap(vnCards[i]));

  int match = -1, quality = 0;
  for (int i = 0; maps.size() > i; i++) {
    auto itr =
        maps[i].equal_range("TEL"); // Get all phone numbers for this contact
    for (auto it = itr.first; it != itr.second; ++it) {
      string &tempNumber = it->second;
      int minLength = min(tempNumber.length(), callerNumber.length());
      // Check if contact phone starts with caller number (or vice versa)
      if (tempNumber == callerNumber.substr(0, minLength)) {
        // Prefer longer matches (better quality)
        if (quality < minLength) {
          quality = minLength;
          match = i;
        }
      }
    }
  }

  // Populate with best match if found
  if (match >= 0)
    multiMapToAddress(maps[match], fillThis);
  return;
}

std::string DaviCal::normalizePhoneNumber(const std::string &number) const {
  string pn = number;
  if (pn.length() < 2)
    throw runtime_error("invalid phone number, too short");
  if (pn[0] == '0' && pn[1] != '0') {
    pn = "+49" + pn.substr(1);
  }
  return pn;
}

// Constructs and executes CardDAV REPORT query for direct dial address book
// Uses exact match filtering (match-type="equals") for complete phone number
// lookup Sends HTTP REPORT request with CardDAV XML query to retrieve matching
// vCard data
stringstream
DaviCal::getXmlOfAddressBook(const Call &call,
                             const string &directDialBookUrl) const {
  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  stringstream responseStream;

  // CardDAV addressbook-query XML with exact phone number matching
  // TEL property filter ensures only contacts with matching phone numbers are
  // returned
  string requestXmlString =
      R"(<?xml version="1.0" encoding="utf-8" ?>
<C:addressbook-query xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav">
		<D:prop>
    <D:getetag/>
    <C:address-data>
    </C:address-data>
  </D:prop>
  <C:filter>
    <C:prop-filter name="TEL">
      <C:text-match collation="i;unicode-casemap"
                    match-type="equals"
      >)" +
      normalizePhoneNumber(call.phoneNumber) + R"(</C:text-match>
    </C:prop-filter>
  </C:filter>
</C:addressbook-query>)";

  // Set HTTP headers for CardDAV REPORT request
  list<string> headers;
  headers.push_back("Depth: 1"); // Search one level deep in address book
  headers.push_back(R"(Content-Type: text/xml; charset="utf-8")");
  stringstream outstream(requestXmlString);

  // Configure cURL request with CardDAV server credentials and XML payload
  request.setOpt(new curlpp::options::HttpHeader(headers));
  request.setOpt(new curlpp::options::Url(directDialBookUrl));
  request.setOpt(
      new curlpp::options::UserPwd(configUser + ":" + configPassword));
  request.setOpt(new curlpp::options::WriteStream(&responseStream));
  request.setOpt(new curlpp::options::CustomRequest(
      "REPORT")); // CardDAV uses REPORT method
  request.setOpt(new curlpp::options::PostFields(requestXmlString));
  request.setOpt(new curlpp::options::PostFieldSize(requestXmlString.length()));

  request.perform();
  responseStream.seekg(0); // Reset stream position for reading

  return responseStream;
}

// Helper: Build CardDAV XML query header and namespace declarations
// Returns XML string with standard CardDAV addressbook-query format
string DaviCal::buildCardDavXmlHeader() const {
  return R"(<?xml version="1.0" encoding="utf-8" ?>
<C:addressbook-query xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav">
		<D:prop>
    <D:getetag/>
    <C:address-data>
    </C:address-data>
  </D:prop>
)";
}

// Helper: Build phone number filter for CardDAV query using "starts-with"
// matching Truncates last 5 digits from phone number for company-level
// (extension-independent) lookup to match company main numbers
string DaviCal::buildPhoneNumberFilter(const string &phoneNumber) const {
  // Remove extension digits (last 5) for company main number matching
  string truncatedNumber = phoneNumber.substr(0, phoneNumber.size() - 5);
  logging::Logger::info("Phone number truncated - Before: " + phoneNumber +
                        " After: " + truncatedNumber);

  return R"(  <C:filter>
    <C:prop-filter name="TEL">
      <C:text-match collation="i;unicode-casemap"
                    match-type="starts-with"
      >)" +
         truncatedNumber + R"(</C:text-match>
    </C:prop-filter>
  </C:filter>
</C:addressbook-query>)";
}

// Constructs CardDAV query for company address book with prefix matching
// Truncates last 5 digits from phone number for broader company-level lookup
// Uses "starts-with" matching to find main company numbers that partially match
// caller
stringstream
DaviCal::getXmlOfCompaniesBook(const Call &call,
                               const std::string &companiesBookUrl) const {
  logging::Logger::info("getXmlOfCompaniesBook started");
  curlpp::Cleanup myCleanup;
  curlpp::Easy request;
  stringstream responseStream;

  // Build complete CardDAV XML query with header and phone filter
  string requestXmlString =
      buildCardDavXmlHeader() + buildPhoneNumberFilter(call.phoneNumber);

  // Configure CardDAV REPORT request with standard headers
  list<string> headers;
  headers.push_back("Depth: 1"); // Search one level deep in address book
  headers.push_back(R"(Content-Type: text/xml; charset="utf-8")");

  // Set up cURL request with CardDAV server authentication and XML payload
  request.setOpt(new curlpp::options::HttpHeader(headers));
  request.setOpt(new curlpp::options::Url(companiesBookUrl));
  request.setOpt(
      new curlpp::options::UserPwd(configUser + ":" + configPassword));
  request.setOpt(new curlpp::options::WriteStream(&responseStream));
  request.setOpt(new curlpp::options::CustomRequest(
      "REPORT")); // CardDAV uses REPORT method
  request.setOpt(new curlpp::options::PostFields(requestXmlString));
  request.setOpt(new curlpp::options::PostFieldSize(requestXmlString.length()));

  request.perform();
  responseStream.seekg(0); // Reset stream position for reading

  return responseStream;
}

// vCard parser - converts RFC 6350 vCard text format to key-value multimap
// Handles vCard properties like FN (Full Name), ORG (Organization), TEL (Phone)
// Processes complex vCard format with proper field separation and value
// extraction
multimap<string, string> DaviCal::vnCardToMultiMap(const string &card) const {

  multimap<string, string> map;
  istringstream stream(card);
  string line;
  string key;
  string value;

  // Parse vCard line by line, extracting property-value pairs
  while (getline(stream, line)) {
    // Remove Windows line endings (\r\n -> \n)
    if (line[line.length() - 1] == '\r')
      line.resize(line.length() - 1);

    // Skip vCard envelope markers - not actual properties
    if (line.find("END:VCARD") != string::npos)
      continue;

    if (line.find("BEGIN:VCARD") != string::npos)
      continue;

    // Find property-value separator (:)
    size_t seperaterPos = line.find(":");
    if (seperaterPos == string::npos)
      continue;

    // Handle vCard properties with parameters (e.g., "TEL;TYPE=work:+49123456")
    // Extract just the property name before semicolon if parameters exist
    size_t seperaterTwo = line.find(";");
    if (seperaterTwo != string::npos && seperaterTwo < seperaterPos) {
      key = line.substr(0, seperaterTwo); // "TEL" from "TEL;TYPE=work:value"

    } else
      key = line.substr(0, seperaterPos); // "FN" from "FN:John Doe"

    // Extract property value after colon separator
    auto semicolon = line.find(";", seperaterPos);
    if (semicolon != string::npos)
      value = line.substr(seperaterPos + 1, semicolon - seperaterPos - 1);
    else
      value = line.substr(seperaterPos + 1);
    map.insert({key, value});
  }
  return map;
}

// Project ID parser - splits comma-separated project IDs from X-CUSTOM1 vCard
// field Removes escape characters and whitespace from parsed IDs Used to
// extract OpenProject project associations from vCard custom fields
vector<string> DaviCal::splittIdsFromString(const string &ids) const {
  vector<string> result;
  stringstream stream(ids);
  string token;

  // Split by comma delimiter and clean each project ID
  while (getline(stream, token, ',')) {
    // Remove backslash escape characters from project IDs
    token.erase(remove(token.begin(), token.end(), '\\'), token.end());
    // Remove all spaces for clean project ID format
    token.erase(remove(token.begin(), token.end(), ' '), token.end());
    if (!token.empty())
      result.push_back(token);
  }
  return result;
}

// Converts vCard multimap data to AddressSystem contact fields
// Extracts full name (FN) and organization (ORG) from vCard properties
// Populates the name and companyName fields for ticket creation
void DaviCal::multiMapToAddress(const multimap<string, string> &map,
                                addressInformation &fillThis) const {
  auto itr = map.find("FN");
  if (itr != map.end())
    fillThis.name = itr->second;

  itr = map.find("ORG");
  if (itr != map.end())
    fillThis.companyName = itr->second;

  auto range = map.equal_range("TEL");
  for (auto it = range.first; it != range.second; ++it)
    fillThis.phoneNumbers.push_back(it->second);

  itr = map.find(
      "X-CUSTOM1"); // Custom field containing comma-separated project IDs
  if (itr != map.end()) {
    fillThis.projectIds = splittIdsFromString(itr->second);
  }
}

//==============================================================================
// Dashboard API (Not Implemented)
//==============================================================================

/**
 * @brief Dashboard API endpoint - not implemented for CardDAV
 *
 * Note: This method is defined in AddressSystem interface but not needed for
 * CardDAV. Dashboard data aggregation is handled by TicketSystem, not
 * AddressSystem.
 *
 * Commented out original design (kept for reference):
 * - Would parse incoming call payload JSON
 * - Would lookup caller information using getInformationByNumber
 * - Would format contact data (name, caller, projectIds, company) as JSON
 * - Would return structured JSON for web interface consumption
 *
 * @param payload Incoming request payload (unused)
 * @param urlParams URL parameters (unused)
 * @return Empty string (not implemented)
 */
string DaviCal::getDashboardInformation(std::istream &payload,
                                        std::string &urlParams) {
  return "";
}

//==============================================================================
// Private Helper Methods
//==============================================================================

/**
 * @brief Safely extracts configuration value with error handling
 *
 * Error recovery strategy:
 * - If parameter exists: return its value
 * - If parameter missing: set it to default in config, set error flag, return
 * default
 *
 * This allows config template generation for missing parameters while still
 * returning usable default values for continued operation.
 *
 * @tparam T Value type (string, int, etc.)
 * @param config JSON configuration object to read from
 * @param param Configuration parameter name to extract
 * @param defaultVal Default value to use if parameter missing
 * @param hasError Reference flag set to true if parameter was missing
 * @return Extracted value if found, defaultVal otherwise
 */
template <typename T>
T DaviCal::getConfigValue(nlohmann::json &config, const char *param,
                          const T &defaultVal, bool &hasError) {
  try {
    // Attempt to extract parameter value from JSON config
    return config[param];
  } catch (nlohmann::json::exception &e) {
    // If parameter missing or wrong type, set default value and flag error
    config[param] = defaultVal;
    hasError = true; // Signal caller that config value was missing
    return defaultVal;
  }
}

//==============================================================================
// Plugin Factory Function (C Linkage)
//==============================================================================

/**
 * @brief Plugin factory function - creates DaviCal instance for microkernel
 *
 * C linkage requirements:
 * - extern "C" prevents name mangling for dynamic library loading
 * - Microkernel uses dlsym() to locate this function by name
 * - Function signature must match AddressSystem* (*)(nlohmann::json&)
 *
 * Plugin lifecycle:
 * 1. Microkernel loads libDaviCal.so via dlopen()
 * 2. Microkernel finds createAddressSystem() via dlsym()
 * 3. Microkernel calls createAddressSystem(config) to instantiate plugin
 * 4. Plugin pointer returned as AddressSystem* base class
 * 5. Microkernel uses plugin via AddressSystem interface methods
 *
 * @param config JSON configuration from config.json
 * @return Pointer to new DaviCal instance (as AddressSystem* base class)
 */
extern "C" {
AddressSystem *createAddressSystem(nlohmann::json &config) {
  return new DaviCal(config);
}
}
