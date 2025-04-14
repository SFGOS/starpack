#ifndef REPOSITORY_HPP
#define REPOSITORY_HPP

#include <string>

namespace Starpack {

/**
 * @class Repository
 * @brief Provides methods to create and maintain a Starpack repository index.
 */
class Repository
{
public:
    /**
     * @brief Creates a repository index file (repo.db.yaml) from all Starpack
     *        package files (*.starpack) found in the specified directory.
     *
     * @param location The directory containing *.starpack packages.
     */
    static void createRepoIndex(const std::string& location);

    /**
     * @brief Detects packages in the repository directory that are missing
     *        from repo.db.yaml and adds them, updating the index accordingly.
     *
     * @param location The directory containing the repository index and packages.
     */
    static void addMissingPackagesToIndex(const std::string& location);
};

} // namespace Starpack

#endif // REPOSITORY_HPP
