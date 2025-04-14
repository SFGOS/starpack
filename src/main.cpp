#include <iostream>
#include <string>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <unistd.h>

#include "repository.hpp"
#include "install.hpp"
#include "remove.hpp"
#include "chroot_util.hpp"
#include "update.hpp"
#include "info.hpp"
#include "cache.hpp"
#include "spaceship.hpp"
#include "list.hpp"
#include "config.hpp"

// Helper function: Parse the installed database to get all installed package names.
std::vector<std::string> getInstalledPackages(const std::string& dbPath = "/var/lib/starpack/installed.db")
{
    std::vector<std::string> pkgs;
    std::ifstream db(dbPath);
    std::string line;

    while (std::getline(db, line)) {
        // Look for a line indicating the start of a package entry
        if (line.find(" /") != std::string::npos) {
            std::istringstream iss(line);
            std::string pkgName;
            iss >> pkgName;  // read up to the space before '/'
            pkgs.push_back(pkgName);
        }
    }
    return pkgs;
}

void printHelp()
{
    std::cout << "Starpack Alpha (x86_64)\n"
              << "Usage: starpack [options] command\n\n"
              << "Starpack is the SFG OS package manager that provides commands for\n"
              << "installing, removing, and updating packages.\n"
              << "It offers a simplified and interactive interface for package management.\n\n"
              << "Useful commands:\n"
              << "  install      - Install packages\n"
              << "  remove       - Remove packages\n"
              << "  update       - Update package list or upgrade packages\n"
              << "  list         - List installed packages\n"
              << "  info         - Show package details\n"
              << "  repo         - Manage repositories\n"
              << "  clean        - Clean the cache\n\n"
              << "This Star Has Spaceship Powers.\n";
}

