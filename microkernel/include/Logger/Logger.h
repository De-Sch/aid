/**
 * @file Logger.h
 * @brief Thread-safe singleton logging system for backend and frontend logs
 *
 * Provides a centralized logging facility with multiple severity levels (TRACE
 * to FATAL) and separate log files for backend and frontend components. The
 * logger is designed to work in both CGI and standalone execution environments.
 *
 * Key Features:
 * - Singleton pattern ensures single instance across microkernel
 * - Thread-safe log writing with mutex protection
 * - Configurable log levels and file paths via JSON config
 * - Separate backend.log and frontend.log files
 * - Automatic log directory creation
 * - Immediate flush for CGI compatibility
 *
 * Usage:
 * @code
 * // Initialize once at startup
 * Logger::initialize("/path/to/config.json");
 *
 * // Use throughout application
 * Logger::info("[Component] Operation succeeded");
 * Logger::error("[Component] Operation failed: reason");
 * @endcode
 *
 * @dependencies nlohmann/json (json.hpp)
 *
 */

#pragma once

#include "json.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace logging {

/**
 * @enum LogLevel
 * @brief Severity levels for log messages
 *
 * Levels are ordered from most verbose (TRACE) to most severe (FATAL).
 * Setting the global log level filters out less severe messages.
 */
enum class LogLevel {
  TRACE = 0, ///< Detailed trace information for debugging
  DEBUG = 1, ///< Debugging information
  INFO = 2,  ///< Informational messages
  WARN = 3,  ///< Warning messages for potentially harmful situations
  ERROR = 4, ///< Error messages for failures that may be recoverable
  FATAL = 5  ///< Fatal errors that may cause system shutdown
};

/**
 * @enum LogType
 * @brief Target log file for a message
 */
enum class LogType {
  BACKEND, ///< Write to backend.log (C++ microkernel operations)
  FRONTEND ///< Write to frontend.log (UI/JavaScript operations)
};

/**
 * @class Logger
 * @brief Thread-safe singleton logger with separate backend/frontend logs
 *
 * Architecture:
 * - Singleton pattern prevents multiple instances
 * - Static interface (Logger::info()) for convenient access
 * - Thread-safe log writing with std::mutex
 * - Designed for CGI environment (immediate flush after each write)
 *
 * Configuration:
 * Loads settings from config.json:
 * - logLevel: Filter level (TRACE/DEBUG/INFO/WARN/ERROR/FATAL)
 * - paths.baseDir: Directory for log files
 * - paths.backendLog.fullPath: Full path to backend.log
 * - paths.frontendLog.fullPath: Full path to frontend.log
 *
 * Thread Safety:
 * All write operations are protected by mutex_. Safe to call from multiple
 * threads.
 *
 * @note Logger is header-only (no .cpp file) for simplicity
 */
class Logger {
public:
  /**
   * @brief Log a message with specified severity and type
   *
   * @param level Severity level of the message
   * @param message Log message content
   * @param type Target log file (BACKEND or FRONTEND)
   *
   * @note Messages below the configured log level are silently ignored
   * @note This method is thread-safe
   */
  static void log(LogLevel level, const std::string &message,
                  LogType type = LogType::BACKEND) {
    getInstance().writeLog(level, message, type);
  }

  /**
   * @brief Set the global minimum log level
   *
   * Only messages at or above this level will be written to log files.
   *
   * @param level New minimum log level
   */
  static void setLogLevel(LogLevel level) { getInstance().logLevel = level; }

  /**
   * @brief Get the current global log level
   *
   * @return LogLevel Current minimum log level
   */
  static LogLevel getLogLevel() { return getInstance().logLevel; }

  /**
   * @brief Get the singleton Logger instance
   *
   * Creates the instance on first call (lazy initialization).
   * Uses Meyer's Singleton pattern for thread-safe initialization.
   *
   * @return Logger& Reference to the singleton instance
   */
  static Logger &getInstance() {
    static Logger instance;
    return instance;
  }

  /**
   * @brief Initialize logger with configuration file
   *
   * Must be called once at application startup before logging.
   * Loads log level, file paths, and creates necessary directories.
   *
   * @param configPath Path to config.json file
   *
   * @note If file doesn't exist or is invalid, defaults are used
   * @note Creates log directories if they don't exist
   */
  static void initialize(const std::string &configPath) {
    getInstance().loadConfig(configPath);
  }

  // Convenience methods for common log levels

  /**
   * @brief Log a TRACE level message
   *
   * @param message Log message
   * @param type Target log file (default: BACKEND)
   */
  static void trace(const std::string &message,
                    LogType type = LogType::BACKEND) {
    log(LogLevel::TRACE, message, type);
  }

