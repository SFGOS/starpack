#include "search.hpp"
#include "utils.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <curl/curl.h>
#include <yaml-cpp/yaml.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace Starpack {

// ============================================================================
// Helper: loadRepoUrls
// ============================================================================
// Reads repository entries from /etc/starpack/repos.conf, ensuring each line
// ends with a slash, then appending 'repo.db.yaml'.
std::vector<std::string> loadRepoUrls(const std::string& configPath)
{
    std::ifstream configFile(configPath);
    if (!configFile.is_open()) {
        throw std::runtime_error("Failed to open config file: " + configPath);
    }

    std::vector<std::string> repoUrls;
    std::string line;

    while (std::getline(configFile, line)) {
        // Skip empty lines and comments
        if (!line.empty() && line[0] != '#') {
            if (line.back() != '/') {
                line += '/';  // Ensure slash at the end
            }
            line += "repo.db.yaml";  // Append the repository filename
            repoUrls.push_back(line);
        }
    }
    return repoUrls;
}

// ============================================================================
// Search::searchPackages
// ============================================================================
// Looks for packages in each repository's database whose name, version,
// or description matches the user-supplied query string.
void Search::searchPackages(const std::string& query, const std::string& configPath)
{
    try {
        auto repoUrls = loadRepoUrls(configPath);
        bool found = false;

        for (const auto& url : repoUrls) {
            std::cout << "Searching in repository: " << url << std::endl;

            // Retrieve repository data (YAML) from URL
            std::string repoData = fetchRepoData(url);
            YAML::Node repo = YAML::Load(repoData);

            if (!repo["packages"]) {
                std::cerr << "Error: Invalid repository data at " << url << std::endl;
                continue;
            }

            // Loop through each package in the 'packages' list
            for (const auto& package : repo["packages"]) {
                std::string name        = package["name"].as<std::string>();
                std::string version     = package["version"].as<std::string>();
                std::string description = package["description"].as<std::string>();

                // Check if the query string appears in any field
                if (name.find(query)        != std::string::npos ||
                    version.find(query)     != std::string::npos ||
                    description.find(query) != std::string::npos)
                {
                    std::cout << "Package: " << name << " (Version: " << version << ")\n";
                    std::cout << "Description: " << description << "\n\n";
                    found = true;
                }
            }
        }

        if (!found) {
            std::cout << "No packages found matching: " << query << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error searching packages: " << e.what() << std::endl;
    }
}

// ============================================================================
// Search::searchByFile
// ============================================================================
// Looks for packages in each repository whose 'files' list contains a path
// matching the user-supplied filePath. If an exact path match fails, it tries
// partial matches by filename only.
void Search::searchByFile(const std::string& filePath, const std::string& configPath)
{
    try {
        auto repoUrls = loadRepoUrls(configPath);
        bool found = false;

        // Extract the base filename to handle partial matches
        std::string fileName = fs::path(filePath).filename().string();

        for (const auto& url : repoUrls) {
            std::cout << "Searching in repository: " << url << std::endl;

            // Retrieve repository data (YAML) from URL
            std::string repoData = fetchRepoData(url);
            YAML::Node repo = YAML::Load(repoData);

            if (!repo["packages"]) {
                std::cerr << "Error: Invalid repository data at " << url << std::endl;
                continue;
            }

            // Check each package's file list
            for (const auto& package : repo["packages"]) {
                std::string name        = package["name"].as<std::string>();
                std::string version     = package["version"].as<std::string>();
                std::string description = package["description"].as<std::string>();

                if (package["files"]) {
                    for (const auto& file : package["files"]) {
                        std::string normalizedFile = file.as<std::string>();

                        // Ensure paths are absolute
                        if (!normalizedFile.empty() && normalizedFile.front() != '/') {
                            normalizedFile = "/" + normalizedFile;
                        }

                        // Check for exact path match or filename match
                        if (normalizedFile == filePath ||
                            fs::path(normalizedFile).filename().string() == fileName)
                        {
                            std::cout << "Package: " << name
                                      << " (Version: " << version << ")\n";
                            std::cout << "Description: " << description << "\n";

                            // Highlight matched file path in red
                            std::cout << "Matched File: \033[31m" << normalizedFile
                                      << "\033[0m\n\n";

                            found = true;
                            break;
                        }
                    }
                }
            }
        }

        if (!found) {
            std::cout << "No packages found containing file: " << filePath << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error searching by file: " << e.what() << std::endl;
    }
}

} // namespace Starpack
