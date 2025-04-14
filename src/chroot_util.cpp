#include "chroot_util.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <system_error>
#include <filesystem>

// Required Linux/Unix Headers
#include <sys/mount.h> // mount, umount2
#include <sys/types.h> // pid_t
#include <sys/wait.h>  // waitpid
#include <unistd.h>    // chroot, chdir, fork, execve, _exit
#include <errno.h>
#include <cstring>     // strerror, strcmp
#include <cstdlib>     // exit, EXIT_FAILURE, EXIT_SUCCESS

namespace fs = std::filesystem;

namespace Starpack {
namespace ChrootUtil {

    // Function for Mounting
    static bool mountFileSystem(const std::string& source, const std::string& target,
                               const std::string& filesystemtype, unsigned long mountflags,
                               const void* data) {
        // Ensures that the target directory exists before mounting
        if (!fs::exists(target)) {
             try {
                 // Creates parent directories recursively if they don't exist
                 fs::path targetPath(target);
                 if (!fs::exists(targetPath.parent_path())) {
                      fs::create_directories(targetPath.parent_path());
                 }
                 // Creates the target mount point itself if it's just a directory
                 if (!fs::is_directory(target)) { // Avoid error if it's already there
                    fs::create_directory(target);
                 }
             } catch (const std::exception& e) {
                  std::cerr << "Error creating mount target directory " << target << ": " << e.what() << std::endl;
                  return false;
             }
        }

        if (mount(source.c_str(), target.c_str(), filesystemtype.c_str(), mountflags, data) != 0) {
            std::cerr << "Error mounting " << source << " (" << filesystemtype << ") to " << target << ": " << strerror(errno) << std::endl;
            return false;
        }
        return true;
    }

    // Function for Unmounting
    static bool unmountFileSystem(const std::string& target, int flags = MNT_DETACH) {
         // Use umount2 with MNT_DETACH for busy filesystems
        if (umount2(target.c_str(), flags) != 0) {
            // Only report error if it wasn't already unmounted (ENOENT) or invalid argument (EINVAL)
            if (errno != ENOENT && errno != EINVAL) {
                 std::cerr << "Error unmounting " << target << " (umount2): " << strerror(errno) << std::endl;
                 // Try basic umount as a fallback ONLY if umount2 failed for reasons other than non-existence
                 if (umount(target.c_str()) != 0) {
                      if (errno != ENOENT && errno != EINVAL) {
                          std::cerr << "Error unmounting " << target << " (umount fallback): " << strerror(errno) << std::endl;
                          return false;
                      }
                 }
            } else if (errno == EINVAL){
                 // If umount2 failed with EINVAL, try simple umount
                 if (umount(target.c_str()) != 0 && errno != ENOENT) {
                      std::cerr << "Error unmounting " << target << " (umount EINVAL fallback): " << strerror(errno) << std::endl;
                      return false;
                 }
            }
             // If errno was ENOENT for umount2, it was already unmounted, which is fine.
             else if (errno != ENOENT){
                 return false; // if umount2 failed for other reason
             }
        }
        return true;
    }