  /**
   * @brief Log a DEBUG level message
   *
   * @param message Log message
   * @param type Target log file (default: BACKEND)
   */
  static void debug(const std::string &message,
                    LogType type = LogType::BACKEND) {
    log(LogLevel::DEBUG, message, type);
  }

  /**
   * @brief Log an INFO level message
   *
   * @param message Log message
   * @param type Target log file (default: BACKEND)
   */
  static void info(const std::string &message,
                   LogType type = LogType::BACKEND) {
    log(LogLevel::INFO, message, type);
  }

  /**
   * @brief Log a WARN level message
   *
   * @param message Log message
   * @param type Target log file (default: BACKEND)
   */
  static void warn(const std::string &message,
                   LogType type = LogType::BACKEND) {
    log(LogLevel::WARN, message, type);
  }

  /**
   * @brief Log an ERROR level message
   *
   * @param message Log message
   * @param type Target log file (default: BACKEND)
   */
  static void error(const std::string &message,
                    LogType type = LogType::BACKEND) {
    log(LogLevel::ERROR, message, type);
  }

  /**
   * @brief Log a FATAL level message
   *
   * @param message Log message
   * @param type Target log file (default: BACKEND)
   */
  static void fatal(const std::string &message,
                    LogType type = LogType::BACKEND) {
    log(LogLevel::FATAL, message, type);
  }

private:
  /**
   * @brief Private constructor for singleton pattern
   *
   * Initializes default log paths:
   * - baseDir: ./logs
   * - backendFullPath: ./logs/backend.log
   * - frontendFullPath: ./logs/frontend.log
   *
   * Actual paths are overridden by loadConfig() during initialization.
   */
  Logger() : logLevel(LogLevel::INFO) {
    // Default initialization
    paths.baseDir = "./logs";
    paths.backendFullPath = paths.baseDir + "/backend.log";
    paths.frontendFullPath = paths.baseDir + "/frontend.log";
  }

  /**
   * @brief Load configuration from JSON file
   *
   * Parses config.json and extracts Logger section:
   * - logLevel: String or numeric level
   * - paths.baseDir: Log directory
   * - paths.backendLog.fullPath: Backend log file path
   * - paths.frontendLog.fullPath: Frontend log file path
   *
   * Creates log directories and initializes log files.
   *
   * @param configPath Path to configuration file
   *
   * @note If config file is missing/invalid, uses defaults
   * @note Creates missing directories with permissions 0755
   */
  void loadConfig(const std::string &configPath) {
    std::ifstream configFile(configPath);
    if (configFile.is_open()) {
      nlohmann::json config;
      configFile >> config;

      if (config.contains("Logger")) {
        auto loggerConfig = config["Logger"];

        // Set log level if specified
        if (loggerConfig.contains("logLevel")) {
          std::string levelStr = loggerConfig["logLevel"];
          if (levelStr == "TRACE")
            logLevel = LogLevel::TRACE;
          else if (levelStr == "DEBUG")
            logLevel = LogLevel::DEBUG;
          else if (levelStr == "INFO")
            logLevel = LogLevel::INFO;
          else if (levelStr == "WARN")
            logLevel = LogLevel::WARN;
          else if (levelStr == "ERROR")
            logLevel = LogLevel::ERROR;
          else if (levelStr == "FATAL")
            logLevel = LogLevel::FATAL;
        } else if (loggerConfig.contains("level")) {
          // Support numeric levels for backward compatibility
          int levelNum = loggerConfig["level"];
          logLevel = static_cast<LogLevel>(levelNum);
        }

        // Load paths configuration
        if (loggerConfig.contains("paths")) {
          auto pathsConfig = loggerConfig["paths"];
          if (pathsConfig.contains("baseDir")) {
            paths.baseDir = pathsConfig["baseDir"];
          }
          if (pathsConfig.contains("backendLog") &&
              pathsConfig["backendLog"].contains("fullPath")) {
            paths.backendFullPath = pathsConfig["backendLog"]["fullPath"];
          } else {
            paths.backendFullPath = paths.baseDir + "/backend.log";
          }
          if (pathsConfig.contains("frontendLog") &&
              pathsConfig["frontendLog"].contains("fullPath")) {
            paths.frontendFullPath = pathsConfig["frontendLog"]["fullPath"];
          } else {
            paths.frontendFullPath = paths.baseDir + "/frontend.log";
          }
        }
      }

      // Create directories if they don't exist
      struct stat info;
      if (stat(paths.baseDir.c_str(), &info) != 0) {
        // Directory doesn't exist, create it
        mkdir(paths.baseDir.c_str(), 0755);
      }

      // Initialize log files
      initializeLogFiles();
    }
  }

