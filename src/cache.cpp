#include "cache.hpp"
#include <iostream>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

namespace Starpack {

    void Cache::clean() {
        std::cout << "Cleaning up Starpack cache..." << std::endl;

        // Patterns and directories to clean
        const std::vector<std::pair<std::string, std::string>> cleanupTargets = {
            {"/tmp", R"(.*\.starpack$)"},
            {"/tmp", R"(.*\.sig$)"},
            {"/tmp", R"(.*\.yaml$)"},
            {"/var/lib/starpack/cache", R"(.*)"}
        };

        // Iterate through each directory and pattern
        for (const auto& [directory, pattern] : cleanupTargets) {
            removeFiles(directory, pattern);
        }

        std::cout << "Cache cleanup completed." << std::endl;
    }

    void Cache::removeFiles(const std::string& directory, const std::string& pattern) {
        try {
            if (!fs::exists(directory)) {
                std::cerr << "Directory not found: " << directory << std::endl;
                return;
            }

            std::regex regexPattern(pattern);

            for (const auto& entry : fs::directory_iterator(directory)) {
                if (fs::is_regular_file(entry)) {
                    std::string filename = entry.path().filename().string();
                    if (std::regex_match(filename, regexPattern)) {
                        fs::remove(entry);
                        std::cout << "Removed: " << entry.path() << std::endl;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error cleaning directory " << directory << ": " << e.what() << std::endl;
        }
    }

}