#include "update.hpp"
#include "install.hpp"  // Provides Installer::verifyGPGSignature(...)
#include "hook.hpp"     // Provides Hook::runNewStyleHooks(...)

#include <iostream>        // For standard I/O
#include <fstream>         // For file stream operations
#include <filesystem>      // For path manipulation (C++17)
#include <sstream>         // For string stream operations
#include <algorithm>       // For std::find, std::sort, etc.
#include <cstdlib>         // For std::system, if needed
#include <archive.h>       // For reading archives (libarchive)
#include <archive_entry.h> // For archive entry handling
#include <yaml-cpp/yaml.h> // For parsing YAML
#include <vector>          // For dynamic arrays
#include <regex>           // Potentially for version/date parsing
#include <chrono>          // Potentially for timestamps
#include <cstdio>          // For fopen with libcurl
#include <ctime>           // Potentially for date conversions
#include <set>            // For sets (e.g., installed files)
#include <unordered_set>  // For efficient lookups
#include <curl/curl.h>    // For downloading files (libcurl)
#include <string.h>       // For strerror, strcmp, etc.

// Need to review logic

namespace fs = std::filesystem;

namespace { // Anonymous namespace for internal helpers and constants

// ---------------------------------------------------------------------------
// A set of critical package names to warn about.
const std::unordered_set<std::string> criticalPackages = {
    "glibc", "linux", "coreutils", "bash", "systemd"
};

// ---------------------------------------------------------------------------
// Checks if a given package is in the critical list.
bool isCriticalPackage(const std::string& pkg)
{
    return (criticalPackages.find(pkg) != criticalPackages.end());
}

// ---------------------------------------------------------------------------
// removeObsoleteFiles
//
// Removes files belonging to a previous version of a package that do not appear
// in the new package's file list. Reads from the installed DB to find what was
// previously installed, then compares to the "files" list in the new version.
// need to add tracking for explicit vs. dependency-owned files, etc.
//

void removeObsoleteFiles(const std::string& packageName,
                         const std::string& installDir,
                         const YAML::Node& newFiles)
{
    // Construct the path to the installed database.
    std::string dbPath = installDir + "/var/lib/starpack/installed.db";
    std::ifstream dbFile(dbPath);
    if (!dbFile.is_open()) {
        std::cerr << "Warning: Could not open database " << dbPath
                  << " to remove obsolete files for " << packageName << ".\n";
        return;
    }

    // Read the currently installed files for this package.
    std::set<std::string> installedFiles;
    std::string line;
    bool inPackageSection = false;
    bool inFilesSection   = false;

    while (std::getline(dbFile, line)) {
        if (!inPackageSection && line.rfind(packageName, 0) == 0) {
            // Found the package header
            inPackageSection = true;
            continue;
        }
        if (!inPackageSection) {
            continue; // Skip until we reach this package
        }

        // Detect the "Files:" list
        if (line.rfind("Files:", 0) == 0) {
            inFilesSection = true;
            continue;
        }

        // If we hit an empty line or next section, end the files section
        if (inFilesSection && (line.empty() || line.find(':') != std::string::npos)) {
            break;
        }

        // Accumulate file paths
        if (inFilesSection) {
            std::string filePath = line;
            Hook::trim(filePath);
            if (!filePath.empty()) {
                if (filePath[0] == '/') {
                    filePath.erase(0, 1);
                }
                installedFiles.insert(filePath);
            }
        }

        // Possibly detect the end of the package entry
        if (line == "----------------------------------------") {
            break; // Done with this package
        }
    }
    dbFile.close();

    // Build a set of files in the new package version
    std::set<std::string> newFileSet;
    if (newFiles && newFiles.IsSequence()) {
        for (const auto& node : newFiles) {
            if (!node.IsScalar()) {
                continue;
            }
            std::string fileStr = node.as<std::string>();
            Hook::trim(fileStr);
            if (!fileStr.empty()) {
                if (fileStr[0] == '/') {
                    fileStr.erase(0, 1);
                }
                if (!fileStr.empty() && fileStr.back() == '/') {
                    fileStr.pop_back();
                }
                newFileSet.insert(fileStr);
            }
        }
    } else {
        std::cerr << "Warning: No valid 'files' list for " << packageName
                  << ". Cannot remove obsolete files.\n";
        return;
    }

    // For each installed file not in the new set, remove it
    for (const auto& file : installedFiles) {
        if (newFileSet.find(file) == newFileSet.end()) {
            // This file is obsolete
            fs::path fullPath = fs::path(installDir) / file;
            std::error_code ec;

            if (!fs::exists(fullPath, ec) || ec) {
                // If it doesn't exist or error checking existence, skip
                continue;
            }

            try {
                if (fs::is_symlink(fullPath)) {
                    fs::remove(fullPath, ec);
                    if (!ec) {
                        std::cout << "Removed obsolete symlink: "
                                  << fullPath.string() << std::endl;
                    }
                } else if (fs::is_regular_file(fullPath)) {
                    fs::remove(fullPath, ec);
                    if (!ec) {
                        std::cout << "Removed obsolete file: "
                                  << fullPath.string() << std::endl;
                    }
                } else if (fs::is_directory(fullPath)) {
                    // Remove only if empty
                    if (fs::is_empty(fullPath, ec) && !ec) {
                        fs::remove(fullPath, ec);
                        if (!ec) {
                            std::cout << "Removed obsolete empty directory: "
                                      << fullPath.string() << std::endl;
                        }
                    }
                }
                if (ec) {
                    std::cerr << "Warning: Error removing obsolete item "
                              << fullPath.string() << ": " << ec.message() << std::endl;
                }
            } catch (const fs::filesystem_error& e) {
                std::cerr << "Warning: Exception removing " << fullPath.string()
                          << ": " << e.what() << std::endl;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// extract_archive_section
//
// Uses libarchive to extract only entries whose paths start with `sectionPrefix`.
int extract_archive_section(const std::string& archivePath,
                            const std::string& sectionPrefix,
                            const std::string& destDir,
                            int stripComponents)
{
    struct archive* a   = archive_read_new();
    struct archive* ext = archive_write_disk_new();
    if (!a || !ext) {
        std::cerr << "Error: Failed to initialize libarchive read/write handles.\n";
        if (a)  archive_read_free(a);
        if (ext) archive_write_free(ext);
        return 1;
    }

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    archive_write_disk_set_options(
        ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
             ARCHIVE_EXTRACT_ACL  | ARCHIVE_EXTRACT_FFLAGS
    );
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, archivePath.c_str(), 10240) != ARCHIVE_OK) {
        std::cerr << "Error: Could not open archive " << archivePath
                  << ": " << archive_error_string(a) << "\n";
        archive_read_free(a);
        archive_write_free(ext);
        return 1;
    }

    fs::create_directories(destDir);

    bool foundAny = false;
    struct archive_entry* entry;
    int r;

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        std::string entryName = archive_entry_pathname(entry);

        if (entryName.rfind(sectionPrefix, 0) != 0) {
            archive_read_data_skip(a);
            continue;
        }

        std::string relativePath = entryName.substr(sectionPrefix.size());
        fs::path p(relativePath);
        fs::path finalRelativePath;
        int currStrip = 0;

        for (const auto& part : p) {
            if (currStrip < stripComponents) {
                currStrip++;
                continue;
            }
            finalRelativePath /= part;
        }

        if (finalRelativePath.empty()) {
            archive_read_data_skip(a);
            continue;
        }

        fs::path fullDestPath = fs::path(destDir) / finalRelativePath;
        archive_entry_set_pathname(entry, fullDestPath.string().c_str());
        foundAny = true;

        r = archive_write_header(ext, entry);
        if (r < ARCHIVE_OK) {
            std::cerr << "Warning: write_header error for "
                      << fullDestPath.string() << ": "
                      << archive_error_string(ext) << "\n";
        } else if (archive_entry_size(entry) > 0) {
            const void* buff;
            size_t size;
            int64_t offset;

            while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                if (archive_write_data_block(ext, buff, size, offset) < ARCHIVE_OK) {
                    std::cerr << "Error writing data for "
                              << fullDestPath.string() << ": "
                              << archive_error_string(ext) << "\n";
                    break;
                }
            }
            if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
                std::cerr << "Error reading data block for "
                          << fullDestPath.string() << ": "
                          << archive_error_string(a) << std::endl;
            }
        }

        r = archive_write_finish_entry(ext);
        if (r < ARCHIVE_OK) {
            std::cerr << "Error finishing entry "
                      << fullDestPath.string() << ": "
                      << archive_error_string(ext) << std::endl;
        }
    }

