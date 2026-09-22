#include <dk/core/StableId.hpp>
#include <dk/io/File.hpp>
#include <dk/io/Path.hpp>
#include <dk/math/Math.hpp>
#include <dk/math/Transform.hpp>
#ifdef DK_DEMO_WITH_LOGGING
#include <dk/core/Log.hpp>
#endif

#include <charconv>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>

namespace {

struct Sample {
    dk::EntityId id;
    dk::Transformd world;
};

std::unexpected<dk::Error> failure(dk::ErrorCode code, std::string message)
{
    return std::unexpected(dk::Error{code, std::move(message), {"foundation_sample"}});
}

// Example-only format; this is not the engine's future scene protocol.
class Tokens {
public:
    explicit Tokens(std::string_view text) : remaining_{text} {}
    std::string_view next()
    {
        constexpr std::string_view whitespace = " \t\r\n";
        const auto begin = remaining_.find_first_not_of(whitespace);
        if (begin == std::string_view::npos) { remaining_ = {}; return {}; }
        remaining_.remove_prefix(begin);
        const auto end = remaining_.find_first_of(whitespace);
        if (end == std::string_view::npos) { return std::exchange(remaining_, {}); }
        const auto token = remaining_.substr(0, end);
        remaining_.remove_prefix(end);
        return token;
    }
private:
    std::string_view remaining_;
};

dk::Result<Sample> decode(std::string_view text)
{
    Tokens tokens{text};
    if (tokens.next() != "DK_FOUNDATION_SAMPLE") {
        return failure(dk::ErrorCode::invalid_argument, "Invalid sample header");
    }
    if (tokens.next() != "1") {
        return failure(dk::ErrorCode::not_supported, "Unsupported sample version");
    }
    if (tokens.next() != "entity") {
        return failure(dk::ErrorCode::invalid_argument, "Expected entity field");
    }
    const auto id = dk::EntityId::parse(tokens.next());
    if (!id) { return std::unexpected(id.error().with_context("sample.decode.entity")); }
    if (id->is_nil()) { return failure(dk::ErrorCode::invalid_argument, "Sample entity ID cannot be nil"); }
    if (tokens.next() != "matrix") {
        return failure(dk::ErrorCode::invalid_argument, "Expected matrix field");
    }
    dk::Mat4d matrix;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const auto token = tokens.next();
            if (token.empty()) { return failure(dk::ErrorCode::invalid_argument, "Truncated sample matrix"); }
            double value = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !std::isfinite(value)) {
                return failure(dk::ErrorCode::invalid_argument, "Invalid matrix number");
            }
            matrix(row, column) = value;
        }
    }
    if (!tokens.next().empty()) { return failure(dk::ErrorCode::invalid_argument, "Trailing sample data"); }
    const auto transform = dk::Transformd::from_matrix(matrix);
    if (!transform) { return std::unexpected(transform.error().with_context("sample.decode.matrix")); }
    return Sample{*id, *transform};
}

dk::Result<std::string> encode(const Sample& sample)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "DK_FOUNDATION_SAMPLE 1\nentity " << sample.id.to_string() << "\nmatrix";
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) { stream << ' ' << sample.world.matrix()(row, column); }
    }
    stream << '\n';
    if (!stream) { return failure(dk::ErrorCode::internal_error, "Cannot encode sample"); }
    return stream.str();
}

dk::Result<dk::Vec3d> evaluate(const Sample& sample)
{
    const dk::Vec3d point{1, 2, 3};
    const auto world = sample.world.transform_point(point);
    if (!world) { return std::unexpected(world.error().with_context("sample.evaluate.point")); }
    const auto inverse = sample.world.inverse();
    if (!inverse) { return std::unexpected(inverse.error().with_context("sample.evaluate.inverse")); }
    const auto restored = inverse->transform_point(*world);
    if (!restored) { return std::unexpected(restored.error().with_context("sample.evaluate.restore")); }
    if (!restored->isApprox(point, 1e-12)) {
        return failure(dk::ErrorCode::invalid_state, "Point inverse roundtrip exceeded tolerance");
    }
    return *world;
}

dk::Result<Sample> make_sample()
{
    const auto id = dk::EntityId::generate();
    if (!id) { return std::unexpected(id.error().with_context("sample.create.id")); }
    const auto parent_rotation = dk::rotation_from_axis_angle(dk::Vec3d{0, 0, 1}, dk::radians(90.0));
    const auto local_rotation = dk::rotation_from_axis_angle(dk::Vec3d{0, 0, 1}, dk::radians(45.0));
    if (!parent_rotation) { return std::unexpected(parent_rotation.error()); }
    if (!local_rotation) { return std::unexpected(local_rotation.error()); }
    dk::Trsd parent_trs;
    parent_trs.translation = dk::Vec3d{10, 0, 0};
    parent_trs.rotation = *parent_rotation;
    parent_trs.scale = dk::Vec3d{2, 3, 1};
    dk::Trsd local_trs;
    local_trs.translation = dk::Vec3d{1, 2, 3};
    local_trs.rotation = *local_rotation;
    local_trs.scale = dk::Vec3d{1, 2, 0.5};
    const auto parent = dk::Transformd::from_trs(parent_trs);
    const auto local = dk::Transformd::from_trs(local_trs);
    if (!parent) { return std::unexpected(parent.error()); }
    if (!local) { return std::unexpected(local.error()); }
    const auto world = parent->compose(*local);
    if (!world) { return std::unexpected(world.error()); }
    const auto local_point = local->transform_point(dk::Vec3d{1, 2, 3});
    if (!local_point) { return std::unexpected(local_point.error()); }
    const auto sequential = parent->transform_point(*local_point);
    if (!sequential) { return std::unexpected(sequential.error()); }
    Sample sample{*id, *world};
    const auto composed = evaluate(sample);
    if (!composed) { return std::unexpected(composed.error()); }
    if (!sequential->isApprox(*composed, 1e-12)) {
        return failure(dk::ErrorCode::invalid_state, "Parent/local composition mismatch");
    }
    return sample;
}

