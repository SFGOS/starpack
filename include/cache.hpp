#ifndef CACHE_HPP
#define CACHE_HPP

#include <string>
#include <vector>

namespace Starpack {

class Cache
{
public:
    /**
     * @brief Cleans up cache files and directories.
     */
    static void clean();

private:
    /**
     * @brief Helper function to delete files in a specified directory
     *        matching a given pattern.
     *
     * @param directory Path to the directory to be cleaned.
     * @param pattern   The pattern of file names to remove (e.g. "*.tmp").
     */
    static void removeFiles(const std::string& directory,
                            const std::string& pattern);
};

} // namespace Starpack

#endif // CACHE_HPP
