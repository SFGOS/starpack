#ifndef UPDATE_HPP
#define UPDATE_HPP

#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;

namespace Starpack {

    /**
     * @brief Formats a filesystem time_point as "HH:MM:SS".
     *
     * @param ftime The file time to be formatted.
     * @return A string representing the time in HH:MM:SS format.
     */
    inline std::string formatTimestamp(const fs::file_time_type& ftime)
    {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
        char buffer[9]; // "HH:MM:SS" plus null terminator
        std::tm* tm_info = std::localtime(&cftime);
        std::strftime(buffer, sizeof(buffer), "%H:%M:%S", tm_info);
        return std::string(buffer);
    }

    /**
     * @class Updater
     * @brief Provides functionality to update packages by downloading, comparing versions,
     *        verifying signatures, extracting files, and updating the installation database.
     */
    class Updater
    {
    public:
        /**
         * @brief Updates the specified packages.
         *
         * By default, this function updates all packages found in the repository index,
         * using the repository's update_time for logging and database updates. It prompts
         * for confirmation before starting the update process.
         *
         * @param packageNames A list of package names to update.
         * @param installDir   The installation root directory (default is "/").
         */
        static void updatePackage(const std::vector<std::string>& packageNames,
                                  const std::string& installDir = "/");

    private:
        /**
         * @brief Determines if a given file path should be updated based on update_dirs.
         *
         * @param filePath  The relative file path to check.
         * @param updateDirs A list of allowed update directories.
         * @return True if filePath begins with any entry in updateDirs or if updateDirs is empty.
         */
        static bool shouldUpdateFile(const std::string& filePath,
                                     const std::vector<std::string>& updateDirs);

        /**
         * @brief Extracts only those files from the "files" section of an archive that match update_dirs.
         *
         * @param packagePath   The path to the package archive.
         * @param installDir    The destination installation directory.
         * @param updateDirs    A list of update directories to filter the files.
         * @param effectiveStrip Number of leading path components to remove.
         * @return True if extraction was successful, false otherwise.
         */
        static bool extractUpdatedFiles(const std::string& packagePath,
                                        const std::string& installDir,
                                        const std::vector<std::string>& updateDirs,
                                        int effectiveStrip);

        /**
         * @brief Extracts a single file (such as "metadata.yaml") from an archive.
         *
         * @param archivePath The path to the archive file.
         * @param targetEntry The entry (file) within the archive to extract.
         * @param extractDir  The directory where the extracted file will be saved.
         * @return True if the file was found and extracted successfully, false otherwise.
         */
        static bool extractFileFromArchive(const std::string& archivePath,
                                           const std::string& targetEntry,
                                           const std::string& extractDir);

        /**
         * @brief Downloads a file from a given URL to a local destination.
         *
         * @param url      The URL to download from.
         * @param destPath The destination filesystem path.
         * @return True if the file was successfully downloaded, false otherwise.
         */
        static bool downloadFile(const std::string& url, const std::string& destPath);

        /**
         * @brief Compares two version strings numerically.
         *
         * @param v1 The first version string.
         * @param v2 The second version string.
         * @return 1 if v1 is newer than v2, 0 if equal, -1 if v1 is older than v2.
         */
        static int compareVersions(const std::string& v1, const std::string& v2);

        /**
         * @brief Compares two date strings in dd/mm/yy format.
         *
         * @param d1 The first date string.
         * @param d2 The second date string.
         * @return 1 if d1 is later than d2, 0 if equal, -1 if d1 is earlier than d2.
         */
        static int compareDates(const std::string& d1, const std::string& d2);

        /**
         * @brief Retrieves the installed version of a package from the installation DB.
         *
         * @param packageName The name of the package.
         * @param dbPath      The path to the installed database.
         * @return A string representing the installed version, or an empty string on error.
         */
        static std::string getInstalledVersion(const std::string& packageName, const std::string& dbPath);

        /**
         * @brief Retrieves the last update timestamp for a package from the installed DB.
         *
         * @param packageName The name of the package.
         * @param dbPath      The path to the installed database.
         * @return A date string representing the last update, or an empty string on error.
         */
        static std::string getInstalledUpdateDate(const std::string& packageName, const std::string& dbPath);

        /**
         * @brief Updates the version and update time for a package in the installed DB.
         *
         * Replaces the lines starting with "Version:" and "Update-time:" for the specified package.
         *
         * @param packageName    The package to update.
         * @param dbPath         The path to the installed database.
         * @param newVersion     The new version string.
         * @param newUpdateTime  The new update time string.
         */
        static void updateDatabaseVersion(const std::string& packageName, const std::string& dbPath,
                                          const std::string& newVersion, const std::string& newUpdateTime);

        /**
         * @brief Prompts the user for confirmation before updating packages.
         *
         * @param packages A list of package names (or package description strings) to be updated.
         * @return True if the user confirms, false otherwise.
         */
        static bool getConfirmation(const std::vector<std::string>& packages);
    };

} // namespace Starpack

#endif // UPDATE_HPP
