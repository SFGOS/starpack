#include "spaceship.hpp"
#include <iostream>
namespace Starpack {
    void Spaceship::print() {
        std::cout << 
            // Spaceship top
            "\033[1;34m           /\\\033[0m\n"
            "\033[1;36m          /  \\\033[0m\n"
            "\033[1;37m         /____\\\033[0m\n"
            // Spaceship body
            "\033[1;35m        /\\    /\\\033[0m\n"
            "\033[1;37m       /  \\  /  \\\033[0m\n"
            "\033[1;36m      /____\\/____\\\033[0m\n"
            // Spaceship hull and windows
            "\033[1;34m     /======[ ]======\\\033[0m\n"
            "\033[1;36m    ||  ___ [ ] ___  ||\033[0m\n"
            "\033[1;37m    || |___|| ||___| ||\033[0m\n"
            // Spaceship bottom and engines
            "\033[1;35m    /__|         |__\\\033[0m\n"
            "\033[1;37m   /   \\_________/   \\\033[0m\n"
            "\033[1;36m  /___________________\\\033[0m\n"
            "\033[1;34m      /_|       |_\\\033[0m\n"
            "\033[1;36m     /__|       |__\\\033[0m\n";
    }
}