    if (r != ARCHIVE_EOF) {
        std::cerr << "Error reading archive headers: "
                  << archive_error_string(a) << "\n";
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);

    return foundAny ? 0 : 1;
}

// ---------------------------------------------------------------------------
// atomicDirectoryRename
//
// Attempts to rename a directory, removing the destination first if necessary.
bool atomicDirectoryRename(const fs::path& sourceDir, const fs::path& destDir)
{
    std::error_code ec;
    if (fs::exists(destDir, ec)) {
        fs::remove_all(destDir, ec);
        if (ec) {
            std::cerr << "Error removing existing dest '"
                      << destDir.string() << "': " << ec.message() << "\n";
            return false;
        }
    } else if (ec) {
        std::cerr << "Error checking existence of '"
                  << destDir.string() << "': " << ec.message() << "\n";
        return false;
    }

    fs::rename(sourceDir, destDir, ec);
    if (ec) {
        std::cerr << "Error renaming '"
                  << sourceDir.string() << "' to '"
                  << destDir.string() << "': " << ec.message() << "\n";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// isUrl
//
// Checks if string starts with http:// or https://
bool isUrl(const std::string& url)
{
    return (url.rfind("http://", 0) == 0 ||
            url.rfind("https://", 0) == 0);
}

} // end anonymous namespace

namespace Starpack {

// ============================================================================
// Updater::downloadFile
//
// Downloads a file from a URL to a local path using libcurl.
bool Updater::downloadFile(const std::string& url, const std::string& destPath)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Error: curl_easy_init() failed.\n";
        return false;
    }

    FILE* fp = fopen(destPath.c_str(), "wb");
    if (!fp) {
        std::cerr << "Error: Failed to open destination file '"
                  << destPath << "' for writing: "
                  << strerror(errno) << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);

    bool success = (res == CURLE_OK);
    if (!success) {
        std::cerr << "Error: Failed to download " << url << ": "
                  << curl_easy_strerror(res) << std::endl;
        std::error_code ec;
        fs::remove(destPath, ec); // remove partial file
    }

    curl_easy_cleanup(curl);
    return success;
}