    bool executeInChroot(const std::string& chrootDir,
                         const std::string& command, // Path inside chroot
                         const std::vector<std::string>& args,
                         const std::string& workingDir)
    {
        if (command.empty() || args.empty() || args[0].empty()) {
             std::cerr << "Error: Invalid command or arguments for chroot execution." << std::endl;
             return false;
        }
         if (!fs::exists(chrootDir) || !fs::is_directory(chrootDir)) {
            std::cerr << "Error: Chroot directory '" << chrootDir << "' does not exist or is not a directory." << std::endl;
            return false;
         }

        // Prepare Mount Points
        fs::path procMountPoint = fs::path(chrootDir) / "proc";
        fs::path devPtsMountPoint = fs::path(chrootDir) / "dev" / "pts";

        bool procMounted = false;
        bool devPtsMounted = false;

        pid_t pid = -1; // Initialize pid
        bool commandSuccess = false; // Command execution success flag

        try {

            // Mount /proc
            if (!mountFileSystem("proc", procMountPoint.string(), "proc", MS_NODEV | MS_NOEXEC | MS_NOSUID, nullptr)) {
                throw std::runtime_error("Failed to mount /proc in chroot");
            }
            procMounted = true;

            // Mount /dev/pts
            if (!mountFileSystem("devpts", devPtsMountPoint.string(), "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=620")) {
                // Try without options as fallback? Some systems might not need/support them.
                if (!mountFileSystem("devpts", devPtsMountPoint.string(), "devpts", MS_NOSUID | MS_NOEXEC, nullptr)){
                     throw std::runtime_error("Failed to mount /dev/pts in chroot (with or without options)");
                }
            }
            devPtsMounted = true;


            // --- Fork Process ---
            pid = fork();
            if (pid < 0) {
                throw std::system_error(errno, std::system_category(), "Fork failed");
            }

            // --- Child Process ---
            if (pid == 0) {
                try {
                    if (chroot(chrootDir.c_str()) != 0) {
                         throw std::system_error(errno, std::system_category(), "chroot failed");
                    }
                    if (chdir(workingDir.c_str()) != 0) {
                         throw std::system_error(errno, std::system_category(), "chdir after chroot failed");
                    }

                    // Prepare arguments for execve
                    std::vector<char*> argv;
                    for (const auto& arg : args) {
                        argv.push_back(const_cast<char*>(arg.c_str()));
                    }
                    argv.push_back(nullptr); // Null terminator

                    // Prepare minimal environment
                    static char pathVar[] = "PATH=/usr/bin:/bin:/usr/sbin:/sbin";
                    char* const envp[] = {
                    pathVar,
                    nullptr
                };



                    // Execute the command
                    execve(command.c_str(), argv.data(), envp);

                    // If execve returns, an error occurred
                    perror(("execve failed for command: " + command).c_str());
                     _exit(EXIT_FAILURE);

                } catch (const std::system_error& e) {
                     std::cerr << "Child process error: " << e.what() << " (code: " << e.code() << ")" << std::endl;
                     _exit(EXIT_FAILURE);
                } catch (const std::exception& e) {
                     std::cerr << "Child process exception: " << e.what() << std::endl;
                     _exit(EXIT_FAILURE);
                }
            }
            // Parent Process
            else {
                int status;
                pid_t waited_pid = waitpid(pid, &status, 0);

                if (waited_pid < 0) {
                    perror("waitpid failed");
                    commandSuccess = false;
                } else {
                    if (WIFEXITED(status)) {
                        int exitCode = WEXITSTATUS(status);
                        commandSuccess = (exitCode == 0);
                    } else if (WIFSIGNALED(status)) {
                        std::cerr << "Chrooted process terminated by signal: " << WTERMSIG(status) << std::endl;
                        commandSuccess = false;
                    } else {
                         std::cerr << "Chrooted process finished with unknown status." << std::endl;
                         commandSuccess = false;
                    }
                }
            }

        } catch (const std::exception& e) {
            std::cerr << "Error during chroot setup or parent process: " << e.what() << std::endl;
            commandSuccess = false;
        }

        // Unmount filesystems (cleanup) ---
        // Unmount in reverse order of mounting.
        bool cleanup_ok = true;
        if (devPtsMounted) { if (!unmountFileSystem(devPtsMountPoint.string())) cleanup_ok = false; }
        if (procMounted) { if (!unmountFileSystem(procMountPoint.string())) cleanup_ok = false; }


        if (!cleanup_ok) {
             std::cerr << "!!! WARNING: Failed to unmount one or more filesystems from chroot directory: " << chrootDir << std::endl;
             std::cerr << "!!! Manual cleanup might be required ('umount " << procMountPoint.string() << "', etc.)" << std::endl;
             commandSuccess = false;
        } else {
        }

        return commandSuccess;
    }

} // namespace ChrootUtil
} // namespace Starpack