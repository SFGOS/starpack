//============================================================================
// Includes
//============================================================================

#include "install.hpp"         // Class definition and public static methods
#include "chroot_util.hpp"     // For chroot operations, hooks use it)
#include "hook.hpp"            // For calling Pre/Post install hooks
#include "utils.hpp"           // utility functions like logging might are here

#include <iostream>            // Standard I/O (cout, cerr)
#include <fstream>             // File streams (ifstream, ofstream)
#include <sstream>             // String streams (istringstream)
#include <filesystem>          // Modern C++ filesystem operations
#include <yaml-cpp/yaml.h>     // YAML parsing library
#include <cstdlib>             // std::system, EXIT_SUCCESS/FAILURE
#include <curl/curl.h>         // Libcurl for downloading
#include <unistd.h>            // geteuid (POSIX-specific)
#include <unordered_set>       // Efficient hash sets
#include <unordered_map>       // Efficient hash maps
#include <map>                 // Ordered maps (used in download multi)
#include <set>                 // Ordered sets
#include <vector>              // Dynamic arrays
#include <deque>               // Double-ended queue (for dependency stack)
#include <utility>             // std::pair
#include <regex>               // Regular expressions (for version parsing)
#include <iomanip>             // Output formatting (setprecision, setw, etc.)
#include <chrono>              // Time points and durations
#include <thread>              // std::this_thread::sleep_for
#include <sys/wait.h>          // waitpid, WIFEXITED, WEXITSTATUS (for popen/system)
#include <cctype>              // std::tolower
#include <archive.h>           // Libarchive for extraction
#include <archive_entry.h>     // Libarchive entry handling
#include <cstdio>              // popen, pclose, fgets, FILE*
#include <errno.h>             // errno variable
#include <cstring>             // strerror, strcmp
#include <sys/types.h>         // pid_t
#include <system_error>        // std::system_error
#include <stdexcept>           // Standard exceptions (runtime_error, etc.)
#include <random>              // std::random_device, std::mt19937, etc.
#include <algorithm>           // std::find, std::min, std::max, std::sort, std::transform, std::replace
#include <atomic>              // std::atomic_* types
#include <queue>               // std::queue
#include <functional>          // std::function

// Alias for easier filesystem usage
namespace fs = std::filesystem;

// Use the entire std::chrono namespace for convenience
using namespace std::chrono;

/**
 * ===========================================================================
 * Starpack Namespace
 * ===========================================================================
 *
 * Main namespace for all Starpack-related functionality.
 */
namespace Starpack {

    /**
     * =======================================================================
     * Anonymous Namespace for Internal Helpers
     * =======================================================================
     *
     * All internal helper objects and functions are limited in linkage scope
     * to this translation unit only.
     */
    namespace {

        // Keeps track of each repository URL and its associated local DB path.
        std::unordered_map<std::string, std::string> repoUrlToDbPath;

        /**
         * -------------------------------------------------------------------
         * generateTempFilename
         *
         * Creates a random temporary filename. If baseDir is empty, use the
         * system temporary directory.
         * -------------------------------------------------------------------
         */
        std::string generateTempFilename(const std::string &prefix, const std::string &baseDir) {
            fs::path tempDir = baseDir;
            if (baseDir.empty()) {
                tempDir = fs::temp_directory_path(); // Use the system temp by default
            }

            // Ensure the directory exists or fallback to "."
            if (!fs::exists(tempDir)) {
                try {
                    fs::create_directories(tempDir);
                } catch (const std::exception& e) {
                    std::cerr << "Warning: Could not create temp directory "
                              << tempDir.string() << ": " << e.what() << std::endl;
                    tempDir = "."; // Fallback to current directory
                }
            }

            // Create a random numeric suffix
            std::random_device rd;
            std::mt19937 mt(rd());
            std::uniform_int_distribution<unsigned long> dist(100000, 999999);
            std::string filename = prefix + "_" + std::to_string(dist(mt));

            return (tempDir / filename).string();
        }

        /**
         * -------------------------------------------------------------------
         * copy_data
         *
         * Helper function used by libarchive. Copies data from one archive
         * handle to another (e.g., from reading an archive to writing to disk).
         * -------------------------------------------------------------------
         */
        int copy_data(struct archive* ar, struct archive* aw) {
            const void* buff;
            size_t size;
            la_int64_t offset;
            int r;

            while (true) {
                r = archive_read_data_block(ar, &buff, &size, &offset);
                if (r == ARCHIVE_EOF) {
                    return ARCHIVE_OK; // End of the archive
                }

                if (r == ARCHIVE_RETRY) {
                    // Possibly re-try logic could be inserted here.
                    continue;
                }

                if (r != ARCHIVE_OK) {
                    std::cerr << "archive_read_data_block error: "
                              << archive_error_string(ar) << "\n";
                    return r;
                }

                // Write the retrieved block
                if (archive_write_data_block(aw, buff, size, offset) < ARCHIVE_OK) {
                    std::cerr << "archive_write_data_block error: "
                              << archive_error_string(aw) << "\n";
                    return ARCHIVE_FATAL;
                }
            }

            // Not reached
            return ARCHIVE_OK;
        }

        /**
         * -------------------------------------------------------------------
         * strip_path_components
         *
         * Strips the specified number of leading path components (e.g. "dir1/dir2/file"
         * -> "file" if stripComponents = 2).
         * -------------------------------------------------------------------
         */
        std::string strip_path_components(const std::string& path, int stripComponents) {
            if (stripComponents <= 0 || path.empty()) {
                return path;
            }

            fs::path p(path);
            fs::path result;
            auto it = p.begin();
            int count = 0;

            for (; it != p.end(); ++it) {
                // Skip '.' or empty path components
                if (it->string() == "." || it->string().empty()) {
                    continue;
                }

                if (count < stripComponents) {
                    count++;
                } else {
                    if (result.empty()) {
                        result = *it;
                    } else {
                        result /= *it;
                    }
                }
            }
            return result.string();
        }

        /**
         * -------------------------------------------------------------------
         * copyTreeRecursively
         *
         * Recursively copies the contents of 'src' into 'dst'.
         * Creates directories as needed, and overwrites existing files.
         * -------------------------------------------------------------------
         */
        static void copyTreeRecursively(const fs::path &src, const fs::path &dst) {
            if (!fs::exists(src) || !fs::is_directory(src)) {
                return; // Nothing to copy
            }

            // Create the destination directory if needed
            fs::create_directories(dst);

            for (auto &entry : fs::recursive_directory_iterator(src)) {
                const auto &pathInSrc = entry.path();
                auto relativePath     = fs::relative(pathInSrc, src);
                auto pathInDst        = dst / relativePath;

                std::error_code ec;
                if (entry.is_directory(ec)) {
                    fs::create_directories(pathInDst, ec);
                } else if (entry.is_regular_file(ec)) {
                    fs::create_directories(pathInDst.parent_path(), ec);
                    fs::copy_file(pathInSrc, pathInDst, fs::copy_options::overwrite_existing, ec);
                }

                if (ec) {
                    std::cerr << "Warning: Could not copy " << pathInSrc
                              << " to " << pathInDst << ": "
                              << ec.message() << std::endl;
                }
            }
        }

