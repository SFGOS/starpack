#include "utils.hpp"
#include <curl/curl.h>
#include <stdexcept>
#include <iostream>

namespace Starpack {

/**
 * @brief libcurl callback function. Appends downloaded data into a std::string.
 */
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    size_t totalSize      = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);

    try {
        response->append(static_cast<char*>(contents), totalSize);
    } catch (const std::exception& e) {
        std::cerr << "Error appending data to response: "
                  << e.what() << std::endl;
        return 0; // Signal failure to libcurl
    }

    return totalSize;
}

/**
 * @brief Fetches repository data from a given URL using libcurl. Returns
 *        the response as a string. Throws on error.
 */
std::string fetchRepoData(const std::string& url)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize libcurl");
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        throw std::runtime_error(
            "Failed to fetch repository data: " +
            std::string(curl_easy_strerror(res))
        );
    }

    curl_easy_cleanup(curl);
    return response;
}

/**
 * @brief Returns a substring of `input` up to the first slash ('/') or backslash ('\'),
 *        whichever appears first. If no slashes, returns the entire input.
 */
std::string removeSlashAndAfter(const std::string& input)
{
    size_t posSlash     = input.find('/');
    size_t posBackslash = input.find('\\');
    size_t pos          = std::string::npos;

    // Choose whichever slash/backslash is encountered first
    if (posSlash != std::string::npos) {
        pos = posSlash;
    }
    if (posBackslash != std::string::npos) {
        if (pos == std::string::npos || posBackslash < pos) {
            pos = posBackslash;
        }
    }

    if (pos != std::string::npos) {
        return input.substr(0, pos);
    }
    return input;
}

} // namespace Starpack