// ============================================================================
// Updater::compareVersions
//
// Compares two dot-separated numeric version strings (e.g., "1.2.3").
int Updater::compareVersions(const std::string& v1, const std::string& v2)
{
    std::istringstream iss1(v1), iss2(v2);
    std::string token1, token2;
    int num1 = 0, num2 = 0;
    char delim = '.';

    while (true) {
        bool got1 = bool(std::getline(iss1, token1, delim));
        bool got2 = bool(std::getline(iss2, token2, delim));

        try { num1 = got1 ? std::stoi(token1) : 0; }
        catch (...) { num1 = 0; }
        try { num2 = got2 ? std::stoi(token2) : 0; }
        catch (...) { num2 = 0; }

        if (num1 > num2) return 1;
        if (num1 < num2) return -1;

        if (!got1 && !got2) {
            break; // same length and all equal
        }
        if (got1 && !got2 && num1 > 0) {
            return 1;
        }
        if (!got1 && got2 && num2 > 0) {
            return -1;
        }
        if (!got1 && !got2) {
            break;
        }
    }
    return 0;
}

// ============================================================================
// Updater::compareDates
//
// Compares two "DD/MM/YYYY" date strings.
int Updater::compareDates(const std::string& d1, const std::string& d2)
{
    std::tm tm1 = {}, tm2 = {};
    char format[] = "%d/%m/%Y";

    if (strptime(d1.c_str(), format, &tm1) == nullptr ||
        strptime(d2.c_str(), format, &tm2) == nullptr) {
        std::cerr << "Warning: Could not parse date '" << d1
                  << "' or '" << d2 << "' with DD/MM/YYYY.\n";
        return 0;
    }

    if (tm1.tm_year != tm2.tm_year) return (tm1.tm_year > tm2.tm_year) ? 1 : -1;
    if (tm1.tm_mon != tm2.tm_mon)   return (tm1.tm_mon > tm2.tm_mon)   ? 1 : -1;
    if (tm1.tm_mday != tm2.tm_mday) return (tm1.tm_mday > tm2.tm_mday) ? 1 : -1;

    return 0;
}