        /**
         * -------------------------------------------------------------------
         * extract_archive_section
         *
         * Extracts only the portion of an archive (specified by sectionPrefix)
         * to destDir, optionally stripping some number of leading components.
         * -------------------------------------------------------------------
         */
        int extract_archive_section(const std::string& archivePath,
                                    const std::string& sectionPrefix,
                                    const std::string& destDir,
                                    int stripComponents) {

            struct archive* a   = archive_read_new();        // for reading
            struct archive* ext = archive_write_disk_new();  // for writing to disk
            int result = 0; // 0 is ARCHIVE_OK in success sense

            if (!a || !ext) {
                std::cerr << "Error: archive_read_new or archive_write_disk_new failed.\n";
                if (a)  archive_read_free(a);
                if (ext) archive_write_free(ext);
                return -1;
            }

            // Extract flags: preserve time, permissions, ACL, flags, owner (if root)
            int extract_flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                ARCHIVE_EXTRACT_ACL  | ARCHIVE_EXTRACT_FFLAGS;
            if (geteuid() == 0) {
                extract_flags |= ARCHIVE_EXTRACT_OWNER;
            }

            archive_read_support_filter_all(a);
            archive_read_support_format_all(a);

            archive_write_disk_set_options(ext, extract_flags);
            archive_write_disk_set_standard_lookup(ext);

            // Open the archive file
            if (archive_read_open_filename(a, archivePath.c_str(), 32768) != ARCHIVE_OK) {
                std::cerr << "Error opening archive '" << archivePath << "': "
                          << archive_error_string(a) << "\n";
                result = -1;
                goto cleanup;
            }

            struct archive_entry* entry;
            int r;

            // Read each entry header
            while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {

                std::string origPath        = archive_entry_pathname(entry);
                std::string entryPathToProcess = origPath;

                // If sectionPrefix is non-empty, only process those that match
                if (!sectionPrefix.empty()) {
                    // Check if it starts with 'sectionPrefix'
                    if (entryPathToProcess.rfind(sectionPrefix, 0) == 0) {
                        // Remove the prefix so that only the remainder is extracted
                        entryPathToProcess = entryPathToProcess.substr(sectionPrefix.length());
                        if (!entryPathToProcess.empty() &&
                            (entryPathToProcess[0] == '/' || entryPathToProcess[0] == '\\')) {
                            entryPathToProcess.erase(0, 1);
                        }
                    } else {
                        // Skip if not in the specified section
                        archive_read_data_skip(a);
                        continue;
                    }
                }

                // If the path is now empty after removing the prefix, skip
                if (entryPathToProcess.empty()) {
                    archive_read_data_skip(a);
                    continue;
                }

                // Strip user-specified number of leading components
                std::string strippedRelativePath = strip_path_components(
                                                   entryPathToProcess,
                                                   std::max(0, stripComponents));
                if (strippedRelativePath.empty()) {
                    archive_read_data_skip(a);
                    continue;
                }

                // Build the destination path
                fs::path fullDestPath = fs::path(destDir) / strippedRelativePath;

                // Ensure the parent directory exists
                fs::path parent = fullDestPath.parent_path();
                if (!parent.empty() && !fs::exists(parent)) {
                    std::error_code ec;
                    fs::create_directories(parent, ec);
                    if (ec) {
                        std::cerr << "Warning: Failed to create directory "
                                  << parent.string() << ": " << ec.message() << "\n";
                    }
                }

                // Resolve conflicts if file type differs
                std::error_code ec_stat_before, ec_remove;
                auto existing_status = fs::symlink_status(fullDestPath, ec_stat_before);
                if (!ec_stat_before) {
                    // Compare existing to entry's type
                    mode_t entry_type   = archive_entry_filetype(entry);
                    bool entry_is_dir   = (entry_type == AE_IFDIR);
                    bool existing_is_dir = fs::is_directory(existing_status);

                    if (existing_is_dir != entry_is_dir) {
                        // Remove conflicting file or directory
                        std::cerr << "Warning: Path type conflict for "
                                  << fullDestPath.string()
                                  << ". Removing existing entry." << std::endl;
                        fs::remove_all(fullDestPath, ec_remove);
                        if (ec_remove) {
                            std::cerr << "Error: Failed to remove conflicting entry "
                                      << fullDestPath.string() << ": "
                                      << ec_remove.message()
                                      << ". Skipping extraction for this entry."
                                      << std::endl;
                            archive_read_data_skip(a);
                            result = -1;
                            continue;
                        }
                    }
                } else if (ec_stat_before.value() != ENOENT) {
                    // If we got an error other than "file not found"
                    std::cerr << "Warning: Could not stat "
                              << fullDestPath.string() << ": "
                              << ec_stat_before.message() << std::endl;
                }

                // Overwrite the archive entry's pathname with our new extracted path
                archive_entry_set_pathname(entry, fullDestPath.string().c_str());

                // Adjust any hardlink references to match the new sub-path
                const char* hl_target = archive_entry_hardlink(entry);
                if (hl_target) {
                    std::string hl_target_str(hl_target);

                    // Remove the sectionPrefix if present
                    if (!sectionPrefix.empty()) {
                        if (hl_target_str.rfind(sectionPrefix, 0) == 0) {
                            hl_target_str = hl_target_str.substr(sectionPrefix.length());
                            if (!hl_target_str.empty() &&
                                (hl_target_str[0] == '/' || hl_target_str[0] == '\\')) {
                                hl_target_str.erase(0, 1);
                            }
                        }
                    }

                    // Strip components from the hardlink target
                    std::string strippedTarget = strip_path_components(
                                                 hl_target_str,
                                                 std::max(0, stripComponents));
                    if (!strippedTarget.empty()) {
                        // Prepend the final target directory for a fully qualified path
                        fs::path fullLinkTarget = fs::path(destDir) / strippedTarget;
                        archive_entry_set_hardlink(entry, fullLinkTarget.string().c_str());
                    }
                }

                // Write header
                r = archive_write_header(ext, entry);
                if (r < ARCHIVE_OK) {
                    std::cerr << "Warning (archive_write_header for "
                              << fullDestPath.string() << "): "
                              << archive_error_string(ext) << "\n";
                    archive_read_data_skip(a);
                    if (r < ARCHIVE_WARN) {
                        result = -1;
                    }
                    continue;
                }

                // If it's a regular file with content, copy the data blocks
                if (archive_entry_filetype(entry) == AE_IFREG &&
                    archive_entry_size(entry) > 0) {
                    r = copy_data(a, ext);
                    if (r != ARCHIVE_EOF && r != ARCHIVE_OK && r != ARCHIVE_RETRY) {
                        std::cerr << "Error copying data for "
                                  << fullDestPath.string() << ": "
                                  << archive_error_string(ext)
                                  << " (read error: "
                                  << archive_error_string(a) << ")\n";
                        result = -1;
                    }
                } else {
                    // If it's not a regular file but has a non-zero size, skip it
                    if (archive_entry_size(entry) > 0) {
                        archive_read_data_skip(a);
                    }
                }

                // Finish the entry
                r = archive_write_finish_entry(ext);
                if (r < ARCHIVE_OK) {
                    std::cerr << "Warning (archive_write_finish_entry for "
                              << fullDestPath.string() << "): "
                              << archive_error_string(ext) << "\n";
                    if (r < ARCHIVE_WARN) {
                        result = -1;
                    }
                }
            }

            // If we didn't reach the end of the archive properly, mark it as failure
            if (r != ARCHIVE_EOF) {
                std::cerr << "Error reading archive header: "
                          << archive_error_string(a) << "\n";
                result = -1;
            }

        cleanup:
            archive_read_close(a);
            archive_read_free(a);
            archive_write_close(ext);
            archive_write_free(ext);

            return result;
        }

        /**
         * -------------------------------------------------------------------
         * WriteCallback
         *
         * cURL write callback for receiving data and writing directly to
         * an output file stream.
         * -------------------------------------------------------------------
         */
        size_t WriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
            std::ofstream* outFile = static_cast<std::ofstream*>(userdata);
            size_t totalSize = size * nmemb;

            if (outFile && outFile->is_open()) {
                outFile->write(static_cast<char*>(ptr), totalSize);
                if (!outFile->good()) {
                    std::cerr << "Error writing to download file stream!" << std::endl;
                    return 0; // Signal error to cURL
                }
            } else {
                // If the file stream is invalid but not null, we signal an error
                if (userdata != nullptr) {
                    std::cerr << "Error: Invalid file stream in WriteCallback!" << std::endl;
                    return 0; // Signal error to cURL
                }
            }
            return totalSize;
        }

        /**
         * -------------------------------------------------------------------
         * XferInfoCallback
         *
         * cURL transfer info callback for progress reporting (download
         * progress bar or numeric indicators).
         * -------------------------------------------------------------------
         */
        int XferInfoCallback(void* /*ptr*/,
                             curl_off_t totalToDownload,
                             curl_off_t nowDownloaded,
                             curl_off_t /*totalToUpload*/,
                             curl_off_t /*nowUploaded*/) {

            if (totalToDownload <= 0) {
                // Unknown or zero total size
                if (nowDownloaded > 0) {
                    double megaBytes = static_cast<double>(nowDownloaded) / (1024 * 1024);
                    std::cout << "\rDownloading... "
                              << std::fixed << std::setprecision(1)
                              << megaBytes << " MiB" << std::flush;
                } else {
                    std::cout << "\rDownloading... (size unknown)" << std::flush;
                }
                return 0;
            }

            // Build a simple text progress bar
            const int barWidth = 50;
            double progress = static_cast<double>(nowDownloaded)
                              / static_cast<double>(totalToDownload);
            if (progress > 1.0) {
                progress = 1.0; // Clamp to 100%
            }
            int pos = static_cast<int>(barWidth * progress);

            std::cout << "\r[";
            for (int i = 0; i < barWidth; ++i) {
                if (i < pos) {
                    std::cout << "=";
                } else if (i == pos) {
                    std::cout << ">";
                } else {
                    std::cout << " ";
                }
            }
            std::cout << "] "
                      << std::fixed << std::setprecision(1)
                      << (progress * 100.0) << "%"
                      << std::flush;

            return 0; // Return 0 to indicate success to cURL
        }

        /**
         * -------------------------------------------------------------------
         * parseVersion
         *
         * Splits a version string (e.g. "1.2.3") into numeric components
         * for simpler lexical comparisons.
         * -------------------------------------------------------------------
         */
        static std::vector<int> parseVersion(const std::string& ver) {
            std::vector<int> parts;
            std::stringstream ss(ver);
            std::string token;

            while (std::getline(ss, token, '.')) {
                try {
                    parts.push_back(std::stoi(token));
                } catch (...) {
                    // If parsing fails, default to 0
                    parts.push_back(0);
                }
            }
            return parts;
        }

        /**
         * -------------------------------------------------------------------
         * compareVersionSemantics
         *
         * Compare two dotted version strings numerically (like X.Y.Z).
         * Return -1 if v1 < v2, 0 if equal, 1 if v1 > v2.
         * -------------------------------------------------------------------
         */
        static int compareVersionSemantics(const std::string& v1, const std::string& v2) {
            auto p1 = parseVersion(v1);
            auto p2 = parseVersion(v2);
            size_t n = std::max(p1.size(), p2.size());

            for (size_t i = 0; i < n; ++i) {
                int c1 = (i < p1.size()) ? p1[i] : 0;
                int c2 = (i < p2.size()) ? p2[i] : 0;

                if (c1 < c2) return -1;
                if (c1 > c2) return 1;
            }
            return 0;
        }

