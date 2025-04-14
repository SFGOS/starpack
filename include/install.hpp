#ifndef INSTALL_HPP
#define INSTALL_HPP

#include <string>            // For std::string
#include <vector>            // For std::vector
#include <ctime>             // For std::time_t (date/time)
#include <yaml-cpp/yaml.h>   // For YAML::Node
#include <unordered_map>     // For std::unordered_map, if needed

namespace Starpack {

/**
 * @class Installer
 * @brief Provides static methods for package installation processes, including
 *        dependency resolution, downloads, verification, extraction,
 *        and database updates.
 */
class Installer
{
public:
    /**
     * @brief Orchestrates the installation of specified packages (and dependencies).
     *
     * Loads repositories, resolves dependencies, downloads packages, verifies
     * signatures, extracts files, and updates the local database.
     *
     * @param initialPackageNames A list of package names requested by the user.
     * @param installDir The root directory where packages will be installed 
     *        (default is system root "/").
     * @param confirm If true, user confirmation is required before proceeding
     *        (default is true).
     */
    static void installPackage(const std::vector<std::string>& initialPackageNames,
                               const std::string& installDir = "/",
                               bool confirm = true);

    /**
     * @brief Checks the local installation database to see if a package
     *        is recorded as installed.
     *
     * @param packageName The name of the package to look up.
     * @param installDir The root directory where the database is located
     *        (default is "/").
     * @return True if the package is found in the DB, false otherwise.
     */
    static bool isPackageInstalled(const std::string& packageName,
                                   const std::string& installDir = "/");

    /**
     * @brief Displays packages pending installation and prompts the user
     *        for [Y/n] confirmation.
     *
     * @param packages The list of packages to confirm.
     * @return True for 'Y', 'y', or empty input; false otherwise.
     */
    static bool getConfirmation(const std::vector<std::string>& packages);

    /**
     * @brief Records metadata for an installed package into the local DB.
     *
     * @param packageName The name of the package.
     * @param installDir  The directory where the DB is kept.
     * @param packageNode YAML data holding version, files, dependencies, etc.
     */
    static void createDatabaseEntry(const std::string& packageName,
                                    const std::string& installDir,
                                    const YAML::Node& packageNode);

    /**
     * @brief Verifies a package file using a GPG signature.
     *
     * Imports missing keys if necessary and checks the signature.
     *
     * @param packagePath The path to the downloaded package archive.
     * @param sigPath     The path to the corresponding .sig file.
     * @param installDir  The root directory for keyring or other GPG data.
     * @return True if verification succeeds, false otherwise.
     */
    static bool verifyGPGSignature(const std::string& packagePath,
                                   const std::string& sigPath,
                                   const std::string& installDir);

    /**
     * @brief Parses a date string (e.g. ISO 8601 style) into a time_t value.
     *
     * @param dateStr The date in string format (e.g. "YYYY-MM-DDTHH:MM:SSZ").
     * @return The Unix epoch time, or 0 on parse error.
     */
    static std::time_t parseUpdateDate(const std::string& dateStr);

    /**
     * @brief Retrieves the recorded update timestamp for a package from the DB.
     *
     * @param packageName The package to look up.
     * @param dbPath The path to the installation DB (default is "/var/lib/starpack/installed.db").
     * @return The time_t of the last update/install for that package, or 0 if not found.
     */
    static std::time_t getInstalledPackageUpdateDate(const std::string& packageName,
                                                     const std::string& dbPath = "/var/lib/starpack/installed.db");
};

} // namespace Starpack

#endif // INSTALL_HPP
