#ifndef HOOK_HPP
#define HOOK_HPP

#include <string>
#include <vector>
#include <optional>

/**
 * @class Hook
 * @brief Provides functionality to run hooks at various phases (e.g., PreInstall, PostInstall) 
 *        in the Starpack package management workflow.
 */
class Hook
{
public:
    /**
     * @brief Removes leading and trailing whitespace from the given string.
     * @param s The string to be trimmed.
     */
    static void trim(std::string& s);

    /**
     * @brief Runs any new-style hooks (defined in .hook files) that match the given parameters.
     *
     * @param phase         The phase name (e.g., "PreInstall", "PostUpdate").
     * @param operation     The operation name (e.g., "Install", "Update").
     * @param affectedPaths A list of file paths affected by this operation.
     * @param installDir    The root directory of the installation target.
     * @param packageNameOpt Optional name of the package context; can be empty.
     *
     * @return The number of hooks actually executed.
     */
    static size_t runNewStyleHooks(
        const std::string& phase,
        const std::string& operation,
        const std::vector<std::string>& affectedPaths,
        const std::string& installDir,
        const std::optional<std::string>& packageNameOpt
    );
};

#endif // HOOK_HPP
