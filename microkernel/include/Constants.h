#ifndef CONSTANTS_H
#define CONSTANTS_H

namespace Constants {
// Call Duration Constants
// Default values and rounding intervals for call duration calculation
namespace CallDuration {
constexpr int DEFAULT_MINUTES = 15;
constexpr int ROUNDING_INTERVAL = 15;
constexpr int ROUNDING_OFFSET =
    14; // Used in rounding calculation: (duration + 14) / 15
} // namespace CallDuration

// HTTP Routes
// URL path prefixes for routing requests to appropriate controllers
namespace Routes {
constexpr const char *UI = "/ui";
constexpr const char *CALL = "/call";
constexpr int UI_PREFIX_LENGTH = 3;
constexpr int CALL_PREFIX_LENGTH = 5;
} // namespace Routes

// Ticket Status
// Ticket status identifiers used in status transitions
namespace TicketStatus {
constexpr const char *CLOSED = "Closed";
}

// Comment Formatting
// Patterns used for parsing and generating call comments in ticket descriptions
namespace CommentFormat {
constexpr const char *CALL_START_PREFIX = ": Call start: ";
constexpr const char *CALL_START_PATTERN = ": Call start:";
} // namespace CommentFormat

// System Configuration
// Timeouts and buffer sizes for CGI input handling and system operations
namespace SystemConfig {
constexpr int POLL_TIMEOUT_MS = 100;
constexpr int INPUT_BUFFER_SIZE = 1024;
} // namespace SystemConfig

// HTTP Status Codes
// Standard HTTP response codes returned by the CGI application
namespace HttpStatus {
constexpr int BAD_REQUEST = 400;
constexpr int INTERNAL_SERVER_ERROR = 500;
} // namespace HttpStatus
} // namespace Constants

#endif // CONSTANTS_H