int main(int argc, char* argv[])
{
    // If no command is supplied, show the help message
    if (argc < 2) {
        printHelp();
        return 0;
    }

    // Parse the first argument as the main command
    std::string command = argv[1];

    // Certain commands must be run as root
    if ((command == "install" || command == "remove" ||
         command == "update"  || command == "clean"  ||
         command == "list"    || command == "create-starpack") &&
         (geteuid() != 0))
    {
        std::cerr << "Error: The '" << command << "' command must be run as root.\n";
        return 1;
    }

    std::srand(std::time(nullptr)); // Seed random

    // -------------------------------------------------------------
    // Repo Command
    // -------------------------------------------------------------
    if (command == "repo") {
        if (argc >= 3) {
            std::string subCommand = argv[2];
            if (subCommand == "list") {
                Starpack::Config config = Starpack::Config::loadFromFile("/etc/starpack/repos.conf");
                config.print();
            }
            else if (subCommand == "add" && argc == 4) {
                std::string newRepo = argv[3];
                Starpack::Config config = Starpack::Config::loadFromFile("/etc/starpack/repos.conf");
                config.addRepository(newRepo);
                config.saveToFile("/etc/starpack/repos.conf");
            }
            else if (subCommand == "remove" && argc == 4) {
                std::string repo = argv[3];
                Starpack::Config config = Starpack::Config::loadFromFile("/etc/starpack/repos.conf");
                config.removeRepository(repo);
                config.saveToFile("/etc/starpack/repos.conf");
            }
            else if (subCommand == "index" && argc == 4) {
                std::string location = argv[3];
                Starpack::Repository::createRepoIndex(location);
            }
            else if (subCommand == "add-missing" && argc == 4) {
                std::string location = argv[3];
                Starpack::Repository::addMissingPackagesToIndex(location);
            }
            else {
                std::cerr << "Unknown or invalid subcommand for 'repo'.\n";
            }
        }
        else {
            std::cerr << "Usage: starpack repo <subcommand>\n"
                      << "  list                     List all repositories\n"
                      << "  add <repo_url>           Add a new repository\n"
                      << "  remove <repo_url>        Remove a repository\n"
                      << "  index <location>         Generate repository index from a directory\n"
                      << "  add-missing <location>   Add missing packages to the repository index\n";
        }
    }
    // -------------------------------------------------------------
    // Install Command
    // -------------------------------------------------------------
    else if (command == "install") {
        std::string installDir = "/";
        bool installDirSpecified = false;
        std::vector<std::string> packagesToInstall;

        // Collect arguments after "install"
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--installdir") {
                if (i + 1 < argc) {
                    installDir = argv[i + 1];
                    i++;
                    installDirSpecified = true;
                }
                else {
                    std::cerr << "Error: --installdir requires a directory argument.\n";
                    return 1;
                }
            }
            else {
                packagesToInstall.push_back(arg);
            }
        }

        if (packagesToInstall.empty()) {
            std::cerr << "Usage: starpack install <package_name> [package_name ...] [--installdir <dir>]\n";
            return 1;
        }

        Starpack::Installer::installPackage(packagesToInstall, installDir, true);
    }
    // -------------------------------------------------------------
    // Remove Command
    // -------------------------------------------------------------
    else if (command == "remove") {
        std::string installDir = "/";
        std::vector<std::string> packagesToRemove;

        // We start parsing from argv[1], but skipping the 0th is typical
        // for the command line approach. So let's read everything.
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--installdir") {
                if (i + 1 < argc) {
                    installDir = argv[++i];
                }
                else {
                    std::cerr << "Error: --installdir requires a directory argument.\n";
                    return 1;
                }
            }
            else {
                packagesToRemove.push_back(arg);
            }
        }

        // The local DB is at installDir + /var/lib/starpack/installed.db
        std::string dbPath = installDir + "/var/lib/starpack/installed.db";
        Starpack::removePackages(packagesToRemove, dbPath, false, installDir);
        return 0;
    }
    // -------------------------------------------------------------
    // Update Command
    // -------------------------------------------------------------
    else if (command == "update") {
        std::string installDir = "/";
        std::vector<std::string> packagesToUpdate;

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--installdir") {
                if (i + 1 < argc) {
                    installDir = argv[i + 1];
                    i++;
                }
                else {
                    std::cerr << "Error: --installdir requires a directory argument.\n";
                    return 1;
                }
            }
            else {
                packagesToUpdate.push_back(arg);
            }
        }

        // If no packages specified, update everything that's installed
        if (packagesToUpdate.empty()) {
            packagesToUpdate = getInstalledPackages(installDir + "/var/lib/starpack/installed.db");
        }

        Starpack::Updater::updatePackage(packagesToUpdate, installDir);
    }
    // -------------------------------------------------------------
    // Info Command
    // -------------------------------------------------------------
    else if (command == "info") {
        if (argc >= 3) {
            std::string packageName = argv[2];
            const std::string localDbPath = "/var/lib/starpack/installed.db";
            const std::string reposConfPath = "/etc/starpack/repos.conf";

            PackageInfo packageInfo("", "", "", {}, {});
            if (fetchPackageInfoFromLocal(packageName, localDbPath, packageInfo)) {
                packageInfo.display();
            }
            else if (fetchPackageInfoFromRepos(packageName, reposConfPath, packageInfo)) {
                packageInfo.display();
            }
            else {
                std::cerr << "Error: Package " << packageName
                          << " not found locally or in repositories.\n";
            }
        }
        else {
            std::cerr << "Usage: starpack info <package_name>\n";
            return 1;
        }
    }
    // -------------------------------------------------------------
    // Clean Command
    // -------------------------------------------------------------
    else if (command == "clean") {
        Starpack::Cache::clean();
    }
    // -------------------------------------------------------------
    // List Command
    // -------------------------------------------------------------
    else if (command == "list") {
        Starpack::List::showInstalledPackages();
    }
    // -------------------------------------------------------------
    // Spaceship Command
    // -------------------------------------------------------------
    else if (command == "spaceship") {
        Starpack::Spaceship::print();
    }
    // -------------------------------------------------------------
    // Unknown Command
    // -------------------------------------------------------------
    else {
        std::cerr << "Unknown command or insufficient arguments.\n";
        return 1;
    }

    return 0;
}