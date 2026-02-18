/**
 * @file DaviCal.h
 * @brief DaviCal CardDAV integration header for address book lookups
 *
 * Architecture Overview:
 * - Implements AddressSystem interface for phone number lookups
 * - Connects to CardDAV servers (DaviCal, Nextcloud, etc.) for contact data
 * - Parses vCard (RFC 6350) format for contact information extraction
 * - Supports both direct dial and company address books
 *
 * CardDAV Protocol:
 * - Uses HTTP REPORT method with XML queries
 * - Supports text-match filters for phone number searches
 * - Returns vCard data in XML addressbook-query responses
 *
 * Integration Points:
 * - Called by CallController when incoming call detected
 * - Provides contact name, company, and project IDs for ticket creation
 * - Supports exact match (direct dial) and prefix match (company) lookups
 */
#pragma once
#include "Systems/AddressSystem.h"
#include "json.hpp"
#include "tinyxml.h"
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/cURLpp.hpp>
#include <regex>
#include <string>
#include <vector>

// Note: "using namespace std" kept for now - DaviCal needs refactoring
// TODO: Refactor DaviCal to use std:: prefix explicitly
using namespace std;

/**
 * @struct DaviCal
 * @brief DaviCal CardDAV client implementation for address lookups
 *
 * Responsibilities:
 * - Searches CardDAV address books for phone number matches
 * - Parses vCard (RFC 6350) contact data
 * - Extracts names, companies, and project associations
 * - Supports two-tier lookup: direct dial → company fallback
 *
 * Lookup Strategy:
 * 1. Try direct dial address book (exact phone match)
 * 2. If no match, try company address book (prefix match, last 5 digits
 * truncated)
 * 3. If multiple matches, select best match by longest common prefix
 *
 * vCard Field Mapping:
 * - FN (Full Name) → name
 * - ORG (Organization) → companyName
 * - TEL (Phone) → phoneNumbers[]
 * - X-CUSTOM1 (Custom) → projectIds[] (comma-separated OpenProject IDs)
 */
struct DaviCal : AddressSystem {
  /** @brief Default constructor - initializes empty DaviCal instance */
  DaviCal();

  /**
   * @brief Configuration constructor - loads CardDAV server settings
   * @param config JSON configuration with server URL, credentials, book paths
   */
  DaviCal(nlohmann::json &config);

  /** @brief Destructor - cleans up DaviCal resources */
  ~DaviCal();

  /**
   * @brief Main lookup function - searches CardDAV for phone number matches
   *
   * Lookup flow:
   * 1. Query direct dial address book (exact match)
   * 2. If no results, query company book (prefix match)
   * 3. If multiple results, select best match
   * 4. Parse vCard and populate addressInformation
   *
   * @param call Call object containing phone number to lookup
   * @param fillThis Output structure populated with contact data
   * @return true if contact found, false otherwise
   */
  bool getInformationByNumber(const Call &call,
                              addressInformation &fillThis) const override;

  /**
   * @brief Dashboard API endpoint - not implemented for CardDAV
   * @return Empty string (CardDAV doesn't provide dashboard data)
   */
  std::string getDashboardInformation(std::istream &payload,
                                      std::string &urlParams) override;

private:
  /**
   * @brief Normalizes phone number to international format
   *
   * Normalization rules:
   * - "0..." → "+49..." (German numbers)
   * - "+..." → unchanged (already international)
   * - "00..." → unchanged (international prefix)
   *
   * @param number Phone number to normalize
   * @return Normalized phone number in international format
   * @throws std::runtime_error if number too short (< 2 chars)
   */
  std::string normalizePhoneNumber(const std::string &number) const;

  /**
   * @enum AddressBookType
   * @brief Address book type selector for CardDAV queries
   */
  enum AddressBookType { EABT_COMPANIES, EABT_DIRECT_DIAL };

  /** @brief Raw vCard data from CardDAV responses (RFC 6350 format) */
  std::vector<std::string> vnCards;

  /** @brief Parsed vCard property maps (key→value) for processing */
  std::vector<std::map<std::string, std::string>> maps;

  /**
   * @brief Core CardDAV query - retrieves vCards matching phone number
   *
   * CardDAV workflow:
   * 1. Build XML query with phone number filter
   * 2. Send HTTP REPORT request to CardDAV server
   * 3. Parse XML response (multistatus → response → address-data)
   * 4. Extract vCard text from address-data elements
   *
   * @param call Call object with phone number
   * @param type Address book type (direct dial or companies)
   * @param fillThis Output structure (sets isCompany flag if company book used)
   * @return Vector of vCard strings from matching contacts
   */
  std::vector<std::string> getVnCards(const Call &call, AddressBookType type,
                                      addressInformation &fillThis) const;

  /**
   * @brief Queries direct dial address book with exact phone match
   *
   * CardDAV query details:
   * - Filter: TEL property equals normalized phone number
   * - Match type: "equals" (exact match)
   * - Depth: 1 (search one level deep)
   *
   * @param call Call object with phone number to search
   * @param directDialBookUrl CardDAV URL for direct dial address book
   * @return XML response stream with vCard data
   */
  std::stringstream
  getXmlOfAddressBook(const Call &call,
                      const std::string &directDialBookUrl) const;

  /**
   * @brief Queries company address book with phone prefix match
   *
   * Strategy:
   * - Truncates last 5 digits from phone number (removes extensions)
   * - Uses "starts-with" matching for company main numbers
   * - Example: +49111111122222 → search for +49111111*
   *
   * @param call Call object with phone number
   * @param companiesBookUrl CardDAV URL for company address book
   * @return XML response stream with vCard data
   */
  std::stringstream
  getXmlOfCompaniesBook(const Call &call,
                        const std::string &companiesBookUrl) const;

  /**
   * @brief Builds CardDAV XML query header with namespace declarations
   * @return XML string with addressbook-query structure and namespaces
   */
  std::string buildCardDavXmlHeader() const;

  /**
   * @brief Builds phone number filter XML for CardDAV query
   *
   * Truncates last 5 digits for extension-independent company matching
   *
   * @param phoneNumber Full phone number to filter
   * @return XML filter fragment with starts-with predicate
   */
  std::string buildPhoneNumberFilter(const std::string &phoneNumber) const;

  /**
   * @brief Parses vCard text to property multimap
   *
   * vCard format (RFC 6350):
   * - Property:Value format
   * - Property;Parameters:Value format (e.g., TEL;TYPE=work:+49...)
   * - Multiline values supported
   *
   * Extracted properties:
   * - FN: Full Name
   * - ORG: Organization
   * - TEL: Phone numbers (can be multiple)
   * - X-CUSTOM1: Custom field with project IDs
   *
   * @param card Raw vCard text from CardDAV response
   * @return Multimap of vCard properties to values
   */
  std::multimap<std::string, std::string>
  vnCardToMultiMap(const std::string &card) const;

  /**
   * @brief Parses comma-separated project IDs from X-CUSTOM1 field
   *
   * Processing:
   * - Split by comma delimiter
   * - Remove backslash escape characters
   * - Remove all whitespace
   * - Filter empty tokens
   *
   * @param ids Comma-separated project ID string
   * @return Vector of clean project ID strings
   */
  std::vector<std::string> splittIdsFromString(const std::string &ids) const;

  /**
   * @brief Transfers vCard data to addressInformation structure
   *
   * Field mapping:
   * - FN → name
   * - ORG → companyName
   * - TEL → phoneNumbers[]
   * - X-CUSTOM1 → projectIds[]
   *
   * @param map Parsed vCard property multimap
   * @param fillThis Output structure to populate
   */
  void multiMapToAddress(const std::multimap<std::string, std::string> &map,
                         addressInformation &fillThis) const;

  /**
   * @brief Selects best contact from multiple vCard matches
   *
   * Selection algorithm:
   * - If 1 match: use it
   * - If multiple: compare phone numbers, select longest common prefix
   *
   * @param call Call object with caller phone number
   * @param vnCards Vector of vCard strings from CardDAV
   * @param fillThis Output structure to populate with best match
   */
  void getBestAddressMatch(const Call &call, std::vector<std::string> &vnCards,
                           addressInformation &fillThis) const;

  /**
   * @brief Safely extracts configuration value with error handling
   *
   * @tparam T Value type (string, int, etc.)
   * @param config JSON configuration object
   * @param param Parameter name to extract
   * @param defaultVal Default value if missing
   * @param hasError Set to true if parameter missing
   * @return Extracted value or default
   */
  template <typename T>
  T getConfigValue(nlohmann::json &config, const char *param,
                   const T &defaultVal, bool &hasError);
};
