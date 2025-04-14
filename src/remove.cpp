#include "remove.hpp"      // Functions definitions
#include "hook.hpp"
#include "install.hpp"     // Starpack::Installer::isPackageInstalled
#include <chroot_util.hpp> // Starpack::ChrootUtil support

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <sstream>
#include <unordered_set>
#include <algorithm>
#include <random>
#include <deque>
#include <stdexcept>
#include <cstdlib>
#include <yaml-cpp/yaml.h> // YAML parser support for helper functions

namespace fs = std::filesystem;

// ============================================================================
// Anonymous Namespace - Internal Constants and Helpers
// ============================================================================
namespace {
    // List of packages critical to system stability; refusing to remove them.
    const std::unordered_set<std::string> criticalPackages = {
        "glibc", "linux", "coreutils", "bash", "systemd", "util-linux",
        "linux-zen", "linux-api-headers", "dracut", "linux-zen-headers", "sh"
    };

    // Witty or cautionary messages for removing critical packages.
    const std::vector<std::string> criticalMessages = {
        "Hey! Psst! Look up what removing {pkg} will do to your system.",
        "This is NOT the French language pack. ({pkg})",
        "Are you sure you're not trying to uninstall the operating system? ({pkg})",
        "Removing {pkg} will end your computing career.",
        "{pkg} is holding your system together... barely.",
        "Whoever told you to remove {pkg} hates you with a passion.",
        "Don't do it! Seriously, just don't. ({pkg})",
        "{pkg}? Really?",
        "How about we dont delete {pkg}? Hm?"
    };

    // Special message if user attempts to remove Starpack itself.
    const std::string starpackRemovalMessage =
        "Removing Me? That's like tearing out the very soul of your system. I can't believe you'd do something like this!";

    bool isPackageInstalledInternal(const std::string& packageName, const std::string& dbPath)
    {
        if (!fs::exists(dbPath)) {
            std::cerr << "Warning: Database file " << dbPath << " does not exist.\n";
            return false;
        }
        std::ifstream dbFile(dbPath);
        if (!dbFile.is_open()) {
            std::cerr << "Error: Unable to open the database file for reading: " << dbPath << std::endl;
            return false;
        }

        std::string line;
        while (std::getline(dbFile, line)) {
            // Check if line starts with the package name + " /"
            if (line.rfind(packageName + " /", 0) == 0) {
                return true;
            }
        }
        return false;
    }
} // end anonymous namespace

