/*******************************************************
 * hook.cpp
 *
 * Manages finding, parsing, matching, and executing
 * hooks based on system events (like package installation).
 * Supports both universal hooks and package-specific hooks.
 *******************************************************/

#include "hook.hpp"
#include "chroot_util.hpp"

#include <fstream> // For reading hook files
#include <sstream> // Potentially useful for string manipulation
#include <stdexcept>
#include <iostream>
#include <unistd.h>
#include <sys/types.h> // For waitpid types
#include <sys/wait.h>  // For WIFEXITED, WEXITSTATUS etc.
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <vector>        // For storing lists of hooks, paths, etc.
#include <set>           // Useful for unique items (though unordered_set often faster)
#include <unordered_set> // For efficient duplicate checking (e.g., filenames)
#include <future>        // Available for async operations
#include <cctype>        // For ::tolower

namespace fs = std::filesystem;

/**
 * --------------------------------------------------------------------------
 * Hook::trim
 *
 * Removes leading and trailing whitespace from the given string.
 * --------------------------------------------------------------------------
 */
void Hook::trim(std::string &s)
{
    const char *whitespace = " \t\n\r\f\v";
    s.erase(0, s.find_first_not_of(whitespace));
    s.erase(s.find_last_not_of(whitespace) + 1);
}

namespace
{
    /**
     * ----------------------------------------------------------------------
     * NewHookInfo
     *
     * Structure holding basic hook metadata such as its user-defined name
     * and an optional description of the hook’s purpose or function.
     * ----------------------------------------------------------------------
     */
    struct NewHookInfo
    {
        std::string name;        // User-defined name for the hook
        std::string description; // Optional description of what the hook does
    };

    /**
     * ----------------------------------------------------------------------
     * NewHookWhen
     *
     * Structure defining when a hook should trigger. The "phase" field
     * is mandatory (e.g., "PostInstall"), while the "ops" vector can be
     * empty to signify all operations. "paths" and "negations" define
     * path-based filters (must match paths vs. must NOT match).
     * ----------------------------------------------------------------------
     */
    struct NewHookWhen
    {
        std::string phase;                  // e.g., "PostInstall" or "PreRemove"
        std::vector<std::string> ops;       // e.g., {"Install", "Upgrade"}, empty = any op
        std::vector<std::string> paths;     // required path matches (wildcards possible)
        std::vector<std::string> negations; // if a path matches these, hook is skipped
    };

    /**
     * ----------------------------------------------------------------------
     * NewHookExec
     *
     * Defines the command to be executed by the hook, and whether the hook
     * needs the list of affected paths (currently not implemented).
     * ----------------------------------------------------------------------
     */
    struct NewHookExec
    {
        std::string command;     // The command string to execute
        bool needsPaths = false; // If true, affected paths should be passed in some manner
    };

    /**
     * ----------------------------------------------------------------------
     * NewStyleUniversalHook
     *
     * A single hook definition as read from a .hook file. It comprises:
     * - Hook metadata (NewHookInfo)
     * - When conditions (NewHookWhen)
     * - Execution config (NewHookExec)
     * ----------------------------------------------------------------------
     */
    struct NewStyleUniversalHook
    {
        std::string sourceFilePath; // Original path of the .hook file (for logging)
        NewHookInfo info;           // Hook metadata
        NewHookWhen when;           // Trigger conditions
        NewHookExec exec;           // Execution details
    };

