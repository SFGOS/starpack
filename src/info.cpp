#include "info.hpp"

#include <iostream>
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <curl/curl.h>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

// ============================================================================
// writeToString (libcurl callback)
// ============================================================================
static size_t writeToString(void* ptr, size_t size, size_t nmemb, std::string* data)
{
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// ============================================================================
// PackageInfo Constructor
// ============================================================================
PackageInfo::PackageInfo(const std::string& name,
                         const std::string& version,
                         const std::string& description,
                         const std::vector<std::string>& dependencies,
                         const std::map<std::string, std::string>& files)
    : name(name),
      version(version),
      description(description),
      dependencies(dependencies),
      files(files)
{
}

// ============================================================================
// Accessor functions
// ============================================================================
std::string PackageInfo::getName() const
{
    return name;
}

std::string PackageInfo::getVersion() const
{
    return version;
}

std::string PackageInfo::getDescription() const
{
    return description;
}

std::vector<std::string> PackageInfo::getDependencies() const
{
    return dependencies;
}

std::map<std::string, std::string> PackageInfo::getFiles() const
{
    return files;
}

// ============================================================================
// Display package information
// ============================================================================
void PackageInfo::display() const
{
    std::cout << "Package Name: " << name << "\n";
    std::cout << "Version: " << version << "\n";
    std::cout << "Description: " << description << "\n";

    std::cout << "Dependencies: ";
    for (const auto& dep : dependencies) {
        std::cout << dep << " ";
    }
    std::cout << "\nFiles:\n";
    for (const auto& [path, details] : files) {
        std::cout << "  " << path << " (" << details << ")\n";
    }
}

// ============================================================================
// fetchPackageInfoFromLocal
// ============================================================================
// Fetch package info from a local database. Returns true on success, or false
// if either the DB is missing or the package is not found.
bool fetchPackageInfoFromLocal(const std::string& packageName,
                               const std::string& localDbPath,
                               PackageInfo& packageInfo)
{
    if (!fs::exists(localDbPath)) {
        std::cerr << "Error: Local database not found at " << localDbPath << "\n";
        return false;
    }

    std::ifstream dbFile(localDbPath);
    std::string line;

    while (std::getline(dbFile, line)) {
        if (line.find(packageName) == 0) {
            // Parse the package information
            std::string name     = line;
            std::string version;
            std::vector<std::string> dependencies;
            std::map<std::string, std::string> files;

            // Process subsequent lines for version/files
            while (std::getline(dbFile, line) && !line.empty()) {
                if (line.rfind("Version:", 0) == 0) {
                    version = line.substr(8);
                } else if (line.rfind("Files:", 0) == 0) {
                    while (std::getline(dbFile, line) && !line.empty() && line[0] == '/') {
                        files[line] = "Installed file";
                    }
                    break;
                }
            }

            packageInfo = PackageInfo(name, version, "Installed package", dependencies, files);
            return true;
        }
    }

    std::cerr << "Error: Package " << packageName << " not found in the local database.\n";
    return false;
}

// ============================================================================
// fetchPackageInfoFromRepos
// ============================================================================
// Fetch package info from remote repositories. Returns true on success,
// or false if the repos config is missing or the package is not found in any repo.
bool fetchPackageInfoFromRepos(const std::string& packageName,
                               const std::string& reposConfPath,
                               PackageInfo& packageInfo)
{
    if (!fs::exists(reposConfPath)) {
        std::cerr << "Error: Repositories configuration not found at " << reposConfPath << "\n";
        return false;
    }

    std::ifstream confFile(reposConfPath);
    std::string repoUrl;

    // Iterate through repos.conf file, line by line
    while (std::getline(confFile, repoUrl)) {
        if (repoUrl.empty() || repoUrl[0] == '#') {
            continue;
        }

        // Ensure repoUrl ends with a slash
        if (repoUrl.back() != '/') {
            repoUrl += '/';
        }

        // Construct the .yaml URL for this repo
        std::string repoDbUrl     = repoUrl + "repo.db.yaml";
        std::string repoDbContent;

        // Use libcurl to download the repo database content
        CURL* curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, repoDbUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &repoDbContent);

            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                std::cerr << "Error: Failed to fetch repository database from "
                          << repoDbUrl << " ("
                          << curl_easy_strerror(res) << ")\n";
                curl_easy_cleanup(curl);
                continue;
            }
            curl_easy_cleanup(curl);
        }

        // Parse the YAML repository database
        YAML::Node repo = YAML::Load(repoDbContent);
        if (!repo["packages"]) {
            continue; // No packages key, skip this repo
        }

        for (const auto& package : repo["packages"]) {
            if (!package["name"] || !package["name"].IsScalar()) {
                continue; // Missing name or not a string
            }

            // Check if package name matches
            if (package["name"].as<std::string>() == packageName) {
                // Extract relevant fields
                std::string name        = package["name"].as<std::string>();
                std::string version     = package["version"]       ? package["version"].as<std::string>()       : "";
                std::string description = package["description"]   ? package["description"].as<std::string>()   : "";
                std::vector<std::string> dependencies;

                if (package["dependencies"] && package["dependencies"].IsSequence()) {
                    for (const auto& dep : package["dependencies"]) {
                        dependencies.push_back(dep.as<std::string>());
                    }
                }

                std::map<std::string, std::string> files;
                if (package["files"] && package["files"].IsSequence()) {
                    for (const auto& file : package["files"]) {
                        files[file.as<std::string>()] = "File included";
                    }
                }

                // We’ve found the package!
                packageInfo = PackageInfo(name, version, description, dependencies, files);
                return true;
            }
        }
    }

    std::cerr << "Error: Package " << packageName << " not found in repositories.\n";
    return false;
}