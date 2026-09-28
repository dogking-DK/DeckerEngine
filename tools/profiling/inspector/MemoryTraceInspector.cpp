#include <TracyFileRead.hpp>
#include <TracyWorker.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    const bool arena = argc == 4 && std::strcmp(argv[3], "arena") == 0;
    const bool pool = argc == 5 && std::strcmp(argv[3], "pool") == 0;
    const bool context = argc == 5 && std::strcmp(argv[3], "context") == 0;
    const bool benchmark = argc == 5 && std::strcmp(argv[3], "benchmark") == 0;
    if ((argc != 3 && !arena && !pool && !context && !benchmark) || (std::strcmp(argv[2], "on") && std::strcmp(argv[2], "off"))) { return 2; }
    const bool expected = std::strcmp(argv[2], "on") == 0;
    try {
        nlohmann::json measured;
        if (pool || context || benchmark) {
            std::ifstream input{argv[4]}; input >> measured;
        }
        if (pool) {
            if (measured.at("requests") != 192 || measured.at("memory_enabled") != expected
                || measured.at("backing_allocations").get<std::size_t>() == 0
                || measured.at("backing_allocations").get<std::size_t>() >= 192) {
                throw std::runtime_error("invalid pool probe measurements");
            }
        }
        if (context && (measured.at("systems") != 2 || measured.at("tasks") != 3
            || measured.at("memory_enabled") != expected || measured.at("assets_allocations") != 2
            || measured.at("scene_allocations") != 3 || measured.at("jobs_allocations").get<std::size_t>() == 0)) {
            throw std::runtime_error("invalid context probe measurements");
        }
        if (benchmark && (measured.at("status") != "passed" || measured.at("memory_enabled") != expected
            || measured.at("case_count").get<std::size_t>() < 16 || measured.at("minimum_cross_frees").get<std::size_t>() == 0)) {
            throw std::runtime_error("invalid benchmark measurements");
        }
        auto file = std::unique_ptr<tracy::FileRead>{tracy::FileRead::Open(argv[1])};
        if (!file) { throw std::runtime_error("cannot open capture"); }
        tracy::Worker worker{*file};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        while (!worker.AreSourceLocationZonesReady()) {
            if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error("capture load timed out"); }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        if (worker.GetFailureType() != tracy::Worker::Failure::None) { throw std::runtime_error("Tracy reported a capture failure"); }
        nlohmann::json pools = nlohmann::json::array();
        std::size_t total = 0, cross_thread = 0;
        std::map<std::string, std::pair<std::size_t, std::uint64_t>> counts;
        for (const auto& item : worker.GetMemNameMap()) {
            const auto& memory = *item.second;
            if (memory.data.empty()) { continue; }
            const std::string name = worker.GetString(item.first);
            if (memory.usage || !memory.active.empty()) { throw std::runtime_error("live allocations remain in capture"); }
            std::uint64_t bytes = 0;
            for (const auto& event : memory.data) {
                if (event.TimeFree() < event.TimeAlloc()) { throw std::runtime_error("missing or out-of-order free event"); }
                bytes += event.Size();
                if (event.ThreadAlloc() != event.ThreadFree()) { ++cross_thread; }
            }
            const auto count = static_cast<std::size_t>(memory.data.size());
            counts[name].first += count; counts[name].second += bytes; total += count;
            pools.push_back({{"name", name}, {"allocations", count}, {"bytes", bytes}, {"live_bytes", memory.usage}});
        }
        if (expected && benchmark) {
            if (total != measured.at("backing_allocations").get<std::size_t>() || counts.size() != 2
                || counts["dk/heap/jobs"].first != measured.at("jobs_allocations").get<std::size_t>()
                || counts["dk/heap/assets"].first != measured.at("assets_allocations").get<std::size_t>()
                || cross_thread < measured.at("minimum_cross_frees").get<std::size_t>()) {
                throw std::runtime_error("benchmark events differ from independent counters or expected cross-thread frees");
            }
        } else if (expected && context) {
            if (total != measured.at("backing_allocations").get<std::size_t>() || counts.size() != 3 || cross_thread != 3
                || counts["dk/heap/assets"].first != measured.at("assets_allocations").get<std::size_t>()
                || counts["dk/heap/scene"].first != measured.at("scene_allocations").get<std::size_t>()
                || counts["dk/heap/jobs"].first != measured.at("jobs_allocations").get<std::size_t>()) {
                throw std::runtime_error("context capture differs from independent heap counts or delayed cross-thread frees");
            }
        } else if (expected && pool) {
            if (total != measured.at("backing_allocations").get<std::size_t>() || counts.size() != 1
                || counts["dk/heap/jobs"].second != measured.at("backing_bytes_total").get<std::uint64_t>()) {
                throw std::runtime_error("pool capture must match backing measurements without per-object duplicates");
            }
        } else if (expected && arena) {
            if (total != 3 || counts.size() != 1 || counts["dk/heap/jobs"] != std::pair<std::size_t, std::uint64_t>{3, 3200}
                || cross_thread != 0) {
                throw std::runtime_error("arena must emit only three paired backing chunks totaling 3200 bytes");
            }
        } else if (expected) {
            if (total != 36 || counts.size() != 2 || counts["dk/heap/assets"] != std::pair<std::size_t, std::uint64_t>{35, 2241}
                || counts["dk/heap/scene"] != std::pair<std::size_t, std::uint64_t>{1, 32} || cross_thread != 2) {
                throw std::runtime_error("captured memory workload differs from expected pools, sizes or threads");
            }
        } else if (total != 0) { throw std::runtime_error("memory events exist with DK_PROFILE_MEMORY=OFF"); }
        nlohmann::json plots = nlohmann::json::array();
        std::map<std::string, std::pair<double, double>> expected_plots{
            {"dk/scratch/used", {2912, 0}}, {"dk/scratch/retained", {1024, 0}},
            {"dk/scratch/backing", {3200, 0}}, {"dk/scratch/sampled-peak", {2912, 2912}}};
        if (pool) {
            expected_plots = {
                {"dk/pool/local/live", {512, 0}}, {"dk/pool/local/sampled-peak", {512, 512}},
                {"dk/pool/shared/live", {1024, 0}}, {"dk/pool/shared/sampled-peak", {1024, 1024}},
                {"dk/pool/local/backing", {measured.at("local_backing_peak").get<double>(), 0}},
                {"dk/pool/shared/backing", {measured.at("shared_backing_peak").get<double>(), 0}},
                {"dk/pool/local/idle-backing", {measured.at("local_idle_peak").get<double>(), 0}},
                {"dk/pool/shared/idle-backing", {measured.at("shared_idle_peak").get<double>(), 0}}};
        }
        if (context || benchmark) {
            expected_plots = {
                {"dk/scratch/used", {}}, {"dk/scratch/retained", {}}, {"dk/scratch/backing", {}}, {"dk/scratch/sampled-peak", {}},
                {"dk/pool/local/live", {}}, {"dk/pool/local/backing", {}}, {"dk/pool/local/idle-backing", {}},
                {"dk/pool/local/sampled-peak", {}}};
            if (benchmark) {
                expected_plots.insert({{"dk/pool/shared/live", {}}, {"dk/pool/shared/backing", {}},
                    {"dk/pool/shared/idle-backing", {}}, {"dk/pool/shared/sampled-peak", {}}});
            }
        }
        for (const auto* plot : worker.GetPlots()) {
            if (plot->type != tracy::PlotType::User) { continue; }
            const std::string name = worker.GetString(plot->name);
            if (!name.starts_with("dk/scratch/") && !name.starts_with("dk/pool/")) { continue; }
            if (!expected || (!arena && !pool && !context && !benchmark)) { throw std::runtime_error("unexpected memory curve"); }
            const auto match = expected_plots.find(name);
            if (match == expected_plots.end() || plot->data.empty() || plot->min < 0
                || (!context && !benchmark && (plot->max != match->second.first || plot->data.back().val != match->second.second))
                || plot->format != tracy::PlotValueFormatting::Memory) {
                throw std::runtime_error("memory curve peak, final value or format differs from workload: " + name);
            }
            if ((context || benchmark) && (plot->max <= 0 || plot->data.back().val != (name.ends_with("sampled-peak") ? plot->max : 0))) {
                throw std::runtime_error("context retirement did not clear local memory curves: " + name);
            }
            plots.push_back({{"name", name}, {"samples", plot->data.size()}, {"peak", plot->max}, {"final", plot->data.back().val}});
            expected_plots.erase(match);
        }
        if (expected && (arena || pool || context || benchmark) && !expected_plots.empty()) { throw std::runtime_error("missing memory curves"); }
        std::cout << nlohmann::json{{"status", "passed"}, {"memory_enabled", expected}, {"allocations", total},
            {"cross_thread_frees", cross_thread}, {"pools", pools},
            {"scratch_plots", (pool || context || benchmark) ? nlohmann::json::array() : plots},
            {"pool_plots", pool ? plots : nlohmann::json::array()},
            {"context_plots", context ? plots : nlohmann::json::array()},
            {"benchmark_plots", benchmark ? plots : nlohmann::json::array()}}.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
