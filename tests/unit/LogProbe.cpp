#include <dk/core/Log.hpp>

#include <iostream>

int main(int argc, char* argv[])
{
    if (argc != 2) {
        return 2;
    }
    auto result = dk::Logger::create({dk::LogLevel::info, true, argv[1]});
    if (!result) {
        std::cerr << result.error().message << '\n';
        return 3;
    }
    auto& logger = **result;
    if (!logger.log(dk::LogLevel::info, "probe", "diagnostic {}", 42) || !logger.flush()) {
        return 4;
    }
    std::cout << "protocol-result\n";
    return 0;
}
