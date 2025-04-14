#include "config.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

namespace Starpack {

    Config Config::loadFromFile(const std::string& path) {
        Config config;

        if (!fs::exists(path)) {
            std::cerr << "Error: Configuration file not found: " << path << std::endl;
            return config;
        }

        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Error: Unable to open configuration file: " << path << std::endl;
            return config;
        }

        std::string line;
        while (std::getline(file, line)) {
            // Skips comments or empty lines
            if (line.empty() || line[0] == '#') {
                continue;
            }
            config.repositories.push_back(line);
        }

        file.close();
        return config;
    }

    void Config::saveToFile(const std::string& path) const {
        std::ofstream file(path, std::ios::trunc); // Truncate file to overwrite
        if (!file.is_open()) {
            std::cerr << "Error: Unable to open configuration file for writing: " << path << std::endl;
            return;
        }

        // Adds a comment header
        file << "# Starpack Repository Configuration\n";
        file << "# Define repositories for Starpack to fetch packages from.\n\n";

        // Writes repositories to file
        for (const auto& repo : repositories) {
            file << repo << "\n";
        }

        file.close();
    }

    void Config::print() const {
        std::cout << "Configured Repositories:" << std::endl;
        for (const auto& repo : repositories) {
            std::cout << "  - " << repo << std::endl;
        }
    }

    void Config::addRepository(const std::string& repo) {
        // Ensures that the repository does not already exist
        if (std::find(repositories.begin(), repositories.end(), repo) != repositories.end()) {
            std::cerr << "Error: Repository already exists: " << repo << std::endl;
            return;
        }
    
        repositories.push_back(repo);
        std::cout << "Added repository: " << repo << std::endl;
    }        

    void Config::removeRepository(const std::string& repo) {
        // Find and remove the repository
        auto it = std::find(repositories.begin(), repositories.end(), repo);
        if (it != repositories.end()) {
            repositories.erase(it);
            std::cout << "Removed repository: " << repo << std::endl;
        } else {
            std::cerr << "Error: Repository not found: " << repo << std::endl;
        }
    }    
}
