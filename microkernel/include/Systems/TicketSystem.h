/**
 * @file TicketSystem.h
 * @brief Abstract base class for ticket/work package management systems
 *
 * Provides a plugin interface for different ticketing systems (OpenProject,
 * Jira, etc.) to create, update, query, and close tickets for incoming phone
 * calls. The ticket system is the core backend integration that manages work
 * packages throughout the call lifecycle.
 *
 * Key Responsibilities:
 * - Create new tickets for incoming calls
 * - Update ticket status, assignee, and custom fields during call events
 * - Query tickets by call ID, phone number, or project
 * - Close tickets when calls are completed
 * - Provide dashboard data for UI display
 * - Manage call ID tracking (multiple calls per ticket support)
 *
 * Plugin Architecture:
 * - This is an abstract base class with 11 pure virtual methods
 * - Concrete implementations: Various ticket system plugins
 * - Examples: OpenProject, Jira, GitHub Issues, etc.
 * - Loaded dynamically via TicketSystemCreator and dlopen()
 * - Creator function signature: TicketSysCreator
 *
 * Call Lifecycle Integration:
 * 1. Ring Event: createNewTicket()  Ticket created with caller info
 * 2. Accepted Call: getUserHref(), saveTicket()  Assignee set, status =
 * in-progress
 * 3. Transfer Call: getUserHref(), saveTicket()  Assignee updated
 * 4. Hangup: closeTicket()  Duration calculated, status = closed
 *
 * @see Plugin implementations in lib/ folder for concrete implementations
 * @see TicketSystemCreator Factory for creating TicketSystem instances
 * @see CallController Main consumer of TicketSystem interface
 * @see Ticket Model for ticket data structure
 *
 * @dependencies nlohmann/json (json.hpp), AddressSystem, ConfigError
 *
 */

#pragma once

#include <string>
#include <vector>

#include "AddressSystem.h"
#include "ConfigError.h"
#include "json.hpp"

// Forward declarations
struct Ticket;
struct Call;

/**
 * @class TicketSystem
 * @brief Abstract base class for ticket/work package management systems
 *
 * Defines the interface for creating and managing tickets throughout the call
 * lifecycle. Implementations connect to external ticketing systems
 * (OpenProject, Jira, etc.) via their REST APIs.
 *
 * Configuration:
 * Each implementation receives a JSON config object containing:
 * - libPath: Path to shared library (.so file)
 * - baseUrl: API base URL (e.g., "http://api.example.com/api/v3/")
 * - apiToken: Authentication token
 * - user: User resource URL (HAL+JSON format)
 * - fieldCallId: Custom field name for Asterisk call ID
 * - fieldCallerNumber: Custom field name for caller phone number
 * - fieldCalledNumber: Custom field name for dialed number
 * - fieldCallStart: Custom field name for call start timestamp
 * - fieldCallEnd: Custom field name for call end timestamp
 * - statusNew: Status ID for newly created tickets
 * - statusInProgress: Status ID for accepted calls
 * - statusClosed: Status ID for completed calls
 * - unknownNumberSaveLocation: Project ID for unknown callers
 * - projectWebBaseUrl: Base URL for ticket web links
 *
 * Multi-Call Tracking:
 * Tickets can be associated with multiple call IDs (comma-separated) to handle:
 * - Call-backs (new call for existing ticket)
 * - Conference calls (multiple simultaneous calls)
 * - Call transfers (call ID changes during transfer)
 *
 * @note This is an abstract class - cannot be instantiated directly
 * @note All methods use explicit std:: namespace (no "using namespace std")
 * @note Implementations must handle API errors gracefully and throw exceptions
 */
