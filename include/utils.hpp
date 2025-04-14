#ifndef UTILS_HPP
#define UTILS_HPP

#include <string>
#include <ostream>
#include <iostream>

// ANSI color codes for console output.
#define COLOR_RESET "\033[0m"
#define COLOR_INFO  "\033[32m"
#define COLOR_WARN  "\033[33m"
#define COLOR_ERROR "\033[31m"

namespace Starpack {

/**
 * @brief Logs an informational message to standard error with green coloring.
 *
 * @param message The message to log.
 */
inline void log_message(const std::string &message)
{
    std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET << message << std::endl;
}

/**
 * @brief Logs a warning message to standard error with yellow coloring.
 *
 * @param message The warning message to log.
 */
inline void log_warning(const std::string &message)
{
    std::cerr << COLOR_WARN << "[WARN] " << COLOR_RESET << message << std::endl;
}

/**
 * @brief Logs an error message to standard error with red coloring.
 *
 * @param message The error message to log.
 */
inline void log_error(const std::string &message)
{
    std::cerr << COLOR_ERROR << "[ERROR] " << COLOR_RESET << message << std::endl;
}

// ---------------------------------------------------------------------------
// Other utility function declarations
// ---------------------------------------------------------------------------

/**
 * @brief libcurl write callback function.
 *
 * Appends data received from a libcurl request to a std::string.
 *
 * @param contents Pointer to the received data.
 * @param size Size of each element.
 * @param nmemb Number of elements.
 * @param userp Pointer to the std::string to append data to.
 * @return The total number of bytes processed.
 */
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);

/**
 * @brief Downloads repository data from a given URL.
 *
 * Uses libcurl to fetch the content at the specified URL and returns it as a string.
 *
 * @param url The URL from which to fetch repository data.
 * @return A string containing the fetched data.
 */
std::string fetchRepoData(const std::string& url);

/**
 * @brief Removes any text following the first slash or backslash in the input string.
 *
 * This function is useful for normalizing package names by stripping any
 * additional qualifiers appended via '/' or '\\'.
 *
 * @param input The input string, typically a package name.
 * @return A substring up to (but not including) the first slash/backslash.
 */
std::string removeSlashAndAfter(const std::string& input);

} // namespace Starpack

#endif // UTILS_HPP