    /**
     * ----------------------------------------------------------------------
     * parseNewStyleHookFile
     *
     * Reads a .hook file line by line (INI-like syntax). The file is split
     * into sections like [Hook], [When], [Exec], and keys within those sections.
     *
     * @throws std::runtime_error if the file cannot be opened.
     * @returns a fully populated NewStyleUniversalHook struct.
     * ----------------------------------------------------------------------
     */
    static NewStyleUniversalHook parseNewStyleHookFile(const std::string &filepath)
    {
        std::ifstream file(filepath);
        if (!file.is_open())
        {
            throw std::runtime_error("Cannot open hook file: " + filepath);
        }

        NewStyleUniversalHook hook;
        hook.sourceFilePath = filepath; // Remember where we read it from

        std::string line;
        std::string currentSection;
        int lineNum = 0;

        while (std::getline(file, line))
        {
            lineNum++;
            Hook::trim(line);

            // Skip blank lines and comments
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            // Detect section headers like [Hook], [When], [Exec]
            if (line.front() == '[' && line.back() == ']')
            {
                currentSection = line.substr(1, line.size() - 2);
                Hook::trim(currentSection);
                continue;
            }

            // Look for key=value lines
            size_t equalsPos = line.find('=');
            if (equalsPos == std::string::npos)
            {
                std::cerr << "Warning: Invalid line format (missing '=') in "
                          << filepath << ":" << lineNum << ": " << line << std::endl;
                continue;
            }

            std::string key = line.substr(0, equalsPos);
            std::string value = line.substr(equalsPos + 1);
            Hook::trim(key);
            Hook::trim(value);

            if (key.empty())
            {
                std::cerr << "Warning: Empty key found in "
                          << filepath << ":" << lineNum << ": " << line << std::endl;
                continue;
            }

            // Switch logic based on the current section name
            if (currentSection == "Hook")
            {
                if (key == "Name")
                {
                    hook.info.name = value;
                }
                else if (key == "Description")
                {
                    hook.info.description = value;
                }
                else
                {
                    std::cerr << "Warning: Unknown key '"
                              << key << "' in [Hook] section of "
                              << filepath << ":" << lineNum << std::endl;
                }
            }
            else if (currentSection == "When")
            {
                if (key == "Phase")
                {
                    hook.when.phase = value;
                }
                else if (key == "Operation")
                {
                    hook.when.ops.push_back(value);
                }
                else if (key == "Paths")
                {
                    hook.when.paths.push_back(value);
                }
                else if (key == "Negation")
                {
                    hook.when.negations.push_back(value);
                }
                else
                {
                    std::cerr << "Warning: Unknown key '"
                              << key << "' in [When] section of "
                              << filepath << ":" << lineNum << std::endl;
                }
            }
            else if (currentSection == "Exec")
            {
                if (key == "Command")
                {
                    hook.exec.command = value;
                }
                else if (key == "NeedsPaths")
                {
                    std::string lowerVal = value;
                    std::transform(lowerVal.begin(), lowerVal.end(), lowerVal.begin(),
                                   [](unsigned char c)
                                   {
                                       return std::tolower(c);
                                   });
                    hook.exec.needsPaths =
                        (lowerVal == "yes" || lowerVal == "true");
                }
                else
                {
                    std::cerr << "Warning: Unknown key '"
                              << key << "' in [Exec] section of "
                              << filepath << ":" << lineNum << std::endl;
                }
            }
            else
            {
                // Data outside recognized sections
                std::cerr << "Warning: Data outside of a known section in "
                          << filepath << ":" << lineNum
                          << ": " << line << std::endl;
            }
        }

        // Basic validation
        if (hook.when.phase.empty())
        {
            std::cerr << "Warning: Hook file " << filepath
                      << " is missing mandatory 'Phase' field in [When] section."
                      << std::endl;
        }
        if (hook.exec.command.empty())
        {
            std::cerr << "Warning: Hook file " << filepath
                      << " is missing mandatory 'Command' field in [Exec] section."
                      << std::endl;
        }

        return hook;
    }

