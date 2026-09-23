#include <TracyFileRead.hpp>
#include <TracyWorker.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 3 || (std::strcmp(argv[2], "on") && std::strcmp(argv[2], "off"))) { return 2; }
    const bool expected = std::strcmp(argv[2], "on") == 0;
    try {
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
        if (expected) {
            if (total != 36 || counts.size() != 2 || counts["dk/heap/assets"] != std::pair<std::size_t, std::uint64_t>{35, 2241}
                || counts["dk/heap/scene"] != std::pair<std::size_t, std::uint64_t>{1, 32} || cross_thread != 2) {
                throw std::runtime_error("captured memory workload differs from expected pools, sizes or threads");
            }
        } else if (total != 0) { throw std::runtime_error("memory events exist with DK_PROFILE_MEMORY=OFF"); }
        std::cout << nlohmann::json{{"status", "passed"}, {"memory_enabled", expected}, {"allocations", total},
            {"cross_thread_frees", cross_thread}, {"pools", pools}}.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
