#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include <vector>

namespace Starpack {

class Config
{
public:
    /**
     * @brief A list of repository URLs.
     */
    std::vector<std::string> repositories;

    /**
     * @brief Loads configuration from a file on disk.
     * @param path Path to the configuration file.
     * @return A fully populated Config instance.
     */
    static Config loadFromFile(const std::string& path);

    /**
     * @brief Saves the current configuration to a file.
     * @param path Path to the file where configuration should be saved.
     */
    void saveToFile(const std::string& path) const;

    /**
     * @brief Prints the list of repositories to standard output.
     */
    void print() const;

    /**
     * @brief Adds a new repository URL to the configuration.
     * @param repo The repository URL to add.
     */
    void addRepository(const std::string& repo);

    /**
     * @brief Removes a repository URL from the configuration if it exists.
     * @param repo The repository URL to remove.
     */
    void removeRepository(const std::string& repo);
};

} // namespace Starpack

#endif // CONFIG_HPP