class TicketSystem {
public:
  // Configuration parameters loaded from config.json
  std::string configLibPath;      ///< Path to plugin shared library (.so file)
  std::string configUrl;          ///< API base URL for ticketing system
  std::string configApiToken;     ///< Authentication token/API key
  std::string configUser;         ///< User resource URL (HAL+JSON format)
  std::string configCallId;       ///< Custom field name for call ID storage
  std::string configCallerNumber; ///< Custom field name for caller phone number
  std::string configCalledNumber; ///< Custom field name for dialed number
  std::string configStatusNew;    ///< Status ID for new tickets (unmapped)
  std::string configStatusInProgress; ///< Status ID for in-progress tickets
                                      ///< (call accepted)
  std::string configStatusClosed; ///< Status ID for closed tickets (call ended)
  std::string configUnknownNumberSaveLocation; ///< Project ID for unknown
                                               ///< caller tickets
  std::string configProjectWebBaseUrl; ///< Base URL for ticket web links
  std::string
      configCallStartTimestamp;       ///< Custom field name for call start time
  std::string configCallEndTimestamp; ///< Custom field name for call end time

  nlohmann::json uiJson; ///< Additional UI configuration data

  /**
   * @brief Default constructor
   *
   * Does not initialize configuration. Used internally by derived classes.
   */
  TicketSystem() = default;

  /**
   * @brief Constructor with JSON configuration
   *
   * Loads base configuration parameters used by all TicketSystem
   * implementations. Derived classes should call this constructor in their
   * initializer list.
   *
   * @param config JSON configuration object from config.json "TicketSystem"
   * section
   *
   * @note Logs warnings for missing config parameters via Logger
   * @note Sets hasError flag if any required parameters are missing
   */
  TicketSystem(nlohmann::json &config);

  /**
   * @brief Virtual destructor for proper cleanup of derived classes
   */
  virtual ~TicketSystem() = default;

  // ====================
  // Core Ticket Operations (Pure Virtual - Must Implement)
  // ====================

  /**
   * @brief Create a new ticket for an incoming call
   *
   * Pure virtual method - must be implemented by derived classes.
   *
   * Creates a new work package/ticket with:
   * - Subject: Caller name or phone number
   * - Description: Call details (time, number, etc.)
   * - Project: From addressInformation.projectIds or unknownNumberSaveLocation
   * - Custom fields: Call ID, caller number, dialed number, start timestamp
   * - Status: statusNew (unmapped to agent)
   *
   * @param info Caller information from AddressSystem lookup
   * @param call Call event object containing call metadata
   *
   * @return Ticket* Pointer to newly created ticket (NEVER nullptr)
   *
   * @throws std::runtime_error If ticket creation fails
   * @throws ConfigError If required config parameters are missing
   *
   * @note MUST return a valid Ticket pointer or throw exception
   * @note Caller is responsible for managing returned Ticket* memory
   */
  virtual Ticket *createNewTicket(const AddressSystem::addressInformation &info,
                                  const Call &call) = 0;

  /**
   * @brief Get user resource URL/href by username
   *
   * Looks up a user by name and returns their resource identifier (URL for
   * HAL+JSON APIs). Used to set ticket assignee when a call is accepted or
   * transferred.
   *
   * @param userName Agent name (e.g., "Max", "Anna")
   *
   * @return std::string User resource URL or ID
   *
   * @throws std::runtime_error If user lookup fails or user doesn't exist
   *
   * @note Return format depends on API (HAL+JSON: full URL, others: ID string)
   */
  virtual std::string getUserHref(std::string &userName) = 0;

  /**
   * @brief Save/update an existing ticket
   *
   * Persists changes to a ticket object back to the ticketing system API.
   * Updates fields like assignee, status, custom fields, description, etc.
   *
   * @param ticket Pointer to ticket with modified fields
   *
   * @return bool True if save succeeded, false otherwise
   *
   * @throws std::runtime_error If API request fails
   *
   * @note ticket pointer must be valid (not nullptr)
   */
  virtual bool saveTicket(Ticket *ticket) = 0;

