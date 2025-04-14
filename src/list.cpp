#include "list.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>

namespace Starpack {

void List::showInstalledPackages(const std::string& dbPath)
{
    std::ifstream dbFile(dbPath);
    if (!dbFile.is_open()) {
        std::cerr << "Error: Could not open the installed database file: "
                  << dbPath << std::endl;
        return;
    }

    std::cout << "Installed Packages:\n";
    std::cout << "-------------------\n";

    std::string line;
    bool hasPackages = false;

    while (std::getline(dbFile, line)) {
        // each package entry starts with the package name
        if (line.find(" /") != std::string::npos) {
            std::istringstream iss(line);
            std::string packageName;
            iss >> packageName;
            std::cout << packageName << std::endl;
            hasPackages = true;
        }
    }

    if (!hasPackages) {
        std::cout << "No packages are installed (what?)" << std::endl;
    }

    dbFile.close();
}

} // namespace Starpack
