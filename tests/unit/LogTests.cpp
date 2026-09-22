#include <dk/core/Log.hpp>
#include <dk/core/StableId.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace {

std::filesystem::path test_directory()
{
    auto id = dk::AssetId::generate();
    REQUIRE(id.has_value());
    const auto path = std::filesystem::current_path() / "test-artifacts" / id->to_string();
    REQUIRE(std::filesystem::create_directories(path));
    return path;
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.is_open());
    std::string content{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    // spdlog uses native line endings; assertions compare logical records.
    content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());
    return content;
}

std::unique_ptr<dk::Logger> file_logger(
    const std::filesystem::path& path, dk::LogLevel level = dk::LogLevel::info)
{
    auto result = dk::Logger::create({level, false, path});
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace

TEST_CASE("logger filters levels and preserves literal messages", "[logging]")
{
    const auto path = test_directory() / "filter.log";
    auto logger = file_logger(path, dk::LogLevel::warning);
    REQUIRE_FALSE(logger->enabled(dk::LogLevel::info));
    REQUIRE(logger->enabled(dk::LogLevel::error));
    REQUIRE(logger->write(dk::LogLevel::info, "scene", "filtered message").has_value());
    REQUIRE(logger->write(dk::LogLevel::warning, "scene", "literal {braces}").has_value());
    REQUIRE(logger->log(dk::LogLevel::error, "assets", "asset {} failed", 42).has_value());
    REQUIRE(logger->flush().has_value());
    const auto output = read_file(path);
    REQUIRE(output.find("filtered message") == std::string::npos);
    REQUIRE(output.find("[scene] literal {braces}") != std::string::npos);
    REQUIRE(output.find("[assets] asset 42 failed") != std::string::npos);
    REQUIRE(output.find("[warning]") != std::string::npos);
}

TEST_CASE("logger validates configuration and file failures", "[logging]")
{
    const auto no_output = dk::Logger::create({dk::LogLevel::info, false, {}});
    REQUIRE_FALSE(no_output.has_value());
    REQUIRE(no_output.error().code == dk::ErrorCode::invalid_argument);

    const auto invalid_level = dk::Logger::create({static_cast<dk::LogLevel>(255), true, {}});
    REQUIRE_FALSE(invalid_level.has_value());
    REQUIRE(invalid_level.error().code == dk::ErrorCode::invalid_argument);

    const auto directory = test_directory();
    const auto directory_as_file = dk::Logger::create({dk::LogLevel::info, false, directory});
    REQUIRE_FALSE(directory_as_file.has_value());
    REQUIRE(directory_as_file.error().code == dk::ErrorCode::io_error);

    const auto missing_parent = directory / "missing" / "test.log";
    REQUIRE_FALSE(dk::Logger::create({dk::LogLevel::info, false, missing_parent}).has_value());
    REQUIRE_FALSE(std::filesystem::exists(missing_parent.parent_path()));
}

TEST_CASE("logger validates entries even when output is disabled", "[logging]")
{
    const auto path = test_directory() / "off.log";
    auto logger = file_logger(path, dk::LogLevel::off);
    REQUIRE_FALSE(logger->enabled(dk::LogLevel::critical));
    REQUIRE(logger->write(dk::LogLevel::critical, "core", "filtered").has_value());
    REQUIRE_FALSE(logger->write(dk::LogLevel::off, "core", "invalid").has_value());
    REQUIRE_FALSE(logger->write(dk::LogLevel::info, "", "invalid").has_value());
    REQUIRE_FALSE(logger->write(dk::LogLevel::info, "line\nbreak", "invalid").has_value());
    REQUIRE_FALSE(logger->log(dk::LogLevel::info, std::string{"a\0b", 3}, "{}", 1).has_value());
    REQUIRE(logger->flush().has_value());
    REQUIRE(read_file(path).empty());
}

TEST_CASE("logger supports unicode paths append and destructor flush", "[logging]")
{
    const auto path = test_directory() / std::filesystem::path{u8"日志-测试.txt"};
    {
        auto logger = file_logger(path);
        REQUIRE(logger->write(dk::LogLevel::info, "core", "first lifetime").has_value());
    }
    REQUIRE(read_file(path).find("first lifetime") != std::string::npos);
    {
        auto logger = file_logger(path);
        REQUIRE(logger->write(dk::LogLevel::info, "core", "second lifetime").has_value());
    }
    const auto output = read_file(path);
    REQUIRE(output.find("first lifetime") != std::string::npos);
    REQUIRE(output.find("second lifetime") != std::string::npos);
}

TEST_CASE("logger instances have independent lifetimes", "[logging]")
{
    const auto directory = test_directory();
    const auto first_path = directory / "first.log";
    const auto second_path = directory / "second.log";
    auto first = file_logger(first_path);
    auto second = file_logger(second_path);
    REQUIRE(first->write(dk::LogLevel::info, "one", "first only").has_value());
    first.reset();
    REQUIRE(second->write(dk::LogLevel::info, "two", "still alive").has_value());
    second.reset();
    REQUIRE(read_file(first_path).find("still alive") == std::string::npos);
    REQUIRE(read_file(second_path).find("still alive") != std::string::npos);
}

TEST_CASE("logger writes complete entries from multiple threads", "[logging]")
{
    const auto path = test_directory() / "parallel.log";
    auto logger = file_logger(path);
    std::atomic<int> failures{0};
    std::vector<std::jthread> workers;
    for (int thread = 0; thread < 4; ++thread) {
        workers.emplace_back([&, thread] {
            for (int index = 0; index < 64; ++index) {
                if (!logger->log(dk::LogLevel::info, "jobs", "worker {} item {}", thread, index)) {
                    ++failures;
                }
            }
        });
    }
    workers.clear();
    REQUIRE(failures.load() == 0);
    REQUIRE(logger->flush().has_value());
    const auto output = read_file(path);
    REQUIRE(std::count(output.begin(), output.end(), '\n') == 256);
    for (int thread = 0; thread < 4; ++thread) {
        for (int index = 0; index < 64; ++index) {
            REQUIRE(output.find(fmt::format("[jobs] worker {} item {}\n", thread, index))
                != std::string::npos);
        }
    }
}