  /**
   * @brief Move ticket to a different project
   *
   * Changes the project/parent of a ticket. Used when caller information
   * is updated or corrected after initial ticket creation.
   *
   * @param ticket Pointer to ticket to move
   *
   * @return bool True if move succeeded, false otherwise
   *
   * @throws std::runtime_error If API request fails
   *
   * @note Not all ticketing systems support moving tickets
   */
  virtual bool moveTicket(Ticket *ticket) = 0;

  /**
   * @brief Close a ticket with specified status
   *
   * Sets ticket status to closed/completed. Called when a call ends (hangup
   * event). May also update custom fields like call end timestamp and duration.
   *
   * @param ticket Pointer to ticket to close
   * @param status Status ID to set (usually configStatusClosed)
   *
   * @return bool True if close succeeded, false otherwise
   *
   * @throws std::runtime_error If API request fails
   */
  virtual bool closeTicket(Ticket *ticket, const std::string &status) = 0;

  /**
   * @brief Get dashboard information for UI display
   *
   * Retrieves ticket data for a specific user's dashboard view.
   * Returns JSON formatted data for the SvelteKit frontend.
   *
   * @param payload Input stream containing request payload (JSON)
   * @param urlParams URL parameters from dashboard request (e.g., username)
   *
   * @return std::string JSON response for dashboard UI
   *
   * @throws std::runtime_error If data retrieval fails
   */
  virtual std::string getDashboardInformation(std::istream &payload,
                                              std::string &urlParams) = 0;

  // ====================
  // Ticket Query Methods (Pure Virtual - Must Implement)
  // ====================

  /**
   * @brief Find ticket by exact call ID match
   *
   * Searches for a ticket with the specified call ID in its custom field.
   * Used to find existing tickets for call-back scenarios.
   *
   * @param callId Asterisk call ID to search for
   *
   * @return Ticket* Pointer to found ticket, or nullptr if not found
   *
   * @note Returns nullptr if no matching ticket found (does NOT throw)
   */
  virtual Ticket *getTicketByCallId(std::string &callId) = 0;

  /**
   * @brief Find ticket by partial call ID match (contains)
   *
   * Searches for a ticket where the call ID field contains the specified
   * string. Useful for multi-call tickets where call IDs are comma-separated.
   *
   * @param callId Call ID substring to search for
   *
   * @return Ticket* Pointer to found ticket, or nullptr if not found
   */
  virtual Ticket *getTicketByCallIdContains(std::string &callId) = 0;

  /**
   * @brief Find ticket by ticket ID
   *
   * Retrieves a specific ticket by its unique identifier.
   *
   * @param id Ticket ID (work package ID, issue number, etc.)
   *
   * @return Ticket* Pointer to found ticket, or nullptr if not found
   */
  virtual Ticket *getTicketById(std::string &id) = 0;

  /**
   * @brief Find ticket by caller phone number
   *
   * Searches for tickets with the specified phone number in caller field.
   * Used to find recent tickets from the same caller.
   *
   * @param phoneNumber Phone number to search for
   *
   * @return Ticket* Pointer to found ticket, or nullptr if not found
   */
  virtual Ticket *getTicketByPhoneNumber(std::string &phoneNumber) = 0;

  /**
   * @brief Get the most recent call ticket in a project
   *
   * Finds the latest ticket (by creation date) in the specified project
   * that has a call ID field populated.
   *
   * @param projectId Project ID to search in
   *
   * @return Ticket* Pointer to latest ticket, or nullptr if no tickets found
   */
  virtual Ticket *
  getLatestCallTicketInProject(const std::string &projectId) const = 0;

  /**
   * @brief Get the most recent ticket in a project with specific name
   *
   * Searches for the latest ticket (by creation date) in the specified project
   * with a subject matching the given ticket name.
   *
   * @param projectId Project ID to search in
   * @param ticketName Subject/title to match
   *
   * @return Ticket* Pointer to latest matching ticket, or nullptr if not found
   */
  virtual Ticket *
  getLatestTicketInProjectByName(const std::string &projectId,
                                 const std::string &ticketName) const = 0;

