#include <dk/core/Result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace {

dk::Result<int> parse_count()
{
    return std::unexpected(dk::Error{dk::ErrorCode::invalid_argument,
        "Expected a positive count", {"parse_count"}});
}

dk::Result<int> load_count()
{
    auto parsed = parse_count();
    if (!parsed) {
        return std::unexpected(parsed.error().with_context("load_count"));
    }
    return parsed;
}

} // namespace

TEST_CASE("result propagates error code message and ordered context", "[error]")
{
    const auto result = load_count();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == dk::ErrorCode::invalid_argument);
    REQUIRE(result.error().message == "Expected a positive count");
    REQUIRE((result.error().context == std::vector<std::string>{"parse_count", "load_count"}));

    const auto original = parse_count();
    const auto extended = original.error().with_context("caller");
    REQUIRE(original.error().context.size() == 1);
    REQUIRE(extended.context.back() == "caller");
}

TEST_CASE("result supports void and move only values", "[error]")
{
    dk::Result<void> success{};
    REQUIRE(success.has_value());
    dk::Result<void> failure = std::unexpected(dk::Error{dk::ErrorCode::io_error, "write failed"});
    REQUIRE_FALSE(failure.has_value());
    REQUIRE(failure.error().code == dk::ErrorCode::io_error);

    dk::Result<std::unique_ptr<int>> value = std::make_unique<int>(42);
    auto moved = std::move(value);
    REQUIRE(moved.has_value());
    REQUIRE(**moved == 42);
}

TEST_CASE("error names have stable fallback", "[error]")
{
    REQUIRE(dk::error_code_name(dk::ErrorCode::io_error) == "io_error");
    REQUIRE(dk::error_code_name(dk::ErrorCode::invalid_argument) == "invalid_argument");
    REQUIRE(dk::error_code_name(static_cast<dk::ErrorCode>(999)) == "unknown");
}
