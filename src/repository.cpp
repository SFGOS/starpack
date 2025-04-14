#include "repository.hpp"
#include "utils.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <map>
#include <yaml-cpp/yaml.h>
#include <cstdlib>
#include <sstream>
#include <string>
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <ctime>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Starpack {

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Splits a path string by '/' into a vector of components.
 */
std::vector<std::string> splitPath(const std::string& path)
{
    std::vector<std::string> components;
    std::istringstream iss(path);
    std::string token;

    while (std::getline(iss, token, '/')) {
        if (!token.empty()) {
            components.push_back(token);
        }
    }
    return components;
}

/**
 * @brief Computes how many leading path components all given paths share.
 *        e.g., ["foo/bar/file", "foo/bar/docs"] => 2 ("foo","bar").
 */
int getCommonPrefixComponentCount(const std::vector<std::string>& paths)
{
    if (paths.empty()) {
        return 0;
    }
    auto commonComponents = splitPath(paths[0]);

    for (size_t i = 1; i < paths.size(); ++i) {
        auto comps = splitPath(paths[i]);
        size_t newSize = std::min(commonComponents.size(), comps.size());
        size_t j = 0;

        for (; j < newSize; ++j) {
            if (commonComponents[j] != comps[j]) {
                break;
            }
        }
        commonComponents.resize(j);
        if (commonComponents.empty()) {
            break;
        }
    }

    return static_cast<int>(commonComponents.size());
}

/**
 * @brief Uses libarchive to list the archive's entries, determines the number
 *        of common leading path components, and returns an integer to use in
 *        e.g. `strip_components`.
 */
int getStripComponents(const std::string& packagePath)
{
    std::vector<std::string> paths;
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, packagePath.c_str(), 10240) != ARCHIVE_OK) {
        std::cerr << "Error: Could not open archive " << packagePath << "\n";
        archive_read_free(a);
        return 0;
    }

    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string entryName = archive_entry_pathname(entry);
        if (!entryName.empty()) {
            paths.push_back(entryName);
        }
        archive_read_data_skip(a);
    }
    archive_read_close(a);
    archive_read_free(a);

    int count = getCommonPrefixComponentCount(paths);
    // Arbitrary logic: if there's exactly 1 shared component, we set 2; otherwise we use count
    return (count == 1) ? 2 : count;
}

/**
 * @brief Formats a filesystem time_point into "HH:MM:SS".
 */
std::string formatTimestamp(const fs::file_time_type& ftime)
{
    // Convert fs time to system_clock
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
    std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);

    char buffer[9]; // "HH:MM:SS" + null terminator
    std::tm* tm_info = std::localtime(&cftime);
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", tm_info);

    return std::string(buffer);
}

/**
 * @brief Fetches the last modification time of the .starpack archive, returning
 *        it as a string "HH:MM:SS".
 */
std::string getArchiveUpdateTime(const std::string& packagePath)
{
    try {
        auto ftime = fs::last_write_time(packagePath);
        return formatTimestamp(ftime);
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error getting archive update time: " << e.what() << "\n";
        return "";
    }
}

/**
 * @brief Extracts a single file (e.g., "metadata.yaml") from the tar/gz archive
 *        to the specified directory using libarchive.
 */
bool extractFileFromArchive(const std::string& archivePath,
                            const std::string& targetEntry,
                            const std::string& extractDir)
{
    struct archive* a = archive_read_new();
    struct archive_entry* entry;
    archive_read_support_format_tar(a);
    archive_read_support_filter_gzip(a);

    if (archive_read_open_filename(a, archivePath.c_str(), 10240) != ARCHIVE_OK) {
        std::cerr << "Error: Could not open archive " << archivePath << "\n";
        archive_read_free(a);
        return false;
    }

    bool found = false;
    fs::create_directories(extractDir);

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string entryName = archive_entry_pathname(entry);
        if (entryName == targetEntry || entryName == "./" + targetEntry) {
            std::string outputPath = extractDir + "/" + targetEntry;
            std::ofstream outputFile(outputPath, std::ios::binary);

            const void* buff;
            size_t size;
            int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                outputFile.write(static_cast<const char*>(buff), size);
            }
            outputFile.close();
            found = true;
            break;
        }
    }

    archive_read_close(a);
    archive_read_free(a);
    return found;
}

/**
 * @brief Extracts an entire "files" directory from the tar/gz archive
 *        into `extractDir`, preserving the path structure. Symlinks
 *        and directories are handled appropriately.
 */