// ============================================================================
// Starpack Namespace
// ============================================================================
namespace Starpack {

/**
 * Checks if the given package name is in the critical list.
 */
bool isCriticalPackage(const std::string& packageName)
{
    return (criticalPackages.find(packageName) != criticalPackages.end());
}

/**
 * Provides a random cautionary message for removing a critical package.
 * Uses a simple index history to avoid repeating recent messages.
 */
std::string getRandomCriticalMessage(const std::string& packageName)
{
    if (packageName == "starpack") {
        return starpackRemovalMessage;
    }

    // Static objects for random selection and history
    static std::mt19937 gen(std::random_device{}());
    static std::vector<std::string> shuffledMessages = criticalMessages;
    static std::deque<int> history;       // Indices of recently used messages
    static const size_t historySize = 5;  // Limit message history to 5

    // Collect possible indices that are not in the recent history
    std::vector<int> possibleIndices;
    for (int i = 0; i < (int)shuffledMessages.size(); ++i) {
        bool inHistory = false;
        for (int histIdx : history) {
            if (i == histIdx) {
                inHistory = true;
                break;
            }
        }
        if (!inHistory) {
            possibleIndices.push_back(i);
        }
    }

    int newMessageIndex = 0;
    if (!possibleIndices.empty()) {
        std::uniform_int_distribution<> dist(0, (int)possibleIndices.size() - 1);
        newMessageIndex = possibleIndices[dist(gen)];
    } else if (!shuffledMessages.empty()) {
        // If everything is in history, pick randomly from them anyway
        std::uniform_int_distribution<> dist(0, (int)shuffledMessages.size() - 1);
        newMessageIndex = dist(gen);
    } else {
        // fallback
        return "Critical package warning for " + packageName + "!";
    }

    // Update the history
    history.push_back(newMessageIndex);
    if (history.size() > historySize) {
        history.pop_front();
    }

    // Substitute {pkg} in the selected message
    std::string message = shuffledMessages[newMessageIndex];
    size_t pos = message.find("{pkg}");
    if (pos != std::string::npos) {
        message.replace(pos, 5, packageName);
    }
    return message;
}

std::vector<std::string> getReverseDependencies(const std::string& packageName, const std::string& dbPath)
{
    std::ifstream dbFile(dbPath);
    if (!dbFile.is_open()) {
        throw std::runtime_error("Error: Unable to open the database file: " + dbPath);
    }

    std::vector<std::string> reverseDependencies;
    std::string line;
    std::string currentPackage;
    bool inDependenciesSection = false;

    while (std::getline(dbFile, line)) {
        // Identify package header in the DB
        size_t headerPos = line.find(" /");
        if (headerPos != std::string::npos && headerPos == line.length() - 2) {
            currentPackage = line.substr(0, headerPos);
            inDependenciesSection = false;
        } else if (line == "Dependencies:") {
            if (!currentPackage.empty()) {
                inDependenciesSection = true;
            }
        } else if (inDependenciesSection) {
            // Trim the line
            std::string trimmedLine = line;
            trimmedLine.erase(0, trimmedLine.find_first_not_of(" \t"));
            trimmedLine.erase(trimmedLine.find_last_not_of(" \t") + 1);

            if (trimmedLine == packageName) {
                reverseDependencies.push_back(currentPackage);
            } else if (line == "----------------------------------------") {
                // End of package block
                inDependenciesSection = false;
                currentPackage.clear();
            }
        } else if (line == "----------------------------------------") {
            // End of package block
            inDependenciesSection = false;
            currentPackage.clear();
        }
    }
    return reverseDependencies;
}


std::vector<std::string> getOrphanedDependencies(const std::string& dbPath, const std::string& excludingPackage)
{
    std::ifstream dbFile(dbPath);
    if (!dbFile.is_open()) {
        throw std::runtime_error("Error: Unable to open the database file: " + dbPath);
    }

    std::unordered_set<std::string> allInstalledPackages;
    std::unordered_map<std::string, std::vector<std::string>> packageDependencies;

    std::string line;
    std::string currentPackage;
    bool inDependenciesSection = false;

    // 1) Gather all packages and their dependencies
    while (std::getline(dbFile, line)) {
        size_t headerPos = line.find(" /");
        if (headerPos != std::string::npos && headerPos == line.length() - 2) {
            currentPackage = line.substr(0, headerPos);
            allInstalledPackages.insert(currentPackage);
            packageDependencies[currentPackage] = {};
            inDependenciesSection = false;
        } else if (!currentPackage.empty() && line == "Dependencies:") {
            inDependenciesSection = true;
        } else if (inDependenciesSection) {
            std::string trimmedLine = line;
            trimmedLine.erase(0, trimmedLine.find_first_not_of(" \t"));
            trimmedLine.erase(trimmedLine.find_last_not_of(" \t") + 1);

            if (!trimmedLine.empty() && trimmedLine != "----------------------------------------") {
                packageDependencies[currentPackage].push_back(trimmedLine);
            } else if (line == "----------------------------------------") {
                inDependenciesSection = false;
                currentPackage.clear();
            }
        } else if (line == "----------------------------------------") {
            inDependenciesSection = false;
            currentPackage.clear();
        }
    }

    // 2) Determine which packages are required by others
    std::unordered_set<std::string> requiredDependencies;
    for (const auto& pair : packageDependencies) {
        const std::string& pkg = pair.first;
        // Skip dependencies for the package we’re removing
        if (pkg == excludingPackage) {
            continue;
        }
        for (const std::string& dep : pair.second) {
            requiredDependencies.insert(dep);
        }
    }

    // 3) Orphan = installed but not required by any other package
    std::vector<std::string> orphanedPackages;
    for (const std::string& installedPkg : allInstalledPackages) {
        if (installedPkg == excludingPackage) {
            continue;
        }
        if (requiredDependencies.find(installedPkg) == requiredDependencies.end()) {
            // It's not required by anything; treat it as orphan
            orphanedPackages.push_back(installedPkg);
        }
    }
    return orphanedPackages;
}

std::vector<std::string> getFilesToRemove(const std::string& packageName, const std::string& dbPath)
{
    std::ifstream dbFile(dbPath);
    if (!dbFile.is_open()) {
        throw std::runtime_error("Error: Unable to open the database file: " + dbPath);
    }

    std::vector<std::string> files;
    std::string line;
    bool inPackageSection = false;
    bool inFilesSection = false;

    while (std::getline(dbFile, line)) {
        if (line.rfind(packageName + " /", 0) == 0) {
            inPackageSection = true;
            inFilesSection = false;
            continue;
        }
        if (inPackageSection) {
            if (line == "Files:") {
                inFilesSection = true;
                continue;
            }
            if (line == "Dependencies:" || line == "----------------------------------------") {
                inFilesSection = false;
                if (line == "----------------------------------------") {
                    inPackageSection = false;
                }
                continue;
            }
            if (inFilesSection) {
                if (!line.empty() && line[0] == '/') {
                    files.push_back(line);
                } else if (!line.empty() && line.find_first_not_of(" \t") != std::string::npos) {
                    std::cerr << "Warning: Unexpected line in Files section for package '"
                              << packageName << "' in " << dbPath << ": "
                              << line << std::endl;
                }
            }
        }
    }
    return files;
}

void removeFiles(const std::vector<std::string>& filesToRemove, const std::string& installDir)
{
    // Sort descending by path length so deeper directories come first
    std::vector<std::string> sortedFiles = filesToRemove;
    std::sort(sortedFiles.begin(), sortedFiles.end(),
              [](const std::string& a, const std::string& b) {
                  return a.length() > b.length();
              });

    // First pass: remove files and empty directories
    for (const auto& fileRelPath : sortedFiles) {
        // Basic check to prevent escaping
        if (fileRelPath.find("..") != std::string::npos) {
            std::cerr << "Warning: Skipping potentially unsafe path: " << fileRelPath << std::endl;
            continue;
        }
        // Remove leading '/'
        std::string cleanedRelPath = fileRelPath;
        if (!cleanedRelPath.empty() && cleanedRelPath[0] == '/') {
            cleanedRelPath = cleanedRelPath.substr(1);
        }
        if (cleanedRelPath.empty()) {
            continue;
        }

        fs::path absPath = fs::path(installDir) / cleanedRelPath;
        try {
            if (!fs::exists(fs::symlink_status(absPath))) {
                std::cerr << "Warning: File listed in DB not found, cannot remove: "
                          << absPath.string() << std::endl;
                continue;
            }

            if (fs::is_directory(fs::symlink_status(absPath))) {
                // Remove empty directories
                if (fs::is_empty(absPath)) {
                    fs::remove(absPath);
                    std::cout << "Removed directory: " << absPath.string() << std::endl;
                } else {
                    std::cout << "Skipping non-empty directory (may contain other files): "
                              << absPath.string() << std::endl;
                }
            } else {
                fs::remove(absPath);
                std::cout << "Removed: " << absPath.string() << std::endl;
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Error removing path: " << absPath.string()
                      << " - " << e.what() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Unexpected error removing path: " << absPath.string()
                      << " - " << e.what() << std::endl;
        }
    }

    // Second pass: remove directories that might now be empty
    std::sort(sortedFiles.begin(), sortedFiles.end(),
              [](const std::string& a, const std::string& b) {
                  return a.length() < b.length();
              });

    for (const auto& fileRelPath : sortedFiles) {
        std::string cleanedRelPath = fileRelPath;
        if (!cleanedRelPath.empty() && cleanedRelPath[0] == '/') {
            cleanedRelPath = cleanedRelPath.substr(1);
        }
        if (cleanedRelPath.empty()) {
            continue;
        }
        fs::path absPath = fs::path(installDir) / cleanedRelPath;

        try {
            if (fs::exists(fs::symlink_status(absPath)) &&
                fs::is_directory(fs::symlink_status(absPath)) &&
                fs::is_empty(absPath))
            {
                fs::remove(absPath);
                std::cout << "Removed now-empty directory: "
                          << absPath.string() << std::endl;
            }
        } catch (const std::exception& e) {
        }
    }
}

void updateDatabase(const std::string& packageName, const std::string& dbPath)
{
    fs::path dbFilePath(dbPath);
    fs::path tempDbFilePath = dbFilePath;
    tempDbFilePath += ".tmp"; // Create a .tmp suffix

    std::ifstream dbFile(dbFilePath);
    if (!dbFile.is_open()) {
        if (!fs::exists(dbFilePath.parent_path())) {
            try {
                fs::create_directories(dbFilePath.parent_path());
                std::cerr << "Warning: Database file " << dbPath
                          << " did not exist. Created directory.\n";
                return; // Nothing else to remove since there's no DB content
            } catch (const std::exception& e) {
                throw std::runtime_error("Error: Unable to create database directory: " +
                                         dbFilePath.parent_path().string() + " - " + e.what());
            }
        } else {
            throw std::runtime_error("Error: Unable to open database file: " + dbPath);
        }
    }

    std::ofstream tempDbFile(tempDbFilePath, std::ios::trunc);
    if (!tempDbFile.is_open()) {
        throw std::runtime_error("Error: Unable to create temporary DB file: " + tempDbFilePath.string());
    }

    std::string line;
    bool skipSection = false;
    std::string headerLine = packageName + " /";

    // Copy lines that do not belong to the target package's block
    while (std::getline(dbFile, line)) {
        if (line == headerLine) {
            skipSection = true;
        } else if (skipSection && line == "----------------------------------------") {
            skipSection = false;
        } else if (!skipSection) {
            tempDbFile << line << '\n';
        }
    }

    dbFile.close();
    tempDbFile.close();

    // Replace the original DB file with the temp file
    try {
        fs::rename(tempDbFilePath, dbFilePath);
    } catch (const fs::filesystem_error& e) {
        fs::remove(tempDbFilePath); // Cleanup temp file
        throw std::runtime_error("Error: Failed to update DB file '" +
                                 dbPath + "'. Reason: " + e.what());
    }
    std::cout << "Database " << dbPath << " updated (removed entry for "
              << packageName << ").\n";
}

void removePackages(const std::vector<std::string>& packageNames,
                    const std::string& dbPath,
                    bool force,
                    const std::string& installDir)
{
    std::vector<std::string> successfullyRemoved;
    std::unordered_set<std::string> processedPackages;
    std::deque<std::string> removalQueue(packageNames.begin(), packageNames.end());
    size_t initialCount = packageNames.size();

    while (!removalQueue.empty()) {
        std::string currentPackage = removalQueue.front();
        removalQueue.pop_front();

        if (processedPackages.count(currentPackage)) {
            continue;
        }
        processedPackages.insert(currentPackage);

        std::cout << "--- Processing removal for: " << currentPackage << " ---\n";

        // A) Basic checks
        if (currentPackage == "starpack") {
            std::cerr << "Warning: " << getRandomCriticalMessage(currentPackage)
                      << "\nSkipping removal of 'starpack'.\n";
            continue;
        }
        if (isCriticalPackage(currentPackage)) {
            std::cerr << "Error: Attempted to remove critical package '"
                      << currentPackage << "'\n"
                      << getRandomCriticalMessage(currentPackage) << std::endl;
            continue;
        }

        // Check if installed (via official or internal method)
        if (!Starpack::Installer::isPackageInstalled(currentPackage, installDir)) {
            bool explicitlyRequested = false;
            for (size_t i = 0; i < initialCount; ++i) {
                if (packageNames[i] == currentPackage) {
                    explicitlyRequested = true;
                    break;
                }
            }
            if (explicitlyRequested) {
                std::cerr << "Error: Package '" << currentPackage
                          << "' is not installed.\n";
            }
            continue;
        }

        // B) If not forced, check reverse deps that aren’t also being removed
        if (!force) {
            auto revDeps = getReverseDependencies(currentPackage, dbPath);
            std::vector<std::string> blocking;
            for (const auto& rd : revDeps) {
                bool alsoRemoving = false;
                // Check initial requested removal
                for (const auto& pkg : packageNames) {
                    if (pkg == rd) {
                        alsoRemoving = true;
                        break;
                    }
                }
                // Check processed (covers orphans)
                if (processedPackages.count(rd)) {
                    alsoRemoving = true;
                }
                if (!alsoRemoving) {
                    blocking.push_back(rd);
                }
            }
            if (!blocking.empty()) {
                std::cerr << "Error: Cannot remove '" << currentPackage
                          << "' because it is required by these installed packages:\n";
                for (const auto& b : blocking) {
                    std::cerr << "  - " << b << "\n";
                }
                std::cerr << "Removal of '" << currentPackage << "' skipped. "
                          << "Use --force to override.\n";
                continue;
            }
        }

        // C) Gather the files belonging to this package
        std::vector<std::string> packageFiles = getFilesToRemove(currentPackage, dbPath);
        std::vector<std::string> relativePaths = packageFiles;
        for (auto &path : relativePaths) {
            if (!path.empty() && path[0] == '/') {
                path.erase(0, 1);
            }
        }

        // D) Run PreRemove hooks
        std::cout << "Running PreRemove hooks for " << currentPackage << "...\n";
        Hook::runNewStyleHooks("PreRemove", "Remove", relativePaths, installDir, currentPackage);

        // E) Remove the files
        std::cout << "Removing files for package: " << currentPackage << "...\n";
        removeFiles(packageFiles, installDir);

        // F) Update the database
        try {
            updateDatabase(currentPackage, dbPath);
            successfullyRemoved.push_back(currentPackage);
            std::cout << "Package '" << currentPackage << "' processing complete.\n";
        } catch (const std::exception& e) {
            std::cerr << "Error updating DB after removing " << currentPackage
                      << ": " << e.what() << "\nDatabase may be inconsistent.\n";
            // Decide whether to proceed
            continue;
        }

        // G) Run PostRemove hooks
        std::cout << "Running PostRemove hooks for " << currentPackage << "...\n";
        Hook::runNewStyleHooks("PostRemove", "Remove", relativePaths, installDir, currentPackage);

        // H) Check for orphaned dependencies
        auto orphans = getOrphanedDependencies(dbPath, currentPackage);
        if (!orphans.empty()) {
            std::cout << "Potential orphaned dependencies after removing "
                      << currentPackage << ":\n";
            for (const auto& dep : orphans) {
                if (processedPackages.find(dep) == processedPackages.end()) {
                    bool inQueue = false;
                    for (const auto& q : removalQueue) {
                        if (q == dep) {
                            inQueue = true;
                            break;
                        }
                    }
                    if (!inQueue) {
                        std::cout << "  - Adding '" << dep << "' to removal queue.\n";
                        removalQueue.push_back(dep);
                    }
                }
            }
        }
    }

    // Final summary
    if (successfullyRemoved.empty() && !packageNames.empty()) {
        std::cout << "No packages were removed.\n";
    } else if (!successfullyRemoved.empty()) {
        std::cout << "--- Removal Summary ---\n"
                  << "Successfully removed:\n";
        for (const auto& pkg : successfullyRemoved) {
            std::cout << "  - " << pkg << "\n";
        }
        std::cout << "-----------------------\n";
    }
}


void removePackages(const std::vector<std::string>& packageNames,
                    const std::string& dbPath,
                    bool force)
{
    removePackages(packageNames, dbPath, force, "/");
}

} // end namespace Starpack