    /**
     * ----------------------------------------------------------------------
     * matchWildcard
     *
     * Performs basic wildcard matching where:
     * - "*xyz*" means substring "xyz" can appear anywhere in 'str'.
     * - "*xyz" means 'str' must end in "xyz".
     * - "xyz*" means 'str' must start with "xyz".
     * - Otherwise, we treat the pattern as literal.
     * ----------------------------------------------------------------------
     */
    static bool matchWildcard(const std::string &pattern, const std::string &str)
    {
        // Simple "match everything"
        if (pattern == "*")
        {
            return true;
        }

        size_t firstStar = pattern.find('*');
        if (firstStar == std::string::npos)
        {
            // No wildcard at all, must match exactly
            return (pattern == str);
        }

        size_t lastStar = pattern.rfind('*');

        // Pattern like "*foo*"
        if (firstStar == 0 &&
            lastStar == pattern.size() - 1 &&
            pattern.size() > 1)
        {
            // e.g. "*abc*"
            std::string inner = pattern.substr(1, pattern.size() - 2);
            return (str.find(inner) != std::string::npos);
        }

        // Pattern like "*foo"
        if (firstStar == 0 && lastStar == 0 && pattern.size() > 1)
        {
            // e.g. "*xyz"
            std::string suffix = pattern.substr(1);
            if (str.size() < suffix.size())
            {
                return false;
            }
            return (str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
        }

        // Pattern like "foo*"
        if (firstStar == pattern.size() - 1 &&
            lastStar == pattern.size() - 1 &&
            pattern.size() > 1)
        {
            // e.g. "abc*"
            std::string prefix = pattern.substr(0, pattern.size() - 1);
            if (str.size() < prefix.size())
            {
                return false;
            }
            return (str.compare(0, prefix.size(), prefix) == 0);
        }

        // If more complex, treat as literal
        std::cerr << "Warning: Wildcard pattern '" << pattern
                  << "' is too complex for basic matching. "
                  << "Treating as literal." << std::endl;
        return (pattern == str);
    }

    /**
     * ----------------------------------------------------------------------
     * newStyleHookMatches
     *
     * Checks if a parsed hook (NewStyleUniversalHook) matches the given
     * operation and list of affected paths:
     *
     * 1) If the hook lists operations, the current operation must be present.
     * 2) If the hook lists positive path patterns, at least one of the
     *    affectedPaths must match one of those patterns.
     * 3) If any path matches a negation pattern, the hook is ignored.
     * ----------------------------------------------------------------------
     */
    static bool newStyleHookMatches(const NewStyleUniversalHook &hook,
                                    const std::string &operation,
                                    const std::vector<std::string> &affectedPaths)
    {
        // 1. Operation check (if ops is non-empty)
        if (!hook.when.ops.empty())
        {
            const auto &ops = hook.when.ops;
            if (std::find(ops.begin(), ops.end(), operation) == ops.end())
            {
                return false;
            }
        }

        // 2. Positive path matching
        if (!hook.when.paths.empty())
        {
            bool matchedAny = false;
            for (const auto &pattern : hook.when.paths)
            {
                for (const auto &p : affectedPaths)
                {
                    if (matchWildcard(pattern, p))
                    {
                        matchedAny = true;
                        break;
                    }
                }
                if (matchedAny)
                {
                    break;
                }
            }

            if (!matchedAny)
            {
                // None of the required patterns matched
                return false;
            }
        }

        // 3. Negation check
        if (!hook.when.negations.empty())
        {
            for (const auto &negPattern : hook.when.negations)
            {
                for (const auto &p : affectedPaths)
                {
                    if (matchWildcard(negPattern, p))
                    {
                        // A path matched a negation pattern, ignore hook
                        return false;
                    }
                }
            }
        }

        // Passed all checks
        return true;
    }

} // end anonymous namespace

/**
 * ==============================================================================
 * Hook::runNewStyleHooks
 *
 * The main function to:
 * 1) Collect potential hook files (universal + package-specific).
 * 2) Parse them into NewStyleUniversalHook objects.
 * 3) Filter by phase/operation/paths.
 * 4) Execute each matching hook in ascending filename order.
 *
 * Returns the number of hooks that were actually executed.
 * ==============================================================================
 * Called New Hooks because the early version sucked :p
 */
size_t Hook::runNewStyleHooks(const std::string &phase,
                              const std::string &operation,
                              const std::vector<std::string> &affectedPaths,
                              const std::string &installDir,
                              const std::optional<std::string> &packageNameOpt)
{
    // -----------------------------------------------------------
    // Stage 1: Gather Candidate Hooks
    // -----------------------------------------------------------

    std::vector<fs::path> potentialHookFiles;
    std::unordered_set<std::string> seenHookFilenames; // used to avoid duplicates

    // Directories where hooks may reside:
    const fs::path universalHookDirHost = "/etc/starpack.d/universal-hooks/";
    const fs::path packageHooksDestBase = fs::path(installDir) / "etc" / "starpack" / "hooks";

    // 1a) Gather from the universal hooks directory on the host
    if (fs::exists(universalHookDirHost) && fs::is_directory(universalHookDirHost))
    {
        try
        {
            for (const auto &entry : fs::directory_iterator(universalHookDirHost))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".hook")
                {
                    std::string filename = entry.path().filename().string();
                    if (seenHookFilenames.insert(filename).second)
                    {
                        potentialHookFiles.push_back(entry.path());
                    }
                }
            }
        }
        catch (const fs::filesystem_error &e)
        {
            std::cerr << "Warning: Error iterating universal hooks directory '"
                      << universalHookDirHost.string() << "': "
                      << e.what() << std::endl;
        }
    }

    // 1b) Gather from the package-specific hooks directory (if we have a package name)
    if (packageNameOpt && !packageNameOpt->empty())
    {
        fs::path packageHookDirTarget = packageHooksDestBase / *packageNameOpt;
        if (fs::exists(packageHookDirTarget) && fs::is_directory(packageHookDirTarget))
        {
            try
            {
                for (const auto &entry : fs::directory_iterator(packageHookDirTarget))
                {
                    if (entry.is_regular_file() && entry.path().extension() == ".hook")
                    {
                        std::string filename = entry.path().filename().string();
                        // Add if not already present
                        if (seenHookFilenames.insert(filename).second)
                        {
                            potentialHookFiles.push_back(entry.path());
                        }
                        // else: universal hook with same filename has precedence
                    }
                }
            }
            catch (const fs::filesystem_error &e)
            {
                std::cerr << "Warning: Error iterating package hooks directory '"
                          << packageHookDirTarget.string() << "': "
                          << e.what() << std::endl;
            }
        }
    }

    // 1c) Parse each file and see if it matches the current scenario
    std::vector<NewStyleUniversalHook> matchingHooks;
    for (const auto &hookFile : potentialHookFiles)
    {
        try
        {
            NewStyleUniversalHook parsedHook = parseNewStyleHookFile(hookFile.string());
            // Must match the given phase
            if (parsedHook.when.phase != phase)
            {
                continue;
            }
            // Must also match the current operation/paths
            if (newStyleHookMatches(parsedHook, operation, affectedPaths))
            {
                matchingHooks.push_back(parsedHook);
            }
        }
        catch (const std::exception &ex)
        {
            std::cerr << "Warning: Error parsing hook file '"
                      << hookFile.string() << "': "
                      << ex.what() << ". Skipping." << std::endl;
        }
    }

    // If nothing matched, return
    if (matchingHooks.empty())
    {
        return 0;
    }

    // -----------------------------------------------------------
    // Stage 2: Execute Matching Hooks
    // -----------------------------------------------------------

    std::cout << "Running " << phase << " hooks for "
              << operation << " operation..." << std::endl;

    if (packageNameOpt)
    {
        std::cout << "  (Package context: " << *packageNameOpt << ")"
                  << std::endl;
    }
    else
    {
        std::cout << "  (No specific package context)" << std::endl;
    }

    // Decide if we will chroot, comparing installDir to "/"
    bool useChroot = false;
    fs::path rootPath("/");
    fs::path installPath(installDir);

    try
    {
        fs::path canonicalRoot = fs::exists(rootPath) ? fs::canonical(rootPath) : rootPath;
        fs::path canonicalInstallDir = fs::exists(installPath) ? fs::canonical(installPath) : installPath;
        useChroot = (canonicalInstallDir != canonicalRoot);
    }
    catch (const fs::filesystem_error &fs_err)
    {
        std::cerr << "Warning: Could not perform filesystem comparison for installDir '"
                  << installDir << "' (Reason: " << fs_err.what()
                  << "). Using string comparison fallback." << std::endl;
        useChroot = (installDir != "/");
    }

    if (useChroot)
    {
        std::cout << "  (Execution mode: chroot into " << installDir << ")"
                  << std::endl;
    }
    else
    {
        std::cout << "  (Execution mode: direct on host '/')" << std::endl;
    }

    std::cout << "  Found " << matchingHooks.size()
              << " matching hook(s) to execute." << std::endl;

    // Sort the hooks by their source filepath for a deterministic order
    std::sort(matchingHooks.begin(), matchingHooks.end(),
              [](const auto &a, const auto &b)
              {
                  return a.sourceFilePath < b.sourceFilePath;
              });

    // Execute each hook
    size_t executed_count = 0;
    for (const auto &hook : matchingHooks)
    {
        executed_count++;
        std::cout << "  -> Executing hook (" << executed_count
                  << "/" << matchingHooks.size() << "): "
                  << fs::path(hook.sourceFilePath).filename().string();

        // Optional: show short description if present
        if (!hook.info.description.empty())
        {
            std::cout << " [" << hook.info.description << "]";
        }
        std::cout << std::endl;

        // Skip if command is empty
        if (hook.exec.command.empty())
        {
            std::cerr << "     Warning: Empty command found in hook "
                      << fs::path(hook.sourceFilePath).filename().string()
                      << ". Skipping." << std::endl;
            continue;
        }

        // If it needs paths, we haven't implemented passing them in
        if (hook.exec.needsPaths)
        {
            std::cerr << "     Warning: Hook "
                      << fs::path(hook.sourceFilePath).filename().string()
                      << " requires NeedsPaths=true, but path passing is not implemented. "
                      << "Command will run without paths." << std::endl;
        }

        bool commandSuccess = false;

        // Decide if we run inside a chroot or directly
        if (useChroot)
        {
            // Check for /bin/sh in the chroot
            if (!fs::exists(fs::path(installDir) / "bin" / "sh"))
            {
                std::cerr << "     ERROR: /bin/sh not found within the chroot environment: "
                          << installDir << ". Cannot execute hook command." << std::endl;
                commandSuccess = false;
                return 0;
            }
            else
            {
                std::cout << "     Running command (in chroot at "
                          << installDir << "): " << hook.exec.command
                          << std::endl;

                // We'll call `/bin/sh -c "<command>"`
                std::vector<std::string> commandArgs = {
                    "/bin/sh",
                    "-c",
                    hook.exec.command};

                commandSuccess = Starpack::ChrootUtil::executeInChroot(
                    installDir,
                    commandArgs[0],
                    commandArgs);

                if (!commandSuccess)
                {
                    std::cerr << "     Hook '"
                              << hook.info.name << "' ("
                              << fs::path(hook.sourceFilePath).filename().string()
                              << ") FAILED in chroot." << std::endl;
                    return 0;
                }
            }
        }
        else
        {
            // Directly on the host using std::system
            std::cout << "     Running command (direct on host): "
                      << hook.exec.command << std::endl;

            int result = std::system(hook.exec.command.c_str());

            if (result == -1)
            {
                std::cerr << "     Hook '" << hook.info.name
                          << "' ("
                          << fs::path(hook.sourceFilePath).filename().string()
                          << ") FAILED to execute (std::system error: "
                          << strerror(errno) << ")." << std::endl;
                commandSuccess = false;
            }
            else if (WIFEXITED(result))
            {
                int exitCode = WEXITSTATUS(result);
                commandSuccess = (exitCode == 0);
                if (!commandSuccess)
                {
                    std::cerr << "     Hook '" << hook.info.name
                              << "' ("
                              << fs::path(hook.sourceFilePath).filename().string()
                              << ") FAILED (direct execution). Exit code: "
                              << exitCode << std::endl;
                }
            }
            else if (WIFSIGNALED(result))
            {
                std::cerr << "     Hook '" << hook.info.name << "' ("
                          << fs::path(hook.sourceFilePath).filename().string()
                          << ") FAILED (direct execution). Terminated by signal: "
                          << WTERMSIG(result) << std::endl;
                commandSuccess = false;
            }
            else
            {
                std::cerr << "     Hook '" << hook.info.name
                          << "' ("
                          << fs::path(hook.sourceFilePath).filename().string()
                          << ") finished with unexpected status (direct execution): "
                          << result << std::endl;
                commandSuccess = false;
            }

            if (!commandSuccess)
            {
                return 0;
            }
        }
    }

    // If we got here, all matching hooks executed successfully
    std::cout << "  Finished processing hooks for "
              << phase << "/" << operation << "." << std::endl;

    // Return how many hooks we executed in total
    return matchingHooks.size();
}