bool extractDirectoryFromArchive(const std::string& archivePath,
                                 const std::string& targetDir,
                                 const std::string& extractDir)
{
    struct archive* a = archive_read_new();
    struct archive_entry* entry;
    archive_read_support_format_tar(a);
    archive_read_support_filter_gzip(a);

    if (archive_read_open_filename(a, archivePath.c_str(), 10240) != ARCHIVE_OK) {
        std::cerr << "Error: Could not open archive " << archivePath << "\n";
        archive_read_free(a);
        return false;
    }

    fs::create_directories(extractDir);

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string entryName = archive_entry_pathname(entry);

        // We only want entries that start with "files" or "./files"
        bool hasFilesPrefix = (entryName.rfind("files", 0) == 0) ||
                              (entryName.rfind("./files", 0) == 0);
        if (!hasFilesPrefix) {
            continue;
        }

        // If entry starts with "./", remove it
        std::string adjustedEntry = (entryName.rfind("./", 0) == 0)
                                    ? entryName.substr(2)
                                    : entryName;
        std::string outputPath = extractDir + "/" + adjustedEntry;

        if (archive_entry_filetype(entry) == AE_IFDIR) {
            fs::create_directories(outputPath);
        } else if (archive_entry_filetype(entry) == AE_IFLNK) {
            fs::create_directories(fs::path(outputPath).parent_path());
            std::string symlinkTarget = archive_entry_symlink(entry);

            // Remove existing symlink/file first
            if (fs::exists(outputPath) || fs::is_symlink(outputPath)) {
                fs::remove(outputPath);
            }
            try {
                fs::create_symlink(symlinkTarget, outputPath);
            } catch (const fs::filesystem_error& e) {
                std::cerr << "Error creating symlink: " << e.what() << std::endl;
            }
        } else if (archive_entry_filetype(entry) == AE_IFREG) {
            fs::create_directories(fs::path(outputPath).parent_path());
            std::ofstream outputFile(outputPath, std::ios::binary);

            const void* buff;
            size_t size;
            int64_t blockOffset;
            while (archive_read_data_block(a, &buff, &size, &blockOffset) == ARCHIVE_OK) {
                outputFile.write(static_cast<const char*>(buff), size);
            }
            outputFile.close();
        }
    }

    archive_read_close(a);
    archive_read_free(a);
    return true;
}

// ============================================================================
// Repository Method Implementations
// ============================================================================

/**
 * @brief Creates a repo.db.yaml index from all *.starpack archives in `location`.
 *        Extracts metadata.yaml and files/ to gather package info.
 */
