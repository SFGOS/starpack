#ifndef LIST_HPP
#define LIST_HPP

#include <string>
#include <vector>

namespace Starpack {

/**
 * @class List
 * @brief Provides methods to list installed packages or related metadata.
 */
class List
{
public:
    /**
     * @brief Prints a list of all installed packages to standard output.
     *
     * @param dbPath Path to the local Starpack installation database,
     *        defaults to "/var/lib/starpack/installed.db".
     */
    static void showInstalledPackages(const std::string& dbPath = "/var/lib/starpack/installed.db");
};

} // namespace Starpack

#endif // LIST_HPP
