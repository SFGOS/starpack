# starpack 🚀

**The official package manager for SFG OS.**

`starpack` is a command-line utility designed specifically for SFG OS to manage software packages. It provides essential functionalities for installing, removing, updating, and inspecting packages within the operating system. Built with C++, it aims for efficiency and ease of use.

*This Star Has Spaceship Powers.* ✨

**(Note: `starpack` is currently in Alpha stage.)**

## Features

* **Install Packages:** Easily install one or more packages from configured repositories.
* **Remove Packages:** Uninstall packages from your system.
* **Update System:** Keep your system up-to-date by updating package lists and upgrading installed packages.
* **Package Information:** Query details about installed packages or packages available in repositories.
* **List Packages:** View all packages currently installed on the system.
* **Repository Management:** Add, remove, and list package repositories. Includes tools for repository maintainers (`index`, `add-missing`).
* **Cache Management:** Clean the local package cache.
* **Easter Egg:** Try `starpack spaceship`!

## Requirements

* **Operating System:** SFG OS (x86_64 architecture primarily targeted)
* **Permissions:** Most package management operations (`install`, `remove`, `update`, `clean`, `list`, `repo add/remove/index/add-missing`) require **root privileges**.
* **Build Dependencies:** A C++ compiler (supporting C++11 or later), CMake, and Make.

## Building from Source (Developer/Contributor Info)

If you need to build `starpack` from source (e.g., for development or testing):

1.  **Clone the repository:**
    ```
    git clone https://github.com/SFGOS/starpack.git
    cd starpack
    ```
2.  **Configure with CMake:**
    ```
    cmake .
    ```
3.  **Compile the code:**
    ```
    make
    ```
4.  **Install (Optional):**
    ```
    sudo make install
    ```

## Usage

The basic syntax is:
`starpack [options] <command> [arguments...]`

### Common Commands

**Important:** Prefix commands modifying the system state with `sudo`.

* **Update package lists & Upgrade all installed packages:**
    ```
    sudo starpack update
    ```

* **Install packages:**
    ```
    sudo starpack install <package_name> [another_package...]
    ```
    *Example:* `sudo starpack install my-app utilities`

* **Remove packages:**
    ```
    sudo starpack remove <package_name> [another_package...]
    ```
    *Example:* `sudo starpack remove my-app`

* **List installed packages:**
    ```
    sudo starpack list
    ```

* **Show package information:**
    (Checks local installation first, then repositories)
    ```
    starpack info <package_name>
    ```
    *Example:* `starpack info core-utils`

* **Clean the package cache:**
    (Removes downloaded package files)
    ```
    sudo starpack clean
    ```

* **Manage Repositories:**
    * List configured repositories:
        ```
        starpack repo list
        ```
    * Add a repository:
        ```
        sudo starpack repo add <repository_url>
        ```
    * Remove a repository:
        ```
        sudo starpack repo remove <repository_url>
        ```

* **Have some fun:**
    ```
    starpack spaceship
    ```

### Advanced Usage: `--installdir`

The `install`, `remove`, and `update` commands accept an optional `--installdir <dir>` argument. This allows performing package operations within a specific directory, often used for managing chroots or staging environments.

*Example:* Install `base-system` into `/mnt/sfgos_root`: