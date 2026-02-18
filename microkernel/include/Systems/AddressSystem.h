/**
 * @file AddressSystem.h
 * @brief Abstract base class for address/contact lookup systems
 *
 * Provides a plugin interface for different address systems (CardDAV, LDAP,
 * etc.) to look up caller information by phone number. Implementations
 * integrate with external contact management systems to retrieve caller name,
 * company, and associated project IDs.
 *
 * Key Responsibilities:
 * - Query address book by phone number
 * - Return caller information (name, company, phone numbers, project IDs)
 * - Identify if caller is a company or individual contact
 * - Provide dashboard data for UI display (optional)
 *
 * Plugin Architecture:
 * - This is an abstract base class with pure virtual methods
 * - Concrete implementations: CardDAV, LDAP, and other address book plugins
 * - Loaded dynamically via AddrSystemCreator and dlopen()
 * - Creator function signature: AddrSysCreator
 *
 * @see Plugin implementations in lib/ folder for concrete implementations
 * @see AddrSystemCreator Factory for creating AddressSystem instances
 * @see Call Model containing phone number for lookup
 *
 * @dependencies nlohmann/json (json.hpp)
 *
 */

#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "json.hpp"

// Forward declarations
struct Call;

/**
 * @class AddressSystem
 * @brief Abstract base class for address/contact lookup systems
 *
 * Defines the interface for querying address books by phone number.
 * Implementations connect to external systems (CardDAV, LDAP, etc.)
 * to retrieve caller information for incoming calls.
 *
 * Configuration:
 * Each implementation receives a JSON config object containing:
 * - addressSystemName: Plugin identifier (e.g., plugin name from config)
 * - bookAddresses: URL/path to individual contacts address book
 * - bookCompanies: URL/path to company contacts address book
 * - user: Authentication username
 * - password: Authentication password
 *
 * Lookup Flow:
 * 1. CallController receives incoming call with phone number
 * 2. Calls getInformationByNumber(call, addressInfo)
 * 3. Implementation searches address books (companies first, then individuals)
 * 4. Returns addressInformation struct with name, company, project IDs
 * 5. CallController uses project IDs to route ticket creation
 *
 * @note This is an abstract class - cannot be instantiated directly
 * @note All methods use explicit std:: namespace
 */
class AddressSystem {
public:
  /**
   * @struct addressInformation
   * @brief Container for caller information retrieved from address system
   *
   * Used to pass caller data from AddressSystem to CallController.
   * Contains all relevant information about the caller for ticket creation.
   */
  struct addressInformation {
    std::string name; ///< Caller name (individual or company contact name)
    std::string companyName; ///< Company name (empty for individual contacts)
    std::vector<std::string>
        phoneNumbers; ///< All phone numbers associated with contact
    std::vector<std::string>
        projectIds; ///< Ticket system project IDs linked to this contact
    bool isCompany =
        false; ///< True if this is a company contact, false for individual
  };

  /**
   * @brief Default constructor
   *
   * Does not initialize configuration. Used internally by derived classes.
   */
  AddressSystem() = default;

  /**
   * @brief Constructor with JSON configuration
   *
   * Loads base configuration parameters:
   * - addressSystemName: Plugin identifier
   * - bookAddresses: Individual contacts address book URL
   * - bookCompanies: Company contacts address book URL
   * - user: Authentication username
   * - password: Authentication password
   *
   * Derived classes should call this constructor in their initializer list.
   *
   * @param config JSON configuration object from config.json
   *
   * @note Logs warnings for missing config parameters via Logger
   * @note Sets hasError flag if any required parameters are missing
   */
  AddressSystem(nlohmann::json &config);

  /**
   * @brief Virtual destructor for proper cleanup of derived classes
   */
  virtual ~AddressSystem() = default;

  /**
   * @brief Look up caller information by phone number
   *
   * Pure virtual method - must be implemented by derived classes.
   *
   * Implementation should:
   * 1. Extract phone number from call object
   * 2. Search company address book first (higher priority)
   * 3. Search individual address book if not found in companies
   * 4. Fill addressInformation struct with results
   * 5. Return true if contact found, false otherwise
   *
   * @param call Call object containing phone number and call metadata
   * @param fillThis Output parameter - filled with caller information if found
   *
   * @return bool True if contact found and fillThis populated, false if not
   * found
   *
   * @note fillThis is modified only if contact is found
   * @note Phone numbers are normalized before lookup (implementation-specific)
   */
  virtual bool getInformationByNumber(const Call &call,
                                      addressInformation &fillThis) const = 0;

  /**
   * @brief Get dashboard information for UI display
   *
   * Virtual method with default implementation that returns empty string.
   * Override in derived classes to provide custom dashboard data.
   *
   * @param payload Input stream containing request payload (JSON)
   * @param urlParams URL parameters from dashboard request
   *
   * @return std::string JSON response for dashboard UI (empty by default)
   *
   * @note Default implementation returns empty string
   * @note Override if AddressSystem needs to provide UI data
   */
  virtual std::string getDashboardInformation(std::istream &payload,
                                              std::string &urlParams);

protected:
  // Configuration parameters loaded from config.json
  std::string configAddressSystemName; ///< Plugin identifier (e.g., plugin name
                                       ///< from config)
  std::string
      configBookDirectDial; ///< URL/path to individual contacts address book
  std::string
      configBookCompanies;    ///< URL/path to company contacts address book
  std::string configUser;     ///< Authentication username
  std::string configPassword; ///< Authentication password

  /**
   * @brief Template method to safely extract config values with defaults
   *
   * Attempts to extract a value from JSON config. If the parameter is missing,
   * logs a warning, sets the parameter to defaultVal in the config, and sets
   * hasError flag.
   *
   * @tparam T Type of config parameter (string, int, bool, etc.)
   * @param config JSON configuration object
   * @param param Parameter name to extract
   * @param defaultVal Default value if parameter is missing
   * @param hasError Output flag - set to true if parameter was missing
   *
   * @return T The extracted value or defaultVal if missing
   *
   * @throws None - catches nlohmann::json::exception and returns default
   *
   * @note Logs warning via Logger::warn if parameter is missing
   * @note Modifies config object by adding missing parameter with default value
   */
  template <typename T>
  T getConfigValue(nlohmann::json &config, const char *param,
                   const T &defaultVal, bool &hasError);
};

/**
 * @typedef AddrSysCreator
 * @brief Function pointer type for AddressSystem factory/creator functions
 *
 * Creator functions are exported from shared libraries (.so files) and called
 * by AddrSystemCreator to instantiate concrete AddressSystem implementations.
 *
 * Function Signature:
 * @code
 * extern "C" AddressSystem* create(nlohmann::json& config) {
 *     return new AddressSystemPlugin(config);
 * }
 * @endcode
 *
 * @param config JSON configuration object from config.json
 * @return AddressSystem* Pointer to newly created AddressSystem instance
 *
 * @note Must be declared with extern "C" to prevent name mangling
 * @note Returned pointer is owned by caller (microkernel manages lifecycle)
 */
typedef AddressSystem *(AddrSysCreator)(nlohmann::json &config);
