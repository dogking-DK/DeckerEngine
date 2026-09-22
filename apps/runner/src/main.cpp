#include <dk/core/Version.hpp>

#include <iostream>
#include <string_view>

int main(int argc, char* argv[])
{
    if (argc == 1 || (argc == 2 && std::string_view{argv[1]} == "--help")) {
        std::cout << "DeckerEngine bootstrap runner\n"
                     "Usage: dk-run [--help | --version]\n"
                     "Scene execution is not implemented yet.\n";
        return 0;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "DeckerEngine " << dk::version() << '\n';
        return 0;
    }

    std::cerr << "Unsupported arguments. Use dk-run --help.\n";
    return 2;
}

