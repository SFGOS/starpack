#ifndef CHROOT_UTIL_HPP
#define CHROOT_UTIL_HPP

#include <string>
#include <vector>

namespace Starpack {
namespace ChrootUtil {

/**
 * @brief Executes a given command inside a chroot environment.
 *
 * @param chrootDir The directory that will act as the new root.
 * @param command   The command (executable name/path) to run inside the chroot.
 * @param args      A list of arguments to pass to the command.
 * @param workingDir The working directory inside the chroot (default is "/").
 * @return True if the command executes and exits with code 0, false otherwise.
 *
 * @note This function requires proper privileges to perform a chroot, and
 *       the environment inside the chroot must be sufficient to run the command.
 */
bool executeInChroot(const std::string& chrootDir,
                     const std::string& command,
                     const std::vector<std::string>& args,
                     const std::string& workingDir = "/");

} // namespace ChrootUtil
} // namespace Starpack

#endif // CHROOT_UTIL_HPP