// ============================================================================
// Updater::getInstalledVersion
//
// Fetches the "Version:" line from the installed.db for a given package.
std::string Updater::getInstalledVersion(const std::string& packageName,
                                         const std::string& dbPath)
{
    std::ifstream dbFile(dbPath);
    if (!dbFile.is_open()) {
        return "";
    }

    std::string line;
    bool inPackageSection = false;
    while (std::getline(dbFile, line)) {
        if (!inPackageSection && line.rfind(packageName, 0) == 0) {
            inPackageSection = true;
            continue;
        }
        if (!inPackageSection) {
            continue;
        }

        if (line.rfind("Version:", 0) == 0) {
            std::istringstream iss(line);
            std::string label, version;
            if (iss >> label >> version) {
                return version;
            }
        }
        if (line == "----------------------------------------") {
            break;
        }
    }
    return "";
}

// ============================================================================
// Updater::getInstalledUpdateDate
//
// Fetches the "Update-time:" line from the installed.db for a given package.
std::string Updater::getInstalledUpdateDate(const std::string& packageName,
                                            const std::string& dbPath)
{
    std::ifstream dbFile(dbPath);
    if (!dbFile.is_open()) {
        return "";
    }

    std::string line;
    bool inPackageSection = false;
    while (std::getline(dbFile, line)) {
        if (!inPackageSection && line.rfind(packageName, 0) == 0) {
            inPackageSection = true;
            continue;
        }
        if (!inPackageSection) {
            continue;
        }

        if (line.rfind("Update-time:", 0) == 0) {
            std::istringstream iss(line);
            std::string label, timeStr;
            if (iss >> label >> timeStr) {
                return timeStr;
            }
        }
        if (line == "----------------------------------------") {
            break;
        }
    }
    return "";
}

// ============================================================================
// Updater::updateDatabaseVersion
//
// Updates the Version and Update-time lines for a specific package in the DB.
void Updater::updateDatabaseVersion(const std::string& packageName,
                                    const std::string& dbPath,
                                    const std::string& newVersion,
                                    const std::string& newUpdateDate)
{
    std::ifstream dbFile(dbPath);
    if (!dbFile.is_open()) {
        std::cerr << "Error: Cannot open DB file " << dbPath
                  << " for updating.\n";
        return;
    }

    std::ostringstream updated;
    std::string line;
    bool inTargetPkg = false;
    bool versionUpdated = false;
    bool dateUpdated    = false;

    while (std::getline(dbFile, line)) {
        if (!inTargetPkg && line.rfind(packageName, 0) == 0) {
            inTargetPkg = true;
            updated << line << "\n";
        } else if (inTargetPkg) {
            if (line.rfind("Version:", 0) == 0) {
                updated << "Version: " << newVersion << "\n";
                versionUpdated = true;
            } else if (line.rfind("Update-time:", 0) == 0) {
                updated << "Update-time: " << newUpdateDate << "\n";
                dateUpdated = true;
            } else {
                updated << line << "\n";
            }
            if (line == "----------------------------------------") {
                inTargetPkg = false;
            }
        } else {
            updated << line << "\n";
        }
    }

    dbFile.close();
    if (!versionUpdated || !dateUpdated) {
        std::cerr << "Warning: Could not find '" << packageName
                  << "' or its Version/Update-time in " << dbPath
                  << ". Not updated.\n";
        return;
    }

    std::ofstream outFile(dbPath, std::ios::trunc);
    if (outFile.is_open()) {
        outFile << updated.str();
        outFile.close();
    } else {
        std::cerr << "Error: Failed to open DB " << dbPath
                  << " for writing updates.\n";
    }
}

// ============================================================================
// Updater::getConfirmation
//
// Prompts user for Y/n confirmation about a set of packages to be updated.
bool Updater::getConfirmation(const std::vector<std::string>& packages)
{
    std::cout << "The following packages will be updated:\n";
    for (const auto& pkg : packages) {
        std::cout << "  - " << pkg << "\n";
    }
    std::cout << "Do you want to continue? [Y/n]: ";

    std::string response;
    std::getline(std::cin, response);
    Hook::trim(response);

    return (response.empty() || response == "y" || response == "Y");
}

