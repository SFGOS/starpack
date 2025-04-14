#ifndef INFO_HPP
#define INFO_HPP

#include <string>
#include <vector>
#include <map>

/**
 * @class PackageInfo
 * @brief Holds metadata about a package, including its name, version,
 *        description, dependencies, and associated files.
 */
class PackageInfo
{
public:
    /**
     * @brief Constructs a PackageInfo object with the specified parameters.
     *
     * @param name         The package name.
     * @param version      The package version.
     * @param description  A short description of the package.
     * @param dependencies A list of package dependencies.
     * @param files        A map of file paths to descriptions (or additional info).
     */
    PackageInfo(const std::string& name,
                const std::string& version,
                const std::string& description,
                const std::vector<std::string>& dependencies,
                const std::map<std::string, std::string>& files);

    /**
     * @return The name of the package.
     */
    std::string getName() const;

    /**
     * @return The version string of the package.
     */
    std::string getVersion() const;

    /**
     * @return A short description of the package.
     */
    std::string getDescription() const;

    /**
     * @return A vector containing names of packages this one depends on.
     */
    std::vector<std::string> getDependencies() const;

    /**
     * @return A map of file paths (keys) to file details (values).
     */
    std::map<std::string, std::string> getFiles() const;

    /**
     * @brief Prints the package's metadata (name, version, description,
     *        dependencies, and files) to standard output.
     */
    void display() const;

private:
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> dependencies;
    std::map<std::string, std::string> files;
};

/**
 * @brief Retrieves package information from a local database (installed.db).
 *
 * @param packageName  The name of the package to look up.
 * @param localDbPath  The path to the local database file.
 * @param packageInfo  Reference to a PackageInfo object that will be populated.
 * @return True if the package info was found and filled, false otherwise.
 */
bool fetchPackageInfoFromLocal(const std::string& packageName,
                               const std::string& localDbPath,
                               PackageInfo& packageInfo);

/**
 * @brief Retrieves package information from remote repositories (as defined in repos.conf).
 *
 * @param packageName   The name of the package to look up.
 * @param reposConfPath The path to the repository configuration file.
 * @param packageInfo   Reference to a PackageInfo object that will be populated.
 * @return True if the package info was found in any repository, false otherwise.
 */
bool fetchPackageInfoFromRepos(const std::string& packageName,
                               const std::string& reposConfPath,
                               PackageInfo& packageInfo);

#endif // INFO_HPP