  /**
   * @brief Open log file streams for backend and frontend logs
   *
   * Opens files in append mode. Logs errors to stderr if files cannot be
   * opened.
   *
   * @note Called after configuration is loaded
   * @note Files remain open for the lifetime of the Logger instance
   */
  void initializeLogFiles() {
    // Open backend log
    backendLog_.open(paths.backendFullPath, std::ios::app);
    if (!backendLog_.is_open()) {
      std::cerr << "Error opening backend log file: " << paths.backendFullPath
                << std::endl;
    }

    // Open frontend log
    frontendLog_.open(paths.frontendFullPath, std::ios::app);
    if (!frontendLog_.is_open()) {
      std::cerr << "Error opening frontend log file: " << paths.frontendFullPath
                << std::endl;
    }
  }

  /**
   * @brief Write a log message to the appropriate file
   *
   * Thread-safe implementation that:
   * 1. Acquires mutex lock
   * 2. Checks if message level meets threshold
   * 3. Formats message with timestamp and level
   * 4. Writes to appropriate log file
   * 5. Flushes immediately (for CGI compatibility)
   *
   * Format: "YYYY-MM-DD HH:MM:SS [LEVEL] message"
   *
   * @param level Message severity level
   * @param message Log message content
   * @param type Target log file (BACKEND or FRONTEND)
   *
   * @note This method is thread-safe (protected by mutex)
   * @note Messages below logLevel are silently ignored
   * @note Immediate flush ensures logs are written even if process crashes
   */
  void writeLog(LogLevel level, const std::string &message, LogType type) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (level >= logLevel) {
      auto now = std::chrono::system_clock::now();
      auto time = std::chrono::system_clock::to_time_t(now);

      std::ofstream &logFile =
          (type == LogType::BACKEND) ? backendLog_ : frontendLog_;

      logFile << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
              << " [" << getLogLevelString(level) << "] " << message
              << std::endl;
      logFile.flush(); // Force immediate write for CGI environments

      // UI communication uses proper structured JSON responses via stdout
      // This eliminates the brittle dependency on logger messages for UI
      // success detection
    }
  }

  /**
   * @struct Paths
   * @brief Configuration paths for log files
   */
  struct Paths {
    std::string baseDir;          ///< Base directory for all logs
    std::string backendFullPath;  ///< Full path to backend.log
    std::string frontendFullPath; ///< Full path to frontend.log
  };

  /**
   * @brief Convert LogLevel enum to string representation
   *
   * @param level Log level to convert
   * @return std::string String representation (e.g., "INFO ", "ERROR")
   *
   * @note Strings are padded to 5 characters for alignment in log files
   */
  std::string getLogLevelString(LogLevel level) const {
    switch (level) {
    case LogLevel::TRACE:
      return "TRACE";
    case LogLevel::DEBUG:
      return "DEBUG";
    case LogLevel::INFO:
      return "INFO ";
    case LogLevel::WARN:
      return "WARN ";
    case LogLevel::ERROR:
      return "ERROR";
    case LogLevel::FATAL:
      return "FATAL";
    default:
      return "UNKNOWN";
    }
  }

  std::mutex mutex_;          ///< Mutex for thread-safe log writing
  LogLevel logLevel;          ///< Current minimum log level threshold
  std::ofstream backendLog_;  ///< Output stream for backend.log
  std::ofstream frontendLog_; ///< Output stream for frontend.log
  Paths paths;                ///< Configured log file paths
};

} // namespace logging

/**
 * @section config_example Configuration Example
 *
 * Example Logger configuration in config.json:
 *
 * @code{.json}
 * {
 *   "Logger": {
 *     "logLevel": "INFO",
 *     "paths": {
 *       "baseDir": "/test/logs",
 *       "backendLog": {
 *         "filename": "backend.log",
 *         "fullPath": "/test/logs/backend.log"
 *       },
 *       "frontendLog": {
 *         "filename": "frontend.log",
 *         "fullPath": "/test/logs/frontend.log"
 *       }
 *     },
 *     "fileSettings": {
 *       "backendLog": {
 *         "filename": "backend.log",
 *         "rotation": {
 *           "enabled": true,
 *           "maxSize": "10MB",
 *           "maxFiles": 5
 *         }
 *       },
 *       "frontendLog": {
 *         "filename": "frontend.log",
 *         "rotation": {
 *           "enabled": true,
 *           "maxSize": "10MB",
 *           "maxFiles": 5
 *         }
 *       }
 *     },
 *     "format": {
 *       "timestamp": true,
 *       "level": true,
 *       "message": true,
 *       "type": true
 *     },
 *     "sampling": {
 *       "enabled": false,
 *       "rate": 1.0
 *     }
 *   }
 * }
 * @endcode
 *
 * @note fileSettings, format, and sampling are reserved for future features
 * @note Only logLevel and paths are currently used
 */
