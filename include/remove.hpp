#ifndef REMOVE_HPP
#define REMOVE_HPP

#include <string>
#include <vector>
#include "hook.hpp" // For runNewStyleHooks if used inside remove.cpp

namespace Starpack {

/**
 * @brief Removes multiple packages, handling dependencies, user prompts (if any),
 *        file removal, and database updates.
 *
 * @param packageNames The names of packages to remove.
 * @param dbPath       The path to the local installation database.
 * @param force        If true, bypasses certain dependency checks.
 * @param installDir   The root directory where packages are installed (default "/").
 */
void removePackages(const std::vector<std::string>& packageNames,
                    const std::string& dbPath,
                    bool force,
                    const std::string& installDir = "/");

/**
 * @brief Finds all packages that depend on the given package (reverse dependencies).
 *
 * @param packageName The target package to find who depends on it.
 * @param dbPath      The path to the local installation database.
 * @return A list of packages that depend on packageName.
 */
std::vector<std::string> getReverseDependencies(const std::string& packageName,
                                                const std::string& dbPath);

/**
 * @brief Identifies packages that are no longer required by others (orphans).
 *
 * @param dbPath         The path to the local DB.
 * @param currentPackage A package that might be excluded from the check (e.g. the one being removed).
 * @return A list of orphaned packages.
 */
std::vector<std::string> getOrphanedDependencies(const std::string& dbPath,
                                                 const std::string& currentPackage);

/**
 * @brief Retrieves the list of files belonging to a package by parsing the DB.
 *
 * @param packageName The name of the package.
 * @param dbPath      The path to the local DB.
 * @return A list of file paths owned by the package.
 */
std::vector<std::string> getFilesToRemove(const std::string& packageName,
                                          const std::string& dbPath);

/**
 * @brief Removes the specified files from the filesystem under installDir.
 *
 * @param files      A list of file paths (likely from the package DB).
 * @param installDir The root installation directory where these files reside.
 */
void removeFiles(const std::vector<std::string>& files,
                 const std::string& installDir);

/**
 * @brief Removes the given package's entry from the local DB (uninstalls).
 *
 * @param packageName The package whose entry will be removed.
 * @param dbPath      The path to the local DB file.
 */
void updateDatabase(const std::string& packageName,
                    const std::string& dbPath);

/**
 * @brief Checks if a package is considered critical (e.g., essential system component).
 *
 * @param packageName The package to check.
 * @return True if it's a critical package, false otherwise.
 */
bool isCriticalPackage(const std::string& packageName);

/**
 * @brief Provides a random cautionary message for removing a critical package.
 *
 * @param packageName The critical package.
 * @return A string message warning about removing this package.
 */
std::string getRandomCriticalMessage(const std::string& packageName);

} // namespace Starpack

#endif // REMOVE_HPP