void Repository::createRepoIndex(const std::string& location)
{
    // Validate directory
    if (!fs::exists(location) || !fs::is_directory(location)) {
        std::cerr << "Error: Directory does not exist or is invalid: "
                  << location << std::endl;
        return;
    }

    fs::path repoLocationPath(location);
    fs::path dbPath = repoLocationPath / "repo.db.yaml";
    YAML::Emitter out;

    out << YAML::BeginMap;
    out << YAML::Key << "packages" << YAML::Value << YAML::BeginSeq;

    // Base cache directory for extracting package data
    fs::path baseCacheDir = "/var/lib/cache/starpack_extract_repo_idx";
    std::error_code ec_cache;
    fs::create_directories(baseCacheDir, ec_cache);
    if (ec_cache) {
        std::cerr << "Error: Could not create base cache directory: "
                  << baseCacheDir << " - " << ec_cache.message() << std::endl;
        return;
    }

    // Iterate all .starpack files in location
    for (const auto& entry : fs::directory_iterator(location)) {
        if (entry.path().extension() == ".starpack") {
            fs::path packagePath = entry.path();
            fs::path fileName = entry.path().filename();
            std::cout << "Processing package: " << packagePath.string() << std::endl;

            // Create a unique temp directory for this package
            fs::path tempDir = baseCacheDir / fileName.stem();
            std::error_code ec;
            fs::remove_all(tempDir, ec); // Clean up from previous run
            fs::create_directories(tempDir, ec);
            if (ec) {
                std::cerr << "Error: Failed to create temporary directory "
                          << tempDir << ": " << ec.message() << std::endl;
                continue;
            }

            // Extract metadata.yaml
            if (!extractFileFromArchive(packagePath.string(), "metadata.yaml", tempDir.string())) {
                std::cerr << "Error: Failed to extract metadata.yaml from "
                          << packagePath.string() << std::endl;
                fs::remove_all(tempDir, ec);
                continue;
            }

            // Extract files directory
            fs::path extractedFilesDir = tempDir / "files";
            if (!extractDirectoryFromArchive(packagePath.string(),
                                             "files",
                                             tempDir.string()))
            {
                std::cerr << "Error: Failed to extract files directory from "
                          << packagePath.string() << std::endl;
                // We continue, but file list will be empty.
            }

            // Read metadata.yaml, build a repo entry
            try {
                fs::path metadataPath = tempDir / "metadata.yaml";
                YAML::Node metadata = YAML::LoadFile(metadataPath.string());

                out << YAML::BeginMap;

                // Name
                std::string pkgName = metadata["name"].as<std::string>();
                pkgName = removeSlashAndAfter(pkgName);
                out << YAML::Key << "name" << YAML::Value << pkgName;

                // Version
                out << YAML::Key << "version"
                    << YAML::Value << metadata["version"].as<std::string>();

                // Description
                out << YAML::Key << "description"
                    << YAML::Value << metadata["description"].as<std::string>();

                // file_name
                out << YAML::Key << "file_name"
                    << YAML::Value << fileName.string();

                // Dependencies
                if (metadata["dependencies"] &&
                    metadata["dependencies"].IsSequence())
                {
                    out << YAML::Key << "dependencies" << YAML::Value << YAML::BeginSeq;
                    for (const auto& dep : metadata["dependencies"]) {
                        if (dep.IsScalar()) {
                            std::string depStr = dep.as<std::string>();
                            depStr = removeSlashAndAfter(depStr);
                            out << depStr;
                        }
                    }
                    out << YAML::EndSeq;
                } else {
                    out << YAML::Key << "dependencies"
                        << YAML::Value << YAML::BeginSeq << YAML::EndSeq;
                }

                // strip_components
                int stripComponents = getStripComponents(packagePath.string());
                out << YAML::Key << "strip_components"
                    << YAML::Value << stripComponents;

                // Files list
                out << YAML::Key << "files" << YAML::Value << YAML::BeginSeq;
                if (fs::exists(extractedFilesDir) && fs::is_directory(extractedFilesDir)) {
                    try {
                        for (const auto& fileEntry : fs::recursive_directory_iterator(
                                 extractedFilesDir, fs::directory_options::skip_permission_denied))
                        {
                            std::error_code ec_status;
                            auto status = fileEntry.symlink_status(ec_status);

                            if (!ec_status) {
                                // We'll list both regular files and symlinks
                                if (fs::is_regular_file(status) || fs::is_symlink(status)) {
                                    // Compute relative path from 'files' subdir
                                    std::error_code ec_rel;
                                    fs::path relativePath = fs::relative(
                                        fileEntry.path(),
                                        extractedFilesDir,
                                        ec_rel
                                    );

                                    if (!ec_rel) {
                                        out << relativePath.generic_string();
                                    } else {
                                        std::cerr << "Warning: Could not get relative path for "
                                                  << fileEntry.path().string()
                                                  << ": " << ec_rel.message() << std::endl;
                                    }
                                }
                            } else {
                                std::cerr << "Warning: Could not get status for "
                                          << fileEntry.path().string()
                                          << ": " << ec_status.message() << std::endl;
                            }
                        }
                    } catch (const std::exception& fs_ex) {
                        std::cerr << "Error iterating extracted files in "
                                  << extractedFilesDir.string()
                                  << ": " << fs_ex.what() << std::endl;
                    }
                } else {
                    if (!fs::exists(tempDir / "files")) {
                        std::cerr << "Info: No 'files' directory found or extracted for "
                                  << packagePath.string() << std::endl;
                    }
                }
                out << YAML::EndSeq; // End 'files' key

                out << YAML::EndMap; // End package map
            }
            catch (const YAML::Exception& e) {
                std::cerr << "Error: Failed to parse metadata.yaml for "
                          << packagePath.string() << ": "
                          << e.what() << std::endl;
            }
            catch (const std::exception& e) {
                std::cerr << "Error processing metadata for "
                          << packagePath.string() << ": "
                          << e.what() << std::endl;
            }

            // Cleanup
            fs::remove_all(tempDir, ec);
            if (ec) {
                std::cerr << "Warning: Failed to remove temporary directory "
                          << tempDir << ": " << ec.message() << std::endl;
            }
        }
    }

    out << YAML::EndSeq; // End "packages"
    out << YAML::EndMap; // End root map

    std::ofstream dbFile(dbPath.string());
    if (dbFile) {
        dbFile << out.c_str();
        dbFile.close();
        std::cout << "Repository database created at: " << dbPath.string() << std::endl;
    } else {
        std::cerr << "Error: Failed to write repo.db.yaml to "
                  << dbPath.string() << std::endl;
    }
}

/**
 * @brief Checks for *.starpack files missing from 'repo.db.yaml' and adds them
 *        to the index by extracting metadata.yaml and files.
 */