  /**
   * @brief Find an open/running ticket by name
   *
   * Searches for non-closed tickets with a subject matching the given name.
   * Used to find active tickets for ongoing calls.
   *
   * @param name Subject/title to search for
   *
   * @return Ticket* Pointer to found ticket, or nullptr if not found
   */
  virtual Ticket *getRunningTicketByName(std::string &name) = 0;

  /**
   * @brief Get list of current/active tickets
   *
   * Retrieves all non-closed tickets, typically for dashboard display.
   *
   * @return std::string JSON array of ticket objects
   */
  virtual std::string getCurrentTickets() = 0;

  /**
   * @brief Get assignee display name for a ticket
   *
   * Extracts and returns the readable name of the ticket assignee.
   *
   * @param ticket Pointer to ticket
   *
   * @return std::string Assignee name (e.g., "Max Mueller")
   */
  virtual std::string getAssigneeTitle(Ticket *ticket) = 0;

  /**
   * @brief Check if a user exists in the ticketing system
   *
   * Verifies that a user account exists before attempting to assign tickets.
   *
   * @param name Username to check
   *
   * @return bool True if user exists, false otherwise
   */
  virtual bool checkIfUserExists(const std::string &name) const = 0;

  // ====================
  // Call ID Management (Virtual - Can Override)
  // ====================

  /**
   * @brief Format a call ID for storage
   *
   * Adds formatting to a call ID (default: appends ", " for comma-separated
   * lists). Override to customize call ID formatting.
   *
   * @param callId Raw call ID from Asterisk
   *
   * @return std::string Formatted call ID
   *
   * @note Default implementation appends ", " to the call ID
   */
  virtual std::string formatCallId(const std::string &callId);

  /**
   * @brief Add a call ID to an existing comma-separated list
   *
   * Appends a new call ID to the existing call IDs string, avoiding duplicates.
   * Used for multi-call ticket tracking.
   *
   * @param existingCallIds Reference to existing call IDs string (modified
   * in-place)
   * @param newCallId New call ID to add
   *
   * @note Modifies existingCallIds in-place
   * @note Checks for duplicates before adding
   * @note Handles empty existingCallIds correctly
   */
  virtual void addCallIdToExisting(std::string &existingCallIds,
                                   const std::string &newCallId);

  /**
   * @brief Remove a specific call ID from a comma-separated list
   *
   * Removes the specified call ID from the existing call IDs string.
   * Used when a call is moved to a different ticket.
   *
   * @param existingCallIds Reference to existing call IDs string (modified
   * in-place)
   * @param callIdToRemove Call ID to remove from the list
   *
   * @note Modifies existingCallIds in-place
   * @note Handles whitespace trimming
   * @note Maintains proper comma-separated format
   */
  virtual void removeCallIdFromExisting(std::string &existingCallIds,
                                        const std::string &callIdToRemove);

protected:
  /**
   * @brief Template method to safely extract config values with defaults
   *
   * Attempts to extract a value from JSON config. If the parameter is missing,
   * logs a warning, sets the parameter to defaultVal in the config, and sets
   * hasError flag.
   *
   * @tparam T Type of config parameter (std::string, int, bool, etc.)
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
 * @typedef TicketSysCreator
 * @brief Function pointer type for TicketSystem factory/creator functions
 *
 * Creator functions are exported from shared libraries (.so files) and called
 * by TicketSystemCreator to instantiate concrete TicketSystem implementations.
 *
 * Function Signature:
 * @code
 * extern "C" TicketSystem* create(nlohmann::json& config) {
 *     return new TicketSystemPlugin(config);
 * }
 * @endcode
 *
 * @param config JSON configuration object from config.json
 * @return TicketSystem* Pointer to newly created TicketSystem instance
 *
 * @note Must be declared with extern "C" to prevent name mangling
 * @note Returned pointer is owned by caller (microkernel manages lifecycle)
 */
typedef TicketSystem *(TicketSysCreator)(nlohmann::json &config);