// ============================================================================
// Updater::extractUpdatedFiles
//
// Extracts the "files/" section from a package archive into `destDir`.
bool Updater::extractUpdatedFiles(const std::string& packagePath,
                                  const std::string& destDir,
                                  const std::vector<std::string>& updateDirs,
                                  int effectiveStrip)
{
    int result = extract_archive_section(packagePath, "files/", destDir, effectiveStrip);
    return (result == 0); // 0 => success, 1 => no entries matched
}

// ============================================================================
// Updater::shouldUpdateFile
//
// Checks if a file path belongs to the user-specified updateDirs.
bool Updater::shouldUpdateFile(const std::string& filePath,
                               const std::vector<std::string>& updateDirs)
{
    if (updateDirs.empty()) {
        return true; // If no subdirs specified, update all
    }
    for (const auto& dir : updateDirs) {
        std::string prefix = dir;
        if (!prefix.empty() && prefix.back() != '/') {
            prefix.push_back('/');
        }
        if (filePath.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Updater::extractFileFromArchive
//
// Extracts a single file (e.g., "metadata.yaml") from the .starpack archive
// into the given extractDir.
bool Updater::extractFileFromArchive(const std::string& archivePath,
                                     const std::string& targetEntry,
                                     const std::string& extractDir)
{
    struct archive* a = archive_read_new();
    if (!a) {
        std::cerr << "Error: archive_read_new() failed.\n";
        return false;
    }
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a); // changed to support all compression formats

    if (archive_read_open_filename(a, archivePath.c_str(), 10240) != ARCHIVE_OK) {
        std::cerr << "Error: Could not open " << archivePath
                  << ": " << archive_error_string(a) << "\n";
        archive_read_free(a);
        return false;
    }

    bool found = false;
    struct archive_entry* entry;
    std::error_code ec;
    fs::create_directories(extractDir, ec);
    if (ec) {
        std::cerr << "Error: Failed creating directory '"
                  << extractDir << "': " << ec.message() << "\n";
        archive_read_close(a);
        archive_read_free(a);
        return false;
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string entryName = archive_entry_pathname(entry);

        if (entryName == targetEntry ||
            ("./" + targetEntry) == entryName)
        {
            fs::path outputFilePath = fs::path(extractDir) / fs::path(targetEntry).filename();
            std::ofstream outputFile(outputFilePath, std::ios::binary | std::ios::trunc);
            if (!outputFile) {
                std::cerr << "Error: Failed to open output file " << outputFilePath << std::endl;
                archive_read_data_skip(a);
                continue;
            }
            const void* buff;
            size_t size;
            int64_t offset;
            int r;

            while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                outputFile.write(static_cast<const char*>(buff), size);
                if (!outputFile) {
                    std::cerr << "Error: Writing to " << outputFilePath << " failed.\n";
                    outputFile.close();
                    fs::remove(outputFilePath, ec);
                    found = false;
                    goto finish;
                }
            }
            if (r == ARCHIVE_EOF || r == ARCHIVE_OK) {
                found = true;
            }
            outputFile.close();
            break;
        } else {
            archive_read_data_skip(a);
        }
    }

finish:
    archive_read_close(a);
    archive_read_free(a);
    return found;
}

// ============================================================================
// Updater::updatePackage
//
// The main function that orchestrates package updates: checking repos,
// comparing versions, downloading, verifying, hooking, extracting, database
// updates, etc.
void Updater::updatePackage(const std::vector<std::string>& packageNames,
                            const std::string& installDir)
{
    // ===============================================================
    // FIX for "use of undeclared identifier 'installedDbPath'":
    //
    // We declare this variable at the start of the function so it
    // remains in scope for the entire update process.  
    // ===============================================================
    std::string installedDbPath = installDir + "/var/lib/starpack/installed.db";

    // --- Step 1: Load Repository Configuration ---
    std::cout << "[1/N] Loading repository configuration...\n";
    std::ifstream repoConf("/etc/starpack/repos.conf");
    if (!repoConf.is_open()) {
        std::cerr << "Error: Unable to open /etc/starpack/repos.conf.\n";
        return;
    }

    std::vector<std::string> repoUrls;
    std::string repoUrlLine;
    while (std::getline(repoConf, repoUrlLine)) {
        Hook::trim(repoUrlLine);
        if (repoUrlLine.empty() || repoUrlLine[0] == '#') {
            continue;
        }
        if (repoUrlLine.back() != '/') {
            repoUrlLine += '/';
        }
        repoUrls.push_back(repoUrlLine);
    }
    repoConf.close();

    if (repoUrls.empty()) {
        std::cerr << "Error: No valid repository URLs found.\n";
        return;
    }
    std::cout << "Found " << repoUrls.size() << " repository URL(s).\n";

    // Prepare a structure for potential updates
    struct UpdateCandidate {
        std::string packageName;
        std::string candidateVersion;
        std::string candidateUpdateTime; // e.g., "DD/MM/YYYY"
        std::string packageFileUrl;
        YAML::Node  metadata;
    };
    std::vector<UpdateCandidate> candidates;

    // --- Step 2: Check Repositories for Updates ---
    std::cout << "[2/N] Checking repositories for updates...\n";
    std::string tempRepoDbPath = "/tmp/starpack_repo_cache.db.yaml";

    for (const auto &pkgName : packageNames) {
        std::cout << " -> Checking updates for: " << pkgName << std::endl;
        bool foundCandidate = false;
        UpdateCandidate best;

        // For each repository, download the index and look for pkgName
        for (const auto &url : repoUrls) {
            std::string repoIndexUrl = url + "repo.db.yaml";
            std::cout << "    Checking repo: " << repoIndexUrl << std::endl;

            if (!downloadFile(repoIndexUrl, tempRepoDbPath)) {
                std::cerr << "    Warning: Could not download " << repoIndexUrl << "\n";
                continue;
            }

            YAML::Node repoIndex;
            try {
                repoIndex = YAML::LoadFile(tempRepoDbPath);
            } catch (const std::exception &e) {
                std::cerr << "    Warning: Failed to parse "
                          << repoIndexUrl << ": " << e.what() << "\n";
                std::error_code ec; fs::remove(tempRepoDbPath, ec);
                continue;
            }
            // Remove the temp file after parsing
            std::error_code ec; fs::remove(tempRepoDbPath, ec);

            if (!repoIndex["packages"] || !repoIndex["packages"].IsSequence()) {
                std::cerr << "    Warning: Invalid 'packages' in " << repoIndexUrl << "\n";
                continue;
            }

            // Search for our package in this repo
            for (const auto &node : repoIndex["packages"]) {
                if (!node["name"] || !node["version"] || !node["file_name"]) {
                    // Skip invalid nodes
                    continue;
                }
                if (node["name"].as<std::string>() == pkgName) {
                    std::string repoVersion = node["version"].as<std::string>();
                    std::string repoUpdateTime;
                    if (node["update_time"] && node["update_time"].IsScalar()) {
                        repoUpdateTime = node["update_time"].as<std::string>();
                    }

                    // Compare with the best found so far
                    if (!foundCandidate ||
                        compareVersions(repoVersion, best.candidateVersion) > 0 ||
                       (compareVersions(repoVersion, best.candidateVersion) == 0 &&
                        !repoUpdateTime.empty() &&
                        (best.candidateUpdateTime.empty() ||
                         compareDates(repoUpdateTime, best.candidateUpdateTime) > 0)))
                    {
                        best.packageName         = pkgName;
                        best.candidateVersion    = repoVersion;
                        best.candidateUpdateTime = repoUpdateTime;
                        best.packageFileUrl      = url + node["file_name"].as<std::string>();
                        best.metadata            = YAML::Clone(node);
                        foundCandidate = true;
                    }
                }
            }
        }
        if (!foundCandidate) {
            std::cerr << "Info: '" << pkgName << "' not found in any repo.\n";
            continue;
        }

        // Compare the best candidate version/date with the installed version/date
        std::string installedVersion = getInstalledVersion(pkgName, installedDbPath);
        std::string installedDate    = getInstalledUpdateDate(pkgName, installedDbPath);

        bool upToDate = false;
        if (!installedVersion.empty()) {
            int verCmp = compareVersions(installedVersion, best.candidateVersion);
            if (verCmp > 0) {
                upToDate = true;
            } else if (verCmp == 0) {
                if (!installedDate.empty() && !best.candidateUpdateTime.empty()) {
                    if (compareDates(installedDate, best.candidateUpdateTime) >= 0) {
                        upToDate = true;
                    }
                } else if (best.candidateUpdateTime.empty()) {
                    // No date info in candidate => treat as up-to-date
                    upToDate = true;
                }
            }
        }

        if (upToDate) {
            std::cout << "Info: '" << pkgName << "' is already up-to-date.\n";
            continue;
        }

        std::cout << "Info: Update found for '" << pkgName << "' (Installed: "
                  << (installedVersion.empty() ? "None" : installedVersion)
                  << ", Available: " << best.candidateVersion << ")\n";
        candidates.push_back(best);
    }

    if (candidates.empty()) {
        std::cout << "All specified packages are up-to-date or not found.\n";
        return;
    }

    // --- Step 3: Confirmation ---
    std::cout << "[3/N] Confirming updates...\n";
    bool foundCritical = false;
    for (auto &cand : candidates) {
        if (isCriticalPackage(cand.packageName)) {
            foundCritical = true;
        }
    }
    if (foundCritical) {
        std::cout << "WARNING: At least one critical package is about to be updated!\n";
    }

    // Collect for user prompt
    std::vector<std::string> pkgsToConfirm;
    for (auto &c : candidates) {
        pkgsToConfirm.push_back(c.packageName + " (" + c.candidateVersion + ")");
    }
    if (!getConfirmation(pkgsToConfirm)) {
        std::cout << "Update canceled by user.\n";
        return;
    }

    // --- Step 4: Download, Verify, and Apply Updates ---
    std::cout << "[4/N] Applying updates...\n";
    size_t idx = 0;
    for (auto &cand : candidates) {
        idx++;
        std::cout << "\n(" << idx << "/" << candidates.size()
                  << ") Updating: " << cand.packageName << "\n"
                  << "  Version: " << cand.candidateVersion
                  << (cand.candidateUpdateTime.empty() ? "" : " (Update Time: " + cand.candidateUpdateTime + ")") << "\n"
                  << "  Source: " << cand.packageFileUrl << std::endl;

        // (A) Download
        fs::path tempDir = "/tmp/starpack_update_" + cand.packageName;
        fs::create_directories(tempDir);
        std::string tempPkgPath = (tempDir / (cand.packageName + ".starpack")).string();
        std::string tempSigPath = tempPkgPath + ".sig";

        std::cout << "  Downloading package..." << std::flush;
        if (!downloadFile(cand.packageFileUrl, tempPkgPath)) {
            std::cerr << "\nError: Package download failed.\n";
            fs::remove_all(tempDir);
            continue;
        }
        std::cout << " Done.\n";

        std::cout << "  Downloading signature..." << std::flush;
        if (!downloadFile(cand.packageFileUrl + ".sig", tempSigPath)) {
            std::cerr << "\nError: Signature download failed.\n";
            fs::remove_all(tempDir);
            continue;
        }
        std::cout << " Done.\n";

        // (B) Verify GPG signature
        std::cout << "  Verifying signature..." << std::flush;
        if (!Installer::verifyGPGSignature(tempPkgPath, tempSigPath, installDir)) {
            std::cerr << "\nError: GPG signature verification failed.\n";
            fs::remove_all(tempDir);
            continue;
        }
        std::cout << " OK.\n";

        // (C) Extract metadata.yaml from inside the package
        std::string tempMetaDir = (tempDir / "meta_extract").string();
        YAML::Node packageMetadata;
        if (extractFileFromArchive(tempPkgPath, "metadata.yaml", tempMetaDir)) {
            try {
                packageMetadata = YAML::LoadFile(tempMetaDir + "/metadata.yaml");
            } catch (const std::exception& e) {
                std::cerr << "  Warning: Could not parse metadata.yaml: "
                          << e.what() << " (Using repo metadata fallback)\n";
                packageMetadata = cand.metadata;
            }
        } else {
            std::cerr << "  Warning: Could not extract metadata.yaml. Using repo metadata fallback.\n";
            packageMetadata = cand.metadata;
        }
        fs::remove_all(tempMetaDir);

        if (!packageMetadata || !packageMetadata["files"] || !packageMetadata["files"].IsSequence()) {
            std::cerr << "Error: Invalid metadata for " << cand.packageName << ". Skipping update.\n";
            fs::remove_all(tempDir);
            continue;
        }

        // (D) Gather changed file paths for Hook usage
        std::vector<std::string> changedPaths;
        for (const auto& fNode : packageMetadata["files"]) {
            if (!fNode.IsScalar()) {
                continue;
            }
            std::string path = fNode.as<std::string>();
            Hook::trim(path);
            if (!path.empty() && path.front() == '/') {
                path.erase(0, 1);
            }
            if (!path.empty()) {
                changedPaths.push_back(path);
            }
        }

        // (E) PreUpdate Hooks
        std::cout << "  Running PreUpdate hooks...\n";
        auto preHookCount = Hook::runNewStyleHooks("PreUpdate", "Update",
                                                   changedPaths,
                                                   installDir,
                                                   cand.packageName);
        if (preHookCount > 0) {
            std::cout << "    (" << preHookCount
                      << " PreUpdate hooks executed)\n";
        }

        // (F) Extract updated files to staging
        std::cout << "  Extracting updated files..." << std::flush;
        int stripComponents = 0;
        if (packageMetadata["strip_components"] &&
            packageMetadata["strip_components"].IsScalar()) {
            try {
                stripComponents = packageMetadata["strip_components"].as<int>();
            } catch (...) {
                stripComponents = 0;
            }
        }
        std::vector<std::string> updateDirs; // If partial updates were required
        std::string stagingDir = (tempDir / "staging").string();
        if (!extractUpdatedFiles(tempPkgPath, stagingDir, updateDirs, stripComponents)) {
            std::cerr << "\n  Warning: Some extraction issues occurred.\n";
        } else {
            std::cout << " Done.\n";
        }

        // (G) Move staged files to final
        std::cout << "  Applying file updates..." << std::flush;
        bool applyOk = true;
        try {
            for (const auto& de : fs::recursive_directory_iterator(stagingDir)) {
                fs::path srcPath = de.path();
                fs::path relPath = fs::relative(srcPath, stagingDir);
                fs::path dstPath = fs::path(installDir) / relPath;

                std::error_code ec;
                if (fs::is_directory(srcPath)) {
                    fs::create_directories(dstPath, ec);
                } else {
                    fs::create_directories(dstPath.parent_path(), ec);
                    if (fs::exists(dstPath, ec) || fs::is_symlink(dstPath, ec)) {
                        fs::remove(dstPath, ec);
                    }
                    fs::rename(srcPath, dstPath, ec);
                    if (ec) {
                        throw fs::filesystem_error("Failed to rename staging item",
                                                   srcPath, dstPath, ec);
                    }
                }
            }
        } catch (const fs::filesystem_error &ex) {
            std::cerr << "\nError applying file updates for "
                      << cand.packageName << ": " << ex.what() << std::endl;
            applyOk = false;
        }
        fs::remove_all(stagingDir); // Remove staging dir

        if (!applyOk) {
            std::cerr << "Error: Update failed mid-application for "
                      << cand.packageName << ".\n";
            fs::remove_all(tempDir);
            continue;
        }
        std::cout << " Done.\n";

        // (H) Update DB
        std::cout << "  Updating installation database..." << std::flush;
        updateDatabaseVersion(cand.packageName,
                              installedDbPath,
                              cand.candidateVersion,
                              cand.candidateUpdateTime);
        std::cout << " Done.\n";

        // (I) Remove obsolete files if no partial subdirectories
        if (!packageMetadata["update_dirs"] || !packageMetadata["update_dirs"].IsSequence()) {
            std::cout << "  Removing obsolete files...\n";
            removeObsoleteFiles(cand.packageName, installDir,
                                packageMetadata["files"]);
            std::cout << "  Obsolete file check complete.\n";
        }

        // (J) PostUpdate Hooks
        std::cout << "  Running PostUpdate hooks...\n";
        auto postHookCount = Hook::runNewStyleHooks("PostUpdate", "Update",
                                                    changedPaths,
                                                    installDir,
                                                    cand.packageName);
        if (postHookCount > 0) {
            std::cout << "    (" << postHookCount
                      << " PostUpdate hooks executed)\n";
        }

        // (K) Final Cleanup
        std::cout << "Package updated successfully: " << cand.packageName << "\n";
        if (isCriticalPackage(cand.packageName)) {
            std::cout << "NOTICE: '" << cand.packageName
                      << "' is critical. A reboot is recommended.\n";
        }
        fs::remove_all(tempDir); // Clean up
    } // end for(candidates)

    std::cout << "\n--- Update process finished. ---\n";
}

} // namespace Starpack
