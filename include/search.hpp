#ifndef SEARCH_HPP
#define SEARCH_HPP

#include <string>
#include <vector>

namespace Starpack {

/**
 * @class Search
 * @brief Provides search functionality for packages based on name, version,
 *        description, or file lists in remote repositories.
 */
class Search
{
public:
    /**
     * @brief Searches for packages whose name, version, or description
     *        matches the given query string, using data from repos.conf.
     *
     * @param query      The substring to look for in package metadata.
     * @param configPath The path to repos.conf (default "/etc/starpack/repos.conf").
     */
    static void searchPackages(const std::string& query,
                               const std::string& configPath = "/etc/starpack/repos.conf");

    /**
     * @brief Searches for packages that contain a given file path (or filename)
     *        in their file list. Reads repository data from repos.conf.
     *
     * @param filePath   The file path or filename to look for.
     * @param configPath The path to repos.conf (default "/etc/starpack/repos.conf").
     */
    static void searchByFile(const std::string& filePath,
                             const std::string& configPath = "/etc/starpack/repos.conf");
};

} // namespace Starpack

#endif // SEARCH_HPP