void Repository::addMissingPackagesToIndex(const std::string& location)
{
    std::string dbPath = location + "/repo.db.yaml";
    YAML::Node index;

    // Load existing index if present, otherwise create a new one
    if (fs::exists(dbPath)) {
        try {
            index = YAML::LoadFile(dbPath);
        } catch (const std::exception& e) {
            std::cerr << "Error loading existing index: " << e.what() << std::endl;
            index = YAML::Node(YAML::NodeType::Map);
        }
    } else {
        index = YAML::Node(YAML::NodeType::Map);
    }

    if (!index["packages"]) {
        index["packages"] = YAML::Node(YAML::NodeType::Sequence);
    }

    // Build a set of already indexed package file names
    std::unordered_set<std::string> indexedPackages;
    for (const auto& pkg : index["packages"]) {
        if (pkg["file_name"]) {
            std::string fileName = pkg["file_name"].as<std::string>();
            indexedPackages.insert(fileName);
        }
    }

    // Check for new *.starpack files in 'location'
    for (const auto& entry : fs::directory_iterator(location)) {
        if (entry.path().extension() == ".starpack") {
            std::string fileName = entry.path().filename().string();
            if (indexedPackages.find(fileName) != indexedPackages.end()) {
                // Already in the index
                continue;
            }

            std::string packagePath = entry.path().string();
            std::cout << "Adding missing package: " << packagePath << std::endl;

            // Create a temporary extraction directory
            std::string tempDir = "/tmp/starpack_extract";
            fs::remove_all(tempDir);
            fs::create_directory(tempDir);

            // Extract metadata.yaml
            if (!extractFileFromArchive(packagePath, "metadata.yaml", tempDir)) {
                std::cerr << "Error: Failed to extract metadata.yaml from "
                          << packagePath << std::endl;
                fs::remove_all(tempDir);
                continue;
            }

            // Extract files directory
            if (!extractDirectoryFromArchive(packagePath, "files", tempDir)) {
                std::cerr << "Error: Failed to extract files directory from "
                          << packagePath << std::endl;
                fs::remove_all(tempDir);
                continue;
            }

            // Construct a YAML node for the new package
            YAML::Node pkgEntry;
            try {
                YAML::Node metadata = YAML::LoadFile(tempDir + "/metadata.yaml");

                std::string pkgName = metadata["name"].as<std::string>();
                pkgName = removeSlashAndAfter(pkgName);
                pkgEntry["name"] = pkgName;
                pkgEntry["version"]     = metadata["version"].as<std::string>();
                pkgEntry["description"] = metadata["description"].as<std::string>();
                pkgEntry["file_name"]   = fileName;

                if (metadata["dependencies"]) {
                    YAML::Node depsNode(YAML::NodeType::Sequence);
                    for (const auto& dep : metadata["dependencies"]) {
                        std::string depStr = dep.as<std::string>();
                        depStr = removeSlashAndAfter(depStr);
                        depsNode.push_back(depStr);
                    }
                    pkgEntry["dependencies"] = depsNode;
                }

                // update_dirs (optional)
                if (metadata["update_dirs"]) {
                    pkgEntry["update_dirs"] = metadata["update_dirs"];
                }

                // Archive update time
                std::string updateTime = getArchiveUpdateTime(packagePath);
                if (!updateTime.empty()) {
                    pkgEntry["update_time"] = updateTime;
                }

                // Build file list from extracted "files" directory
                YAML::Node filesNode(YAML::NodeType::Sequence);
                std::string filesPath = tempDir + "/files";
                if (fs::exists(filesPath) && fs::is_directory(filesPath)) {
                    for (const auto& file : fs::recursive_directory_iterator(filesPath)) {
                        std::string relativePath = fs::relative(file.path(), filesPath).string();
                        filesNode.push_back(relativePath);
                    }
                } else {
                    std::cerr << "Warning: No 'files' directory found in package: "
                              << packagePath << std::endl;
                }
                pkgEntry["files"] = filesNode;

                // Determine strip_components
                int stripComponents = getStripComponents(packagePath);
                pkgEntry["strip_components"] = stripComponents;

            } catch (const std::exception& e) {
                std::cerr << "Error: Failed to parse metadata.yaml for "
                          << packagePath << ": " << e.what() << std::endl;
                fs::remove_all(tempDir);
                continue;
            }

            // Add the new package entry
            index["packages"].push_back(pkgEntry);
            fs::remove_all(tempDir);
        }
    }

    // Write updated index to file
    std::ofstream dbFile(dbPath);
    if (dbFile) {
        YAML::Emitter out;
        out << index;
        dbFile << out.c_str();
        dbFile.close();
        std::cout << "Repository database updated at: " << dbPath << std::endl;
    } else {
        std::cerr << "Error: Failed to write updated repo.db.yaml to "
                  << dbPath << std::endl;
    }
}

} // namespace Starpack