        /**
         * -------------------------------------------------------------------
         * compareVersions
         *
         * Uses compareVersionSemantics() internally to compare two versions
         * according to a given operator (>, >=, <, <=, ==, !=).
         * -------------------------------------------------------------------
         */
        bool compareVersions(const std::string& version1,
                             const std::string& version2,
                             const std::string& operatorSymbol) {

            int res = compareVersionSemantics(version1, version2);

            if      (operatorSymbol == ">")   return (res > 0);
            else if (operatorSymbol == ">=")  return (res >= 0);
            else if (operatorSymbol == "<")   return (res < 0);
            else if (operatorSymbol == "<=")  return (res <= 0);
            else if (operatorSymbol == "==" || operatorSymbol == "=") return (res == 0);
            else if (operatorSymbol == "!=")  return (res != 0);

            std::cerr << "Warning: Unknown version comparison operator: '"
                      << operatorSymbol << "'\n";
            return false;
        }

        /**
         * -------------------------------------------------------------------
         * validateDependency
         *
         * Checks a dependency's version constraint (e.g., ">= 1.2.3") against
         * the actual version from a package node in the repository.
         * -------------------------------------------------------------------
         */
        bool validateDependency(const std::string& depName,
                                const std::string& versionConstraint,
                                const YAML::Node& availablePackageNode) {

            // Regex to parse operators and version: >, >=, <, <=, ==, etc.
            std::regex constraintRegex(R"(([><=]=?)\s*([\w\.\-\+~]+))");
            std::smatch match;
            std::string operatorSymbol = "=="; // Default: exact match
            std::string constraintVersion = versionConstraint;

            if (versionConstraint.empty()) {
                // No constraint means anything goes
                return true;
            }

            // Check for "!=" specially
            size_t neqPos = versionConstraint.find("!=");
            if (neqPos != std::string::npos) {
                operatorSymbol    = "!=";
                constraintVersion = versionConstraint.substr(neqPos + 2);
            } else if (std::regex_match(versionConstraint, match, constraintRegex)) {
                operatorSymbol    = match[1].str();
                constraintVersion = match[2].str();
            } else {
                // If no operator matched, treat entire string as the version for '==' comparison
                constraintVersion = versionConstraint;
            }

            // Trim whitespace
            constraintVersion.erase(0, constraintVersion.find_first_not_of(" \t"));
            constraintVersion.erase(constraintVersion.find_last_not_of(" \t") + 1);

            // Check for "version" field in the available package
            if (!availablePackageNode ||
                !availablePackageNode["version"] ||
                !availablePackageNode["version"].IsScalar()) {
                std::cerr << "Error: Cannot find available version for dependency '"
                          << depName << "' to validate constraint." << std::endl;
                return false;
            }
            std::string availableVersion = availablePackageNode["version"].as<std::string>();

            // Compare using compareVersions
            return compareVersions(availableVersion, constraintVersion, operatorSymbol);
        }

        /**
         * -------------------------------------------------------------------
         * printProgressBar
         *
         * A simple ASCII progress bar. The total helps gauge the fraction.
         * -------------------------------------------------------------------
         */
        void printProgressBar(size_t current, size_t total) {
            if (total == 0) return;

            float percent = static_cast<float>(current) / static_cast<float>(total);
            percent = std::min(percent, 1.0f);
            const int barWidth = 50;
            int pos = static_cast<int>(barWidth * percent);

            std::cout << "\rProgress: [";
            for (int i = 0; i < barWidth; i++) {
                if (i < pos) {
                    std::cout << "=";
                } else if (i == pos) {
                    std::cout << ">";
                } else {
                    std::cout << " ";
                }
            }
            std::cout << "] "
                      << std::fixed << std::setprecision(0)
                      << (percent * 100.0f) << "% ("
                      << current << "/" << total << ")"
                      << std::flush;

            if (current == total) {
                std::cout << std::endl;
            }
        }

        /**
         * -------------------------------------------------------------------
         * initializeDatabase
         *
         * Ensures the Starpack package database directory and file exist
         * inside the specified installation directory.
         * -------------------------------------------------------------------
         */
        void initializeDatabase(const std::string& installDir) {
            fs::path dbDir  = fs::path(installDir) / "var" / "lib" / "starpack";
            fs::path dbPath = dbDir / "installed.db";

            try {
                if (!fs::exists(dbDir)) {
                    std::cout << "Creating database directory: " << dbDir << std::endl;
                    fs::create_directories(dbDir);
                }
                if (!fs::exists(dbPath)) {
                    std::cout << "Creating empty database file: " << dbPath << std::endl;
                    std::ofstream dbFile(dbPath);
                    if (!dbFile) {
                        std::cerr << "Error: Failed to create database file at "
                                  << dbPath << std::endl;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "Error initializing database directory/file: "
                          << e.what() << std::endl;
            }
        }

        /**
         * -------------------------------------------------------------------
         * DownloadJob
         *
         * Structure that holds information about a download job for the
         * multi-file asynchronous download mechanism.
         * -------------------------------------------------------------------
         */
        struct DownloadJob {
            std::ofstream fileStream;
            CURL*         easyHandle = nullptr;
            std::string   url;
            std::string   outputPath;
            bool          success = false;
        };

    } // end anonymous namespace

    //========================================================================
    // Download Functions (Starpack namespace)
    //========================================================================

    /**
     * ------------------------------------------------------------------------
     * downloadSingleFileSync
     *
     * Downloads a single file using a synchronous cURL invocation. Returns
     * true if the file either already exists or downloads successfully.
     * ------------------------------------------------------------------------
     */
    bool downloadSingleFileSync(const std::string& url, const std::string& outputPath) {
        if (fs::exists(outputPath)) {
            // Already present, consider it "good"
            return true;
        }

        std::cout << "[Sync] Downloading: " << url << " -> " << outputPath << std::endl;

        fs::path outPathFs(outputPath);
        fs::path parentDir = outPathFs.parent_path();
        if (!parentDir.empty() && !fs::exists(parentDir)) {
            try {
                fs::create_directories(parentDir);
            } catch (const std::exception& e) {
                std::cerr << "[Sync] Error creating directory "
                          << parentDir.string() << ": " << e.what() << std::endl;
                return false;
            }
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "[Sync] Error: Failed to initialize curl." << std::endl;
            return false;
        }

        std::ofstream outFile(outputPath, std::ios::binary | std::ios::trunc);
        if (!outFile) {
            std::cerr << "[Sync] Error: Failed to open file for writing: "
                      << outputPath << std::endl;
            curl_easy_cleanup(curl);
            return false;
        }

        // Configure cURL
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outFile);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfoCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

        CURLcode res = curl_easy_perform(curl);
        long response_code = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        }

        curl_easy_cleanup(curl);
        outFile.close();
        std::cout << std::endl; // progress bar newline

        if (res != CURLE_OK) {
            std::cerr << "[Sync] Error downloading " << url << ": "
                      << curl_easy_strerror(res) << std::endl;
            fs::remove(outputPath);
            return false;
        }

        if (response_code >= 400) {
            std::cerr << "[Sync] Error downloading " << url << ": Server responded with code "
                      << response_code << std::endl;
            fs::remove(outputPath);
            return false;
        }