dk::Result<Sample> load_sample(const std::filesystem::path& file)
{
    const auto bytes = dk::read_file_bytes(file, 4096);
    if (!bytes) { return std::unexpected(bytes.error().with_context("sample.load")); }
    if (bytes->empty()) { return failure(dk::ErrorCode::invalid_argument, "Empty sample"); }
    const std::string_view text{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
    auto sample = decode(text);
    if (!sample) { return std::unexpected(sample.error().with_context("sample.load.decode")); }
    return sample;
}

dk::Result<Sample> execute(bool save, const std::filesystem::path& root, const std::filesystem::path& relative)
{
    const auto project = dk::ProjectPaths::create(root);
    if (!project) { return std::unexpected(project.error().with_context("sample.project")); }
    const auto file = project->resolve(relative);
    if (!file) { return std::unexpected(file.error().with_context("sample.resolve")); }
    if (!save) { return load_sample(*file); }
    const auto sample = make_sample();
    if (!sample) { return std::unexpected(sample.error().with_context("sample.create")); }
    const auto text = encode(*sample);
    if (!text) { return std::unexpected(text.error()); }
    const auto written = dk::write_file_bytes_atomic(*file, std::as_bytes(std::span{text->data(), text->size()}));
    if (!written) { return std::unexpected(written.error().with_context("sample.save")); }
    const auto restored = load_sample(*file);
    if (!restored) { return std::unexpected(restored.error().with_context("sample.save.readback")); }
    if (sample->id != restored->id || !(sample->world.matrix().array() == restored->world.matrix().array()).all()) {
        return failure(dk::ErrorCode::invalid_state, "Saved identity or matrix changed on reload");
    }
    return restored;
}

void diagnostic(std::string_view message, bool is_error)
{
#ifdef DK_DEMO_WITH_LOGGING
    auto logger = dk::Logger::create();
    if (logger) {
        const auto logged = (*logger)->write(is_error ? dk::LogLevel::error : dk::LogLevel::info,
            "foundation-demo", message);
        if (logged && (*logger)->flush()) { return; }
    }
#else
    (void)is_error;
#endif
    std::cerr << "[foundation-demo] " << message << '\n';
}

int report_error(const dk::Error& error)
{
    std::ostringstream stream;
    stream << dk::error_code_name(error.code) << ": " << error.message;
    for (const auto& context : error.context) { stream << "\n  " << context; }
    diagnostic(stream.str(), true);
    return 1;
}

int run(bool save, const std::filesystem::path& root, const std::filesystem::path& relative)
{
    const auto sample = execute(save, root, relative);
    if (!sample) { return report_error(sample.error()); }
    const auto point = evaluate(*sample);
    if (!point) { return report_error(point.error()); }
    diagnostic(save ? "Saved and reloaded sample" : "Loaded sample", false);
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10)
              << "entity_id=" << sample->id.to_string() << "\nworld_point="
              << point->x() << ' ' << point->y() << ' ' << point->z()
              << "\nlocal_point=1 2 3\nroundtrip=ok\n";
    return 0;
}

template <typename Character>
int entry(int argc, Character* argv[], std::basic_string_view<Character> save,
    std::basic_string_view<Character> load, std::basic_string_view<Character> help)
{
    try {
        if (argc == 2 && std::basic_string_view<Character>{argv[1]} == help) {
            std::cout << "Usage: dk-foundation-demo <save|load> <existing-project-root> <relative-file>\n";
            return 0;
        }
        const std::basic_string_view<Character> command = argc > 1 ? argv[1] : std::basic_string_view<Character>{};
        if (argc != 4 || (command != save && command != load)) {
            diagnostic("Usage: dk-foundation-demo <save|load> <existing-project-root> <relative-file>", true);
            return 2;
        }
        return run(command == save, std::filesystem::path{argv[2]}, std::filesystem::path{argv[3]});
    } catch (const std::exception& error) {
        std::cerr << "[foundation-demo] internal_error: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
    return entry<wchar_t>(argc, argv, L"save", L"load", L"--help");
}
#else
int main(int argc, char* argv[])
{
    return entry<char>(argc, argv, "save", "load", "--help");
}
#endif