        return true;
    }

    /**
     * ------------------------------------------------------------------------
     * downloadMultipleFilesMulti
     *
     * Downloads multiple files asynchronously using cURL multi-interface.
     * Returns true if all files download successfully, false otherwise.
     * ------------------------------------------------------------------------
     */
    bool downloadMultipleFilesMulti(const std::vector<std::pair<std::string, std::string>>& filesToDownload) {
        if (filesToDownload.empty()) {
            return true;
        }

        CURLM* multiHandle = curl_multi_init();
        if (!multiHandle) {
            std::cerr << "[Multi Error] curl_multi_init failed!\n";
            return false;
        }

        std::map<CURL*, DownloadJob> jobs;
        std::atomic<bool> overallSuccess = {true};
        std::atomic<int>  completedCount = {0};
        size_t totalJobsAttempted       = 0;

        // Let’s limit concurrency to 10 or the number of files, whichever is smaller
        const int maxConcurrent = std::max(1, std::min((int)filesToDownload.size(), 10));
        int currentDownloads    = 0;
        size_t fileIndex        = 0;
        int stillRunning        = 0;

        // Main event loop
        do {
            // Add new transfers as long as we have capacity and unprocessed files
            while (currentDownloads < maxConcurrent && fileIndex < filesToDownload.size()) {
                const auto& url  = filesToDownload[fileIndex].first;
                const auto& path = filesToDownload[fileIndex].second;
                fileIndex++;
                totalJobsAttempted++;

                // If file already exists, skip
                if (fs::exists(path)) {
                    completedCount++;
                    continue;
                }

                // Make sure destination directory exists
                fs::path outPathFs(path);
                fs::path parentPath = outPathFs.parent_path();
                if (!parentPath.empty() && !fs::exists(parentPath)) {
                    try {
                        fs::create_directories(parentPath);
                    } catch (const std::exception& e) {
                        std::cerr << "[Multi Error] Creating directory "
                                  << parentPath.string() << " failed: "
                                  << e.what() << ". Skipping URL: " << url << std::endl;
                        overallSuccess = false;
                        completedCount++;
                        continue;
                    }
                }

                CURL* easyHandle = curl_easy_init();
                if (!easyHandle) {
                    std::cerr << "[Multi Error] curl_easy_init failed for URL: "
                              << url << ". Skipping." << std::endl;
                    overallSuccess = false;
                    completedCount++;
                    continue;
                }

                // Emplace a blank job first
                auto [it, success_emplace] =
                     jobs.emplace(easyHandle, DownloadJob{});
                if (!success_emplace) {
                    std::cerr << "[Multi Internal Error] Failed to emplace job for handle. "
                              << "Skipping URL: " << url << std::endl;
                    curl_easy_cleanup(easyHandle);
                    overallSuccess = false;
                    completedCount++;
                    continue;
                }

                // Reference the job stored in the map
                DownloadJob& job_in_map  = it->second;
                job_in_map.url           = url;
                job_in_map.outputPath    = path;
                job_in_map.easyHandle    = easyHandle;
                job_in_map.fileStream.open(path, std::ios::binary | std::ios::trunc);

                if (!job_in_map.fileStream) {
                    std::cerr << "[Multi Error] Failed to open file for writing: '"
                              << path << "'. Skipping URL: " << url << std::endl;
                    curl_easy_cleanup(easyHandle);
                    jobs.erase(it);
                    overallSuccess = false;
                    completedCount++;
                    continue;
                }

                // Set cURL options
                curl_easy_setopt(easyHandle, CURLOPT_URL, url.c_str());
                curl_easy_setopt(easyHandle, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(easyHandle, CURLOPT_WRITEDATA, &job_in_map.fileStream);
                curl_easy_setopt(easyHandle, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(easyHandle, CURLOPT_FAILONERROR, 1L);
                curl_easy_setopt(easyHandle, CURLOPT_PRIVATE, easyHandle);
                curl_easy_setopt(easyHandle, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(easyHandle, CURLOPT_XFERINFOFUNCTION, XferInfoCallback);
                curl_easy_setopt(easyHandle, CURLOPT_CONNECTTIMEOUT, 15L);
                curl_easy_setopt(easyHandle, CURLOPT_TIMEOUT, 300L);
                curl_easy_setopt(easyHandle, CURLOPT_USERAGENT, "Starpack/1.0");

                // Add handle to the multi stack
                CURLMcode mc = curl_multi_add_handle(multiHandle, easyHandle);
                if (mc != CURLM_OK) {
                    std::cerr << "[Multi Error] curl_multi_add_handle failed (" << mc
                              << "): " << curl_multi_strerror(mc)
                              << " for URL: " << url << ". Skipping." << std::endl;

                    if (job_in_map.fileStream.is_open()) {
                        job_in_map.fileStream.close();
                    }
                    fs::remove(path);
                    curl_easy_cleanup(easyHandle);
                    jobs.erase(it);
                    overallSuccess = false;
                    completedCount++;
                    continue;
                }

                // We have one more transfer in progress
                currentDownloads++;
            }

            // Perform the transfers
            CURLMcode mc_perf = curl_multi_perform(multiHandle, &stillRunning);
            if (mc_perf != CURLM_OK && mc_perf != CURLM_CALL_MULTI_PERFORM) {
                std::cerr << "[Multi Error] curl_multi_perform failed (" << mc_perf
                          << "): " << curl_multi_strerror(mc_perf) << std::endl;
            }

            // Check for completed transfers
            int msgsInQueue = 0;
            CURLMsg* msg;

            while ((msg = curl_multi_info_read(multiHandle, &msgsInQueue))) {
                if (msg->msg == CURLMSG_DONE) {
                    CURL* easyHandle = msg->easy_handle;
                    CURLcode result  = msg->data.result;
                    long response_code = 0;
                    double total_time  = 0;

                    auto it = jobs.find(easyHandle);
                    if (it != jobs.end()) {
                        DownloadJob& completedJob = it->second;

                        if (completedJob.fileStream.is_open()) {
                            completedJob.fileStream.close();
                        }

                        curl_easy_getinfo(easyHandle, CURLINFO_RESPONSE_CODE, &response_code);
                        curl_easy_getinfo(easyHandle, CURLINFO_TOTAL_TIME, &total_time);

                        if (result == CURLE_OK && response_code < 400) {
                            completedJob.success = true;
                        } else {
                            std::cout << std::endl; // new line for clarity
                            std::cerr << "[Multi Error] Failed download:\n"
                                      << "  URL : " << completedJob.url << "\n"
                                      << "  Path: " << completedJob.outputPath << std::endl;

                            if (result != CURLE_OK) {
                                std::cerr << "  Curl Error: "
                                          << curl_easy_strerror(result)
                                          << " (Code: " << result << ")\n";
                            }

                            if (response_code >= 400) {
                                std::cerr << "  HTTP Status: " << response_code << "\n";
                            }
                            std::cerr << "  Time: "
                                      << std::fixed << std::setprecision(2)
                                      << total_time << "s\n";

                            overallSuccess = false;
                            fs::remove(completedJob.outputPath);
                        }

                        curl_multi_remove_handle(multiHandle, easyHandle);
                        curl_easy_cleanup(easyHandle);
                        jobs.erase(it);
                        currentDownloads--;
                        completedCount++;
                    } else {
                        // We didn't find the handle in our map
                        std::cerr << "[Multi Internal Error] Completed handle not found in map!"
                                  << std::endl;
                        curl_multi_remove_handle(multiHandle, easyHandle);
                        curl_easy_cleanup(easyHandle);
                        currentDownloads--;
                        completedCount++;
                    }
                    // Clear progress line
                    std::cout << "\r" << std::string(80, ' ') << "\r" << std::flush;
                }
            }

            // If still running, or there's more to queue, wait for activity
            if (stillRunning > 0) {
                struct timeval timeout;
                fd_set fdread, fdwrite, fdexcep;
                int maxfd = -1;

                FD_ZERO(&fdread);
                FD_ZERO(&fdwrite);
                FD_ZERO(&fdexcep);

                timeout.tv_sec  = 0;
                timeout.tv_usec = 100 * 1000; // 100ms

                CURLMcode mc_fdset = curl_multi_fdset(multiHandle,
                                                      &fdread, &fdwrite, &fdexcep,
                                                      &maxfd);

                if (mc_fdset != CURLM_OK) {
                    std::cerr << "[Multi Error] curl_multi_fdset: "
                              << curl_multi_strerror(mc_fdset) << std::endl;
                }

                if (maxfd == -1) {
                    std::this_thread::sleep_for(milliseconds(100));
                } else {
                    int rc = select(maxfd + 1,
                                    &fdread, &fdwrite, &fdexcep, &timeout);
                    if (rc == -1) {
                        std::perror("[Multi Error] select failed");
                        overallSuccess = false;
                    }
                }
            } else if (fileIndex < filesToDownload.size()) {
                // If there's more to do but none are running, short sleep
                std::this_thread::sleep_for(milliseconds(10));
            }

        } while (stillRunning > 0 || completedCount < totalJobsAttempted);

        // Final cleanup check
        int msgsInQueue = 0;
        CURLMsg* msg;
        while ((msg = curl_multi_info_read(multiHandle, &msgsInQueue))) {
            if (msg->msg == CURLMSG_DONE) {
                CURL* easyHandle = msg->easy_handle;
                auto it = jobs.find(easyHandle);

                if (it != jobs.end()) {
                    DownloadJob& completedJob = it->second;
                    if (completedJob.fileStream.is_open()) {
                        completedJob.fileStream.close();
                    }
                    if (!completedJob.success) {
                        overallSuccess = false;
                        fs::remove(completedJob.outputPath);
                    }
                    curl_multi_remove_handle(multiHandle, easyHandle);
                    curl_easy_cleanup(easyHandle);
                    jobs.erase(it);
                } else {
                    curl_multi_remove_handle(multiHandle, easyHandle);
                    curl_easy_cleanup(easyHandle);
                }
            }
        }

        curl_multi_cleanup(multiHandle);
        std::cout << "\r" << std::string(80, ' ') << "\r";
        std::cout << "[Multi] Download processing finished." << std::endl;
        return overallSuccess;
    }

    //========================================================================
    // Installer Class Methods
    //========================================================================

    /**
     * ------------------------------------------------------------------------
     * portable_timegm
     *
     * Cross-platform (ish) replacement for timegm() to handle UTC conversions
     * from struct tm.
     * ------------------------------------------------------------------------
     */
    static std::time_t portable_timegm(std::tm* tm) {
        #if defined(_WIN32)
            // Windows version is _mkgmtime
            return _mkgmtime(tm);
        #elif defined(__USE_MISC) || (_XOPEN_SOURCE >= 600)
            // Many POSIX systems have timegm
            return timegm(tm);
        #else
            // Fallback method
            std::time_t local = std::mktime(tm);
            if (local == -1) {
                return -1;
            }
            // Adjust by the timezone
            return local - timezone;
        #endif
    }

    /**
     * ------------------------------------------------------------------------
     * Installer::parseUpdateDate
     *
     * Attempts to parse a date/time string in several formats (ISO8601, etc.).
     * ------------------------------------------------------------------------
     */
    std::time_t Installer::parseUpdateDate(const std::string& dateStr) {
        std::tm tm = {};
        std::istringstream ss(dateStr);

        // Try ISO 8601 (e.g. "2021-09-15T14:23:00Z")
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        if (ss.fail()) {
            // Fallback
            ss.clear(); ss.str(dateStr);
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            if (ss.fail()) {
                // Another fallback (RFC 822/1123)
                ss.clear(); ss.str(dateStr);
                ss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S %Z");
                if (ss.fail()) {
                    return 0; // Give up
                }
            }
        }
        return portable_timegm(&tm);
    }

    /**
     * ------------------------------------------------------------------------
     * Installer::getInstalledPackageUpdateDate
     *
     * Looks up a package in the local DB to retrieve the "Update-time" or
     * "Build-date" entry (returning it as a time_t).
     * ------------------------------------------------------------------------
     */
    std::time_t Installer::getInstalledPackageUpdateDate(const std::string& packageName,
                                                         const std::string& dbPath) {

        std::ifstream dbFile(dbPath);
        if (!dbFile.is_open()) {
            return 0;
        }

        std::string line;
        std::string searchHeader = packageName + " /";
        bool inPackageSection    = false;

        while (std::getline(dbFile, line)) {
            if (!inPackageSection) {
                // Check if we found the package block start
                if (line.rfind(searchHeader, 0) == 0) {
                    inPackageSection = true;
                }
            } else {
                // Already inside the correct package block
                if (line.rfind("Update-time:", 0) == 0) {
                    std::string dateStr = line.substr(std::string("Update-time:").length());
                    dateStr.erase(0, dateStr.find_first_not_of(" \t"));
                    dateStr.erase(dateStr.find_last_not_of(" \t") + 1);
                    return parseUpdateDate(dateStr);
                } else if (line.rfind("Build-date:", 0) == 0) {
                    std::string dateStr = line.substr(std::string("Build-date:").length());
                    dateStr.erase(0, dateStr.find_first_not_of(" \t"));
                    dateStr.erase(dateStr.find_last_not_of(" \t") + 1);
                    return parseUpdateDate(dateStr);
                } else if (line == "----------------------------------------") {
                    // End of this package block
                    return 0;
                }
            }
        }
        return 0;
    }

    /**
     * ------------------------------------------------------------------------
     * Installer::isPackageInstalled
     *
     * Checks if a given package is recorded in the local installed.db.
     * ------------------------------------------------------------------------
     */
    bool Installer::isPackageInstalled(const std::string& packageName,
                                       const std::string& installDir) {

        std::string dbPath = fs::path(installDir) /
                             "var" / "lib" / "starpack" /
                             "installed.db";

        if (!std::filesystem::exists(dbPath)) {
            return false;
        }

        std::ifstream dbFile(dbPath);
        if (!dbFile) {
            return false;
        }

        std::string line;
        std::string searchHeader = packageName + " /";

        while (std::getline(dbFile, line)) {
            if (line.rfind(searchHeader, 0) == 0) {
                return true;
            }
        }
        return false;
    }

    /**
     * ------------------------------------------------------------------------
     * Installer::getConfirmation
     *
     * Prompts the user to confirm installation of the provided package list.
     * ------------------------------------------------------------------------
     */
    bool Installer::getConfirmation(const std::vector<std::string>& packages) {
        if (packages.empty()) {
            std::cout << "Internal Info: No packages identified for installation action."
                      << std::endl;
            return true; // Nothing to confirm
        }

        std::cout << "\nThe following packages will be processed for installation:\n  ";
        for (size_t i = 0; i < packages.size(); ++i) {
            std::cout << packages[i]
                      << ((i < packages.size() - 1) ? " " : "");
        }
        std::cout << "\nProceed? [Y/n]: ";

        std::string response;
        std::getline(std::cin, response);

        // Clean up input
        std::string processedResponse;
        if (!response.empty()) {
            processedResponse = response;
            processedResponse.erase(0, processedResponse.find_first_not_of(" \t\n\r"));
            processedResponse.erase(processedResponse.find_last_not_of(" \t\n\r") + 1);
            std::transform(processedResponse.begin(),
                           processedResponse.end(),
                           processedResponse.begin(),
                           [](unsigned char c){ return std::tolower(c); });
        }

        if (processedResponse.empty() ||
            processedResponse == "y" ||
            processedResponse == "yes") {
            return true;
        } else {
            std::cout << "Aborting installation." << std::endl;
            return false;
        }
    }

    /**
     * ------------------------------------------------------------------------
     * Installer::createDatabaseEntry
     *
     * Appends the package information to the installed.db within the given
     * installation directory.
     * ------------------------------------------------------------------------
     */
    void Installer::createDatabaseEntry(const std::string& packageName,
                                        const std::string& installDir,
                                        const YAML::Node& packageNode) {

        fs::path dbDir  = fs::path(installDir) / "var" / "lib" / "starpack";
        fs::path dbPath = dbDir / "installed.db";

        try {
            if (!fs::exists(dbDir)) {
                fs::create_directories(dbDir);
            }

            std::ofstream dbFile(dbPath, std::ios::app | std::ios::binary);
            if (!dbFile) {
                throw std::runtime_error("Unable to open database file for writing: "
                                         + dbPath.string());
            }

            // Package block header
            dbFile << packageName << " /\n";

            // Lambda for writing a key/value if present
            auto write_scalar = [&](const std::string& key,
                                    const std::string& dbKey) {
                if (packageNode[key] && packageNode[key].IsScalar()) {
                    dbFile << dbKey << ": "
                           << packageNode[key].as<std::string>() << "\n";
                }
            };

            // Write fields if they exist
            write_scalar("version",     "Version");
            write_scalar("description", "Description");
            write_scalar("size",       "Size");
            write_scalar("arch",       "Architecture");

            // For date, prefer 'update_time', fallback to 'build_date'
            if (packageNode["update_time"] &&
                packageNode["update_time"].IsScalar()) {
                write_scalar("update_time", "Update-time");
            } else {
                write_scalar("build_date",  "Build-date");
            }

            // Files list
            if (packageNode["files"] &&
                packageNode["files"].IsSequence()) {
                dbFile << "Files:\n";
                for (const auto& fileNode : packageNode["files"]) {
                    if (fileNode.IsScalar()) {
                        std::string filePath = fileNode.as<std::string>();
                        if (filePath.empty()) {
                            continue;
                        }
                        if (filePath[0] != '/') {
                            filePath = "/" + filePath;
                        }
                        dbFile << filePath << "\n";
                    }
                }
            } else {
                std::cerr << "Warning: Missing 'files' list for package "
                          << packageName << " in DB entry.\n";
            }

            // Dependencies list
            if (packageNode["dependencies"] &&
                packageNode["dependencies"].IsSequence()) {
                dbFile << "Dependencies:\n";
                bool hasDeps = false;
                for (const auto& depNode : packageNode["dependencies"]) {
                    if (depNode.IsScalar()) {
                        std::string depStr = depNode.as<std::string>();
                        if (!depStr.empty()) {
                            dbFile << depStr << "\n";
                            hasDeps = true;
                        }
                    }
                }
            }

            // End block
            dbFile << "----------------------------------------\n";
            dbFile.flush();

        } catch (const YAML::Exception& e) {
            std::cerr << "YAML Error processing package node for "
                      << packageName << ": " << e.what() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error writing database entry for "
                      << packageName << ": " << e.what() << std::endl;
        }
    }

    /**
     * ------------------------------------------------------------------------
     * Installer::verifyGPGSignature
     *
     * Verifies a package file with the corresponding .sig file using gpg,
     * and attempts to automatically fetch missing keys if needed.
     * ------------------------------------------------------------------------
     */
    bool Installer::verifyGPGSignature(const std::string& packagePath,
                                       const std::string& sigPath,
                                       const std::string& installDir) {

        // Quick existence checks
        if (!fs::exists(sigPath)) {
            std::cerr << "Error: Missing signature file: " << sigPath << std::endl;
            return false;
        }
        if (!fs::exists(packagePath)) {
            std::cerr << "Error: Missing data file for signature verification: "
                      << packagePath << std::endl;
            return false;
        }

        // GPG key storage directories inside the target install
        fs::path baseDirPath(installDir);
        fs::path keysDir   = baseDirPath / "etc" / "starpack" / "keys";
        fs::path keyringFile = keysDir / "starpack.gpg";
        fs::path cacheDir  = baseDirPath / "var" / "lib" / "starpack" / "cache";

        // Ensure these directories exist
        try {
            if (!fs::exists(keysDir))  fs::create_directories(keysDir);
            if (!fs::exists(cacheDir)) fs::create_directories(cacheDir);
        } catch (const std::exception& e) {
            std::cerr << "Error ensuring GPG directories exist ("
                      << keysDir << ", " << cacheDir << "): "
                      << e.what() << std::endl;
            return false;
        }

        // Construct the GPG verify command, sending status to fd 1
        std::string command = "gpg --batch --no-tty --status-fd 1 "
                              "--no-default-keyring --keyring \"";
        command += keyringFile.string() + "\" --verify \""
                   + sigPath + "\" \"" + packagePath
                   + "\" 2>/dev/null";

        // Open a pipe to read GPG's stdout
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            std::perror("Error running popen for gpg verify");
            return false;
        }

        // Parse GPG status lines
        char buffer[256];
        std::string status_output;
        bool goodSig = false, badSig = false, expSig = false,
             expKey  = false, revKey = false;
        std::string missingKey, keyId;

        while (fgets(buffer, sizeof(buffer), pipe)) {
            status_output += buffer;
            std::string line = buffer;

            if      (line.rfind("[GNUPG:] GOODSIG", 0) == 0) {
                goodSig = true;
                std::istringstream iss(line);
                std::string tag, discard;
                iss >> tag >> discard >> keyId;
            }
            else if (line.rfind("[GNUPG:] BADSIG", 0) == 0)  badSig = true;
            else if (line.rfind("[GNUPG:] EXPSIG", 0) == 0)  expSig = true;
            else if (line.rfind("[GNUPG:] EXPKEYSIG", 0) == 0) expKey = true;
            else if (line.rfind("[GNUPG:] REVKEYSIG", 0) == 0) revKey = true;
            else if (line.rfind("[GNUPG:] NO_PUBKEY", 0) == 0) {
                // Capture missing key
                std::istringstream iss(line);
                std::string tag, discard;
                iss >> tag >> discard >> missingKey;
            }
        }

        int status = pclose(pipe);
        int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        // If it was a good signature on the first pass, success
        if (goodSig && exitCode == 0) {
            return true;
        }

        // Check for known negative statuses
        if (badSig) {
            std::cerr << "Error: GPG verification failed: BAD Signature." << std::endl;
            return false;
        }
        if (expSig) {
            std::cerr << "Error: GPG verification failed: Signature has EXPIRED." << std::endl;
            return false;
        }
        if (expKey) {
            std::cerr << "Error: GPG verification failed: Key is EXPIRED." << std::endl;
            return false;
        }
        if (revKey) {
            std::cerr << "Error: GPG verification failed: Key is REVOKED." << std::endl;
            return false;
        }

        // If we're missing a key, try to download it
        if (!missingKey.empty()) {
            std::cerr << "GPG Verification failed: Missing public key: "
                      << missingKey << std::endl;

            // Attempt to read /etc/starpack/repos.conf to get repository URLs
            std::ifstream confFile("/etc/starpack/repos.conf");
            std::vector<std::string> repoUrls;
            std::string repoLine;

            while (std::getline(confFile, repoLine)) {
                repoLine.erase(0, repoLine.find_first_not_of(" \t\n\r"));
                repoLine.erase(repoLine.find_last_not_of(" \t\n\r") + 1);

                if (repoLine.empty() || repoLine[0] == '#') {
                    continue;
                }
                if (repoLine.back() != '/') {
                    repoLine += '/';
                }
                repoUrls.push_back(repoLine);
            }

            if (repoUrls.empty()) {
                std::cerr << "Error: No repository URLs found in /etc/starpack/repos.conf "
                          << "to search for key." << std::endl;
                return false;
            }

            // Attempt to download the key
            std::string keyFileName = missingKey + ".asc";
            std::string tempKeyPath = generateTempFilename(missingKey, cacheDir.string());
            bool downloadedKey = false;

            for (const auto& currentRepoUrl : repoUrls) {
                std::string keyUrl = currentRepoUrl + "keys/" + keyFileName;
                std::cerr << "Attempting download: " << keyUrl << std::endl;
                if (downloadSingleFileSync(keyUrl, tempKeyPath)) {
                    downloadedKey = true;
                    break;
                } else {
                    std::error_code ec;
                    fs::remove(tempKeyPath, ec);
                }
            }

            if (!downloadedKey) {
                std::cerr << "Error: Failed to download key " << missingKey
                          << " from any repository." << std::endl;
                if (fs::exists(tempKeyPath)) {
                    fs::remove(tempKeyPath);
                }
                return false;
            }

            // Now import the key
            std::string importCommand = "gpg --batch --no-tty --no-default-keyring --keyring \"";
            importCommand += keyringFile.string();
            importCommand += "\" --import \"";
            importCommand += tempKeyPath;
            importCommand += "\" 2>/dev/null";

            std::cout << "Importing key: " << missingKey << "..." << std::endl;
            int importSysStatus = std::system(importCommand.c_str());
            std::error_code ec_rm_key;
            fs::remove(tempKeyPath, ec_rm_key);

            if (importSysStatus != 0) {
                std::cerr << "Error: Failed to import key: " << missingKey
                          << " (gpg import exit status: "
                          << importSysStatus << ")" << std::endl;
                return false;
            }

            std::cout << "Key imported successfully: " << missingKey << std::endl;
            std::cerr << "Re-verifying signature..." << std::endl;

            // Re-run the verification
            pipe = popen(command.c_str(), "r");
            if (!pipe) {
                std::perror("Error running popen for gpg re-verify");
                return false;
            }

            status_output.clear();
            goodSig = false;
            keyId.clear();

            while (fgets(buffer, sizeof(buffer), pipe)) {
                status_output += buffer;
                if (std::string(buffer).rfind("[GNUPG:] GOODSIG", 0) == 0) {
                    goodSig = true;
                    std::istringstream iss(buffer);
                    std::string discard;
                    iss >> discard >> discard >> keyId;
                }
            }

            status   = pclose(pipe);
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

            if (goodSig && exitCode == 0) {
                return true;
            } else {
                std::cerr << "Error: Signature verification still fails after key import: "
                          << missingKey << std::endl;
                return false;
            }
        }

        // If we reached this point, it's a general failure
        std::cerr << "Error: GPG signature verification failed (Unknown Reason)." << std::endl;
        return false;
    }

    // An alias for the dependency graph: package -> its dependencies
    using DependencyGraph = std::unordered_map<std::string, std::vector<std::string>>;

    /**
     * ------------------------------------------------------------------------
     * computeInstallationOrderCycleTolerant
     *
     * Uses a Kahn-like approach for topological sorting, but if cycles exist,
     * it includes those cyclical nodes at the end in alphabetical order.
     * Returns the final install ordering.
     * ------------------------------------------------------------------------
     */
    std::vector<std::string> computeInstallationOrderCycleTolerant(const DependencyGraph &graph) {
        std::unordered_map<std::string, int> inDegree;
        std::unordered_set<std::string> allPackages;

        // Gather all unique package names
        for (const auto &pair : graph) {
            allPackages.insert(pair.first);
            inDegree[pair.first] = 0;
            for (const auto &dep : pair.second) {
                allPackages.insert(dep);
                inDegree[dep] = 0;
            }
        }

        // Compute in-degrees
        for (const auto &pair : graph) {
            for (const auto &dependency : pair.second) {
                if (inDegree.count(dependency)) {
                    inDegree[dependency]++;
                }
            }
        }

        // Queue for items with 0 in-degree
        std::queue<std::string> zeroInDegreeQueue;
        for (const auto &pkgName : allPackages) {
            if (inDegree.at(pkgName) == 0) {
                zeroInDegreeQueue.push(pkgName);
            }
        }

        std::vector<std::string> installOrder;
        installOrder.reserve(allPackages.size());

        // Kahn's Algorithm
        while (!zeroInDegreeQueue.empty()) {
            std::string currentPkg = zeroInDegreeQueue.front();
            zeroInDegreeQueue.pop();

            installOrder.push_back(currentPkg);

            // Decrease in-degree for items that depend on currentPkg
            for (const auto &pair : graph) {
                const std::string& potentialDependent = pair.first;
                const auto& dependencies = pair.second;

                // If this dependent actually depends on currentPkg
                if (std::find(dependencies.begin(), dependencies.end(), currentPkg)
                    != dependencies.end()) {
                    if (inDegree.count(potentialDependent)) {
                        if (--inDegree[potentialDependent] == 0) {
                            zeroInDegreeQueue.push(potentialDependent);
                        }
                    }
                }
            }
        }

        // Check if we got all packages
        if (installOrder.size() < allPackages.size()) {
            // Identify cycle participants
            std::vector<std::string> cycleNodes;
            for (const auto &pkgName : allPackages) {
                if (inDegree.at(pkgName) > 0) {
                    cycleNodes.push_back(pkgName);
                }
            }

            std::sort(cycleNodes.begin(), cycleNodes.end());
            installOrder.insert(installOrder.end(),
                                cycleNodes.begin(), cycleNodes.end());

            if (installOrder.size() != allPackages.size()) {
                throw std::runtime_error("Installation order calculation failed: mismatch in package count.");
            }
        }
        return installOrder;
    }

    /**
     * ------------------------------------------------------------------------
     * Installer::installPackage
     *
     * The main method that orchestrates package installation, including
     * repository lookup, dependency resolution, file downloading, signature
     * verification, extraction, and hooking into Pre/Post install steps.
     * ------------------------------------------------------------------------
     */
    void Installer::installPackage(const std::vector<std::string>& initialPackageNames,
                                   const std::string& installDir,
                                   bool confirm) {

        std::cout << "--- Starpack Installation ---" << std::endl;
        std::cout << "Target directory: " << installDir << std::endl;

        // Ensure our local DB is set up
        initializeDatabase(installDir);

        // Data structures
        std::unordered_map<std::string, YAML::Node> repoDbCache;  // Not heavily used
        std::unordered_map<std::string,
                           std::pair<std::string, YAML::Node>> packageSourceCache;
        std::unordered_set<std::string> resolvedPackages;
        std::vector<std::string> repoUrls;
        std::vector<std::pair<std::string, std::string>> downloadTasks;

        // Step 1: Load repository URLs
        std::cout << "[1/8] Loading repository configuration..." << std::endl;
        fs::path repoConfPath = fs::path("/etc") / "starpack" / "repos.conf";

        std::ifstream confFile(repoConfPath);
        if (!confFile) {
            std::cerr << "Error: Failed to open repository config: "
                      << repoConfPath.string() << std::endl;
            return;
        }

        std::string currentRepoUrl;
        while (std::getline(confFile, currentRepoUrl)) {
            currentRepoUrl.erase(0, currentRepoUrl.find_first_not_of(" \t\n\r"));
            currentRepoUrl.erase(currentRepoUrl.find_last_not_of(" \t\n\r") + 1);
            if (currentRepoUrl.empty() || currentRepoUrl[0] == '#') {
                continue;
            }
            if (currentRepoUrl.back() != '/') {
                currentRepoUrl += '/';
            }
            if (std::find(repoUrls.begin(), repoUrls.end(), currentRepoUrl) == repoUrls.end()) {
                repoUrls.push_back(currentRepoUrl);
            }
        }
        confFile.close();

        if (repoUrls.empty()) {
            std::cerr << "Error: No valid repository URLs found in "
                      << repoConfPath.string() << "." << std::endl;
            return;
        }

        std::cout << "Found " << repoUrls.size() << " repository URL(s)." << std::endl;

        // Step 2: Prepare and download repo DBs
        std::cout << "[2/8] Checking/Downloading repository databases..." << std::endl;
        fs::path cacheDirPath = fs::path(installDir) /
                                "var" / "lib" / "starpack" / "cache";
        try {
            if (!fs::exists(cacheDirPath)) {
                fs::create_directories(cacheDirPath);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error creating cache directory "
                      << cacheDirPath.string() << ": " << e.what()
                      << ". Aborting." << std::endl;
            return;
        }
        std::string cacheDir = cacheDirPath.string();

        // Make a list of DB download tasks
        std::vector<std::pair<std::string, std::string>> dbDownloadTasks;
        for (const auto& repoUrl : repoUrls) {
            std::string repoDbUrl = repoUrl + "repo.db.yaml";

            std::string safeRepoName = repoUrl;
            std::replace(safeRepoName.begin(), safeRepoName.end(), '/', '_');
            std::replace(safeRepoName.begin(), safeRepoName.end(), ':', '_');
            std::string localDbFilename = safeRepoName + "repo.db.yaml";
            fs::path localDbPath = cacheDirPath / localDbFilename;

            // Record it in the global map
            repoUrlToDbPath[repoUrl] = localDbPath.string();

            dbDownloadTasks.push_back({repoDbUrl, localDbPath.string()});
        }

        // Download them
        if (!dbDownloadTasks.empty()) {
            if (!downloadMultipleFilesMulti(dbDownloadTasks)) {
                std::cerr << "Warning: One or more repository DB downloads failed. "
                          << "Installation may be incomplete." << std::endl;
            }
        }
        std::cout << "Repository database check/download complete." << std::endl;

        // Step 3: Parse the DBs and build the package cache
        std::cout << "[3/8] Loading repository databases..." << std::endl;
        for (const auto& repoUrl : repoUrls) {
            if (repoUrlToDbPath.find(repoUrl) == repoUrlToDbPath.end()) {
                std::cerr << "Warning: Internal error - missing path map for "
                          << repoUrl << std::endl;
                continue;
            }

            std::string localDbPath = repoUrlToDbPath[repoUrl];
            if (!fs::exists(localDbPath)) {
                std::cerr << "Error: Repository database file is missing: "
                          << localDbPath << "\n       Skipping repository: "
                          << repoUrl << std::endl;
                continue;
            }

            try {
                std::cout << " -> Loading packages from " << repoUrl << "..." << std::endl;
                YAML::Node currentDb = YAML::LoadFile(localDbPath);

                if (currentDb["packages"] && currentDb["packages"].IsSequence()) {
                    int count = 0;
                    for (const YAML::Node& pkgNode : currentDb["packages"]) {
                        if (pkgNode["name"] && pkgNode["name"].IsScalar()) {
                            std::string name = pkgNode["name"].as<std::string>();
                            if (packageSourceCache.find(name) == packageSourceCache.end()) {
                                packageSourceCache[name] = {repoUrl, YAML::Clone(pkgNode)};
                                count++;
                            }
                        }
                    }
                    std::cout << "    Loaded " << count << " package definitions." << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "Error parsing DB " << localDbPath << ": "
                          << e.what() << ". Skipping repo." << std::endl;
            }
        }

        if (packageSourceCache.empty()) {
            std::cerr << "Error: No packages found in any repository database."
                      << std::endl;
            return;
        }

        // Step 4: Resolve dependencies
        std::cout << "[4/8] Resolving dependencies..." << std::endl;
        std::unordered_set<std::string> requiredPackages;
        std::unordered_set<std::string> visitedForDeps;

        // Depth-first approach with a manual stack
        std::vector<std::string> resolutionStack = initialPackageNames;
        while (!resolutionStack.empty()) {
            std::string currentPkg = resolutionStack.back();
            resolutionStack.pop_back();

            if (visitedForDeps.count(currentPkg)) {
                continue;
            }
            visitedForDeps.insert(currentPkg);
            requiredPackages.insert(currentPkg);

            auto it = packageSourceCache.find(currentPkg);
            if (it != packageSourceCache.end()) {
                YAML::Node pkgNode = it->second.second;
                if (pkgNode["dependencies"] && pkgNode["dependencies"].IsSequence()) {
                    for (const auto &depNode : pkgNode["dependencies"]) {
                        if (depNode.IsScalar()) {
                            std::string depName = depNode.as<std::string>();
                            if (!visitedForDeps.count(depName)) {
                                resolutionStack.push_back(depName);
                            }
                        }
                    }
                }
            } else {
                // If the package isn't in the repo, check if it's installed
                if (!isPackageInstalled(currentPkg, installDir)) {
                    std::cerr << "Error: Dependency '" << currentPkg
                              << "' not in repos and not installed." << std::endl;
                    return;
                }
            }
        }

        // Build a graph of dependencies
        DependencyGraph depGraph;
        for (const auto &pkgName : requiredPackages) {
            depGraph[pkgName] = {};
        }
        for (const auto &pkgName : requiredPackages) {
            auto it = packageSourceCache.find(pkgName);
            if (it == packageSourceCache.end()) {
                // Possibly installed already
                continue;
            }

            YAML::Node pkgNode = it->second.second;
            if (pkgNode["dependencies"] && pkgNode["dependencies"].IsSequence()) {
                for (const auto &depNode : pkgNode["dependencies"]) {
                    if (depNode.IsScalar()) {
                        std::string depName = depNode.as<std::string>();
                        if (requiredPackages.count(depName)) {
                            depGraph[depName].push_back(pkgName);
                        }
                    }
                }
            }
        }

        std::vector<std::string> sortedPackages;
        try {
            sortedPackages = computeInstallationOrderCycleTolerant(depGraph);
        } catch (const std::exception &e) {
            std::cerr << "Error resolving dependencies: " << e.what() << std::endl;
            return;
        }

        // Filter out anything already installed
        std::vector<std::string> finalPackagesToInstall;
        for (const auto &pkgName : sortedPackages) {
            if (requiredPackages.count(pkgName) &&
                !isPackageInstalled(pkgName, installDir)) {
                finalPackagesToInstall.push_back(pkgName);
            }
        }

        if (finalPackagesToInstall.empty()) {
            std::cout << "All requested packages and dependencies are already installed."
                      << std::endl;
            return;
        }

        std::cout << "Packages requiring installation/update (in order): ";
        for (size_t i=0; i < finalPackagesToInstall.size(); ++i) {
            std::cout << finalPackagesToInstall[i]
                      << (i == finalPackagesToInstall.size()-1 ? "" : ", ");
        }
        std::cout << std::endl;

        // Step 4.5: confirmation
        if (confirm) {
            std::cout << "[Confirm] User confirmation required..." << std::endl;
            if (!Installer::getConfirmation(finalPackagesToInstall)) {
                return;
            }
            std::cout << "Confirmation received. Proceeding..." << std::endl;
        } else {
            std::cout << "[Confirm] Skipping confirmation prompt (--noconfirm used)."
                      << std::endl;
        }

        // Step 5: Prepare downloads for package archives + signatures
        downloadTasks.clear();
        for (const auto &pkgName : finalPackagesToInstall) {
            auto it = packageSourceCache.find(pkgName);
            if (it == packageSourceCache.end()) {
                std::cerr << "Internal Error: No source info found for required package '"
                          << pkgName << "'. Aborting." << std::endl;
                return;
            }

            YAML::Node pkgNode = it->second.second;
            if (!pkgNode["file_name"] || !pkgNode["file_name"].IsScalar()) {
                std::cerr << "Error: Missing 'file_name' in metadata for package '"
                          << pkgName << "'. Aborting." << std::endl;
                return;
            }

            std::string fileName = pkgNode["file_name"].as<std::string>();
            std::string fileUrl  = it->second.first + fileName;
            fs::path localPath   = cacheDirPath / fileName;

            if (!fs::exists(localPath)) {
                downloadTasks.push_back({ fileUrl, localPath.string() });
            }

            std::string sigUrl = fileUrl + ".sig";
            std::string sigLoc = localPath.string() + ".sig";
            if (!fs::exists(sigLoc)) {
                downloadTasks.push_back({ sigUrl, sigLoc });
            }
        }

        if (!downloadTasks.empty()) {
            std::cout << "[5/8] Downloading required package files and signatures..."
                      << std::endl;
            if (!downloadMultipleFilesMulti(downloadTasks)) {
                std::cerr << "Error: One or more package/signature downloads failed. "
                          << "Aborting installation." << std::endl;
                return;
            }
            std::cout << "Downloads complete." << std::endl;
        } else {
            std::cout << "[5/8] All required package files/signatures are already cached."
                      << std::endl;
        }

        // Step 6: Verify signatures
        std::cout << "[6/8] Verifying package signatures..." << std::endl;
        for (const auto& packageName : finalPackagesToInstall) {
            auto sourceCacheIt = packageSourceCache.find(packageName);
            if (sourceCacheIt == packageSourceCache.end()) {
                std::cerr << "Internal Error: Cache missing for '"
                          << packageName
                          << "' during verification. Aborting." << std::endl;
                return;
            }

            YAML::Node currentPackageNode = sourceCacheIt->second.second;
            if (!currentPackageNode["file_name"] ||
                !currentPackageNode["file_name"].IsScalar()) {
                std::cerr << "Internal Error: Missing 'file_name' for '"
                          << packageName
                          << "' during verification. Aborting."
                          << std::endl;
                return;
            }

            std::string fileName            = currentPackageNode["file_name"].as<std::string>();
            std::string packagePathInCache  = (cacheDirPath / fileName).string();
            std::string sigPathInCache      = packagePathInCache + ".sig";

            if (!fs::exists(packagePathInCache)) {
                std::cerr << "Error: Package file missing from cache after download: "
                          << packagePathInCache << ". Aborting." << std::endl;
                return;
            }
            if (!fs::exists(sigPathInCache)) {
                std::cerr << "Error: Signature file missing from cache after download: "
                          << sigPathInCache << ". Aborting." << std::endl;
                return;
            }

            std::cout << " -> Verifying " << packageName << "..." << std::flush;
            if (!verifyGPGSignature(packagePathInCache, sigPathInCache, installDir)) {
                std::cerr << "Error: Signature verification failed for: "
                          << packageName << ". Aborting." << std::endl;
                return;
            }
            std::cout << " OK" << std::endl;
        }
        std::cout << "All package signatures verified successfully." << std::endl;


        // Step 7: Install packages (extract, hooks, DB update)
        std::cout << "[7/8] Installing packages..." << std::endl;
        size_t totalToInstall = finalPackagesToInstall.size();

        // Storage for running PostInstall hooks afterwards
        std::vector<std::pair<std::string, std::vector<std::string>>> postInstallHooksData;

        for (size_t i = 0; i < totalToInstall; ++i) {
            const std::string &packageName = finalPackagesToInstall[i];
            std::cout << "\n(" << (i + 1) << "/" << totalToInstall
                      << ") Installing " << packageName << "..." << std::endl;

            if (isPackageInstalled(packageName, installDir)) {
                std::cout << "   Skipping already installed package: "
                          << packageName << std::endl;
                printProgressBar(i + 1, totalToInstall);
                continue;
            }

            auto sourceCacheIt = packageSourceCache.find(packageName);
            YAML::Node currentPackageNode = sourceCacheIt->second.second;
            std::string fileName          = currentPackageNode["file_name"].as<std::string>();
            std::string packagePathInCache = (cacheDirPath / fileName).string();

            // PreInstall hooks
            std::cout << " -> Running PreInstall hooks..." << std::endl;
            Hook::runNewStyleHooks("PreInstall",
                                   "Install",
                                   {},
                                   installDir,
                                   packageName);

            // Extraction
            std::cout << " -> Extracting package files..." << std::endl;
            int stripComponents = 0;
            if (currentPackageNode["strip_components"] &&
                currentPackageNode["strip_components"].IsScalar()) {
                try {
                    stripComponents = currentPackageNode["strip_components"].as<int>();
                } catch (...) { //blank so it doesn't complain
                }
            }

            if (extract_archive_section(packagePathInCache,
                                        "files/",
                                        installDir,
                                        std::max(0, stripComponents)) != 0) {
                std::cerr << "Error: Failed file extraction for package: "
                          << packageName << ". Aborting." << std::endl;
                return;
            }

            // /etc/skel logic
            std::cout << " -> Copying /etc/skel contents if present..." << std::endl;
            fs::path skelDir = fs::path(installDir) / "etc" / "skel";
            if (fs::exists(skelDir) && fs::is_directory(skelDir)) {
                fs::path rootDir = fs::path(installDir) / "root";
                copyTreeRecursively(skelDir, rootDir);

                fs::path homeDir = fs::path(installDir) / "home";
                if (fs::exists(homeDir) && fs::is_directory(homeDir)) {
                    for (auto &userHome : fs::directory_iterator(homeDir)) {
                        if (userHome.is_directory()) {
                            copyTreeRecursively(skelDir, userHome.path());
                        }
                    }
                }
            } else {
                std::cout << "    (/etc/skel directory not present or invalid; skipping)"
                          << std::endl;
            }

            // Hooks extraction
            std::cout << " -> Installing hooks..." << std::endl;
            std::string tempHooksExtractDir = generateTempFilename(packageName + "_hooks_",
                                                                   cacheDir);
            int extractResult = extract_archive_section(packagePathInCache,
                                                        "hooks/",
                                                        tempHooksExtractDir,
                                                        std::max(0, stripComponents));

            if (extractResult == 0) {
                fs::path hooksSourceDir(tempHooksExtractDir);
                std::error_code ec_stat;

                if (fs::is_directory(hooksSourceDir, ec_stat) && !ec_stat) {
                    fs::path packageHooksDestBase = fs::path(installDir) /
                                                    "etc" / "starpack" / "hooks";
                    fs::path packageHooksDestDir  = packageHooksDestBase /
                                                    packageName;
                    bool hooksFoundInDir = false;

                    try {
                        fs::create_directories(packageHooksDestDir);
                        for (const auto& entry : fs::directory_iterator(hooksSourceDir)) {
                            std::error_code ec_file_stat;
                            if (entry.is_regular_file(ec_file_stat) &&
                                !ec_file_stat &&
                                entry.path().extension() == ".hook") {

                                hooksFoundInDir = true;
                                fs::path srcPath  = entry.path();
                                fs::path destPath = packageHooksDestDir / srcPath.filename();

                                try {
                                    fs::copy(srcPath,
                                             destPath,
                                             fs::copy_options::overwrite_existing);
                                    std::cout << "   - Installed hook: "
                                              << destPath.filename().string()
                                              << std::endl;
                                } catch (const std::exception& copy_e) {
                                    std::cerr << "   - Error installing hook "
                                              << srcPath.filename().string()
                                              << ": " << copy_e.what()
                                              << std::endl;
                                }
                            } else if (ec_file_stat) {
                                std::cerr << "   - Warning: Could not stat "
                                          << entry.path().string() << ": "
                                          << ec_file_stat.message() << std::endl;
                            }
                        }
                        if (!hooksFoundInDir) {
                            std::cout << "   - No .hook files found in extracted hooks directory."
                                      << std::endl;
                        }
                    } catch (const std::exception& dir_e) {
                        std::cerr << "Error processing extracted hooks directory "
                                  << hooksSourceDir.string() << ": "
                                  << dir_e.what() << std::endl;
                    }
                } else {
                    if (ec_stat) {
                        std::cerr << "   - Warning: Could not stat extracted hooks dir "
                                  << hooksSourceDir.string() << ": "
                                  << ec_stat.message() << std::endl;
                    } else {
                        std::cout << "   - Extracted hooks directory is empty or invalid."
                                  << std::endl;
                    }
                }
            } else {
                std::cerr << "   - Warning: Failed to extract hooks section for "
                          << packageName
                          << " (archive might not contain hooks)."
                          << std::endl;
            }

            // Clean up hooks temp dir
            std::error_code ec_rm;
            if (fs::exists(tempHooksExtractDir)) {
                fs::remove_all(tempHooksExtractDir, ec_rm);
                if(ec_rm) {
                    std::cerr << "Warning: Failed to remove temporary hook directory "
                              << tempHooksExtractDir
                              << ": " << ec_rm.message() << std::endl;
                }
            }

            // Collect installed file paths for PostInstall hook
            std::vector<std::string> installedPathsForHook;
            if (currentPackageNode["files"] && currentPackageNode["files"].IsSequence()) {
                for (const auto &fileNode : currentPackageNode["files"]) {
                    if (fileNode.IsScalar()) {
                        std::string relPath = fileNode.as<std::string>();
                        if (!relPath.empty() && relPath[0] == '/') {
                            relPath = relPath.substr(1);
                        }
                        if (!relPath.empty()) {
                            installedPathsForHook.push_back(relPath);
                        }
                    }
                }
            }

            // Update the local DB
            std::cout << " -> Updating installation database..." << std::endl;
            createDatabaseEntry(packageName, installDir, currentPackageNode);

            // Record data for post-install hooks
            postInstallHooksData.push_back({ packageName, installedPathsForHook });

            // Done with this package
            std::cout << " -> Finished installing " << packageName << std::endl;
            printProgressBar(i + 1, totalToInstall);
        }

        // Step 7.5: PostInstall hooks
        std::cout << "\n[7.5/8] Running PostInstall hooks for all installed packages..." << std::endl;
        size_t totalHooksExecuted = 0;

        for (const auto& data : postInstallHooksData) {
            const std::string &pkgName        = data.first;
            const std::vector<std::string> &installedPaths = data.second;

            size_t hooksExecutedForPackage = Hook::runNewStyleHooks(
                "PostInstall",
                "Install",
                installedPaths,
                installDir,
                pkgName
            );

            // Only print a note if something actually ran
            if (hooksExecutedForPackage > 0) {
                std::cout << " -> Finished PostInstall hooks for package: "
                          << pkgName << " ("
                          << hooksExecutedForPackage << " hook(s) executed)"
                          << std::endl;
                totalHooksExecuted += hooksExecutedForPackage;
            }
        }

        // Step 8: Done
        std::cout << "[8/8] Installation process finished." << std::endl;
        std::cout << "--- Installation Complete ---" << std::endl;
    }

} // namespace Starpack
