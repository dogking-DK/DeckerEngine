#include <dk/memory/Containers.hpp>
#include <dk/memory/Context.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/ObjectPool.hpp>
#include <dk/memory/SmartPtr.hpp>
#include <dk/profiling/Memory.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace dk::memory;
using Clock = std::chrono::steady_clock;
enum class Strategy { heap, pmr, arena, local_pool, shared_pool };
constexpr std::array names{"heap", "pmr", "arena", "local_pool", "shared_pool"};
struct Options { std::size_t rounds = 4, warmup = 1, batch = 8, threads = 2, budget = 0; bool capture = false; };
struct Result {
    const char* strategy = "pipeline";
    bool cross = false;
    std::size_t bytes = 0, alignment = 0, threads = 0;
    std::uint64_t requests = 0, checksum = 0, allocations = 0, jobs = 0, assets = 0;
    std::size_t heap_peak = 0, retained = 0, logical_peak_sum = 0;
    double elapsed_ns = 0, p50 = 0, p95 = 0, p99 = 0;
};
void require(bool value, const char* reason)
{ if (!value) { throw std::runtime_error(reason); } }
MemorySystem system()
{ auto value = MemorySystem::create(); require(value.has_value(), "system creation"); return std::move(*value); }
ResourceHandle heap(MemorySystem& owner, DomainCategory category, std::size_t budget = 0)
{ auto value = owner.create_heap({"benchmark", category, budget}); require(value.has_value(), "heap creation"); return *value; }
double nanoseconds(Clock::duration elapsed)
{ return std::chrono::duration<double, std::nano>(elapsed).count(); }
void quantiles(Result& result, std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](std::size_t percent) { return samples[(samples.size() * percent + 99) / 100 - 1]; };
    result.p50 = percentile(50); result.p95 = percentile(95); result.p99 = percentile(99);
}
std::uint64_t seed(std::size_t thread, std::size_t round, std::size_t block, std::size_t batch)
{ return 1 + thread * 100000000ULL + round * batch + block; }
constexpr auto tail_mask = 0xd6e8feb86659fd93ULL;
void write_payload(void* pointer, std::size_t bytes, std::size_t alignment, std::uint64_t value)
{
    require(reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0, "misaligned payload");
    std::memcpy(pointer, &value, sizeof(value)); value ^= tail_mask;
    std::memcpy(static_cast<std::byte*>(pointer) + bytes - sizeof(value), &value, sizeof(value));
}
std::uint64_t read_payload(void* pointer, std::size_t bytes, std::uint64_t expected)
{
    std::uint64_t first = 0, last = 0;
    std::memcpy(&first, pointer, sizeof(first));
    std::memcpy(&last, static_cast<std::byte*>(pointer) + bytes - sizeof(last), sizeof(last));
    require(first == expected && last == (expected ^ tail_mask), "payload corrupted or wrong handoff");
    return first;
}

Result run_case(Strategy strategy, bool cross, std::size_t bytes, std::size_t alignment,
                std::size_t threads, const Options& options)
{
    DK_PROFILE_ZONE("Memory.Benchmark.Case");
    auto owner = system(); auto resource = heap(owner, DomainCategory::jobs, options.budget);
    SharedPoolResource shared;
    if (strategy == Strategy::shared_pool) {
        auto created = SharedPoolResource::create(resource, {32, 4096}); require(created.has_value(), "shared pool creation");
        shared = *created;
    }
    struct WorkerData {
        std::vector<double> latency;
        std::uint64_t checksum = 0;
        std::size_t logical_peak = 0;
        std::exception_ptr failure;
    };
    std::vector<WorkerData> data(threads);
    for (auto& worker : data) { worker.latency.resize(options.rounds); }
    std::vector<void*> blocks(threads * options.batch, nullptr);
    std::atomic<bool> failed{false};
    std::barrier exchange{static_cast<std::ptrdiff_t>(threads)};
    int phase = 0; Clock::time_point begin, end;
    std::barrier phases{static_cast<std::ptrdiff_t>(threads + 1), [&]() noexcept {
        if (phase == 1) { begin = Clock::now(); }
        if (phase == 2) { end = Clock::now(); }
        ++phase;
    }};
    std::vector<std::jthread> workers; workers.reserve(threads);
    std::exception_ptr launch_failure;
    try {
        for (std::size_t id = 0; id < threads; ++id) {
            workers.emplace_back([&, id] {
                DK_PROFILE_THREAD_NAME("dk-memory-bench-worker");
                auto& local_data = data[id];
                std::unique_ptr<ScratchArena> arena;
                std::unique_ptr<LocalPoolResource> local;
                auto fail = [&] { local_data.failure = std::current_exception(); failed.store(true); };
                try {
                    if (strategy == Strategy::arena) { arena = std::make_unique<ScratchArena>(resource); }
                    if (strategy == Strategy::local_pool) { local = std::make_unique<LocalPoolResource>(resource, PoolOptions{32, 4096}); }
                } catch (...) { fail(); }
                auto batch = [&](std::size_t round, bool timed) {
                    DK_PROFILE_ZONE("Memory.Benchmark.Batch");
                    std::optional<ScratchCheckpoint> checkpoint;
                    try {
                        if (!failed.load(std::memory_order_relaxed)) {
                            if (arena) {
                                auto value = arena->try_checkpoint(); require(value.has_value(), "arena checkpoint"); checkpoint = *value;
                            }
                            for (std::size_t i = 0; i < options.batch; ++i) {
                                void* pointer = nullptr;
                                switch (strategy) {
                                case Strategy::heap: {
                                    auto value = resource.try_allocate(bytes, alignment); require(value.has_value(), "heap allocation"); pointer = *value; break;
                                }
                                case Strategy::pmr: pointer = resource.pmr_resource()->allocate(bytes, alignment); break;
                                case Strategy::arena: {
                                    auto value = arena->try_allocate(bytes, alignment); require(value.has_value(), "arena allocation"); pointer = *value; break;
                                }
                                case Strategy::local_pool: {
                                    auto value = local->try_allocate(bytes, alignment); require(value.has_value(), "local allocation"); pointer = *value; break;
                                }
                                case Strategy::shared_pool: {
                                    auto value = shared.try_allocate(bytes, alignment); require(value.has_value(), "shared allocation"); pointer = *value; break;
                                }
                                }
                                blocks[id * options.batch + i] = pointer;
                                write_payload(pointer, bytes, alignment, seed(id, round, i, options.batch));
                            }
                        }
                    } catch (...) { fail(); }
                    if (!timed && round == 0) {
                        // Outside timing: all blocks remain live until the second barrier, so sampling is quiescent.
                        exchange.arrive_and_wait();
                        if (local && !local->try_sample()) { failed.store(true); }
                        if (id == 0 && shared && !shared.try_sample()) { failed.store(true); }
                        exchange.arrive_and_wait();
                    }
                    if (cross) { exchange.arrive_and_wait(); }
                    const auto source = cross ? (id + threads - 1) % threads : id;
                    for (std::size_t i = 0; i < options.batch; ++i) {
                        auto*& pointer = blocks[source * options.batch + i];
                        if (!pointer) { continue; }
                        try {
                            const auto value = read_payload(pointer, bytes, seed(source, round, i, options.batch));
                            if (timed) { local_data.checksum += value; }
                        } catch (...) { fail(); }
                        switch (strategy) {
                        case Strategy::heap: resource.deallocate(pointer, bytes, alignment); break;
                        case Strategy::pmr: resource.pmr_resource()->deallocate(pointer, bytes, alignment); break;
                        case Strategy::arena: break; // Batch rewind below owns reclamation.
                        case Strategy::local_pool: local->deallocate(pointer, bytes, alignment); break;
                        case Strategy::shared_pool: shared.deallocate(pointer, bytes, alignment); break;
                        }
                        pointer = nullptr;
                    }
                    if (checkpoint && !arena->try_rewind(*checkpoint)) { failed.store(true); }
                    if (cross) { exchange.arrive_and_wait(); }
                };
                phases.arrive_and_wait(); // Setup complete.
                for (std::size_t i = 0; i < options.warmup; ++i) { batch(i, false); }
                phases.arrive_and_wait(); // Completion starts the shared wall clock.
                for (std::size_t i = 0; i < options.rounds; ++i) {
                    const auto start = Clock::now(); batch(i + options.warmup, true);
                    local_data.latency[i] = nanoseconds(Clock::now() - start);
                }
                phases.arrive_and_wait(); // Completion ends timing after the slowest worker.
                if (arena) { local_data.logical_peak = arena->snapshot().peak_used_bytes; }
                if (local) {
                    local_data.logical_peak = local->snapshot().peak_logical_bytes;
                    if (!local->try_sample()) { failed.store(true); }
                }
                phases.arrive_and_wait(); // Publish diagnostic samples.
                phases.arrive_and_wait(); // Main takes the quiescent heap snapshot before releasing locals.
            });
        }
    } catch (...) {
        launch_failure = std::current_exception(); failed.store(true);
        for (auto missing = workers.size(); missing < threads; ++missing) {
            exchange.arrive_and_drop(); phases.arrive_and_drop();
        }
    }
    phases.arrive_and_wait(); phases.arrive_and_wait(); phases.arrive_and_wait(); phases.arrive_and_wait();
    const auto retained = resource.snapshot().backing_requested_bytes;
    const auto shared_peak = shared ? shared.snapshot().peak_logical_bytes : 0;
    if (shared && !shared.try_sample()) { failed.store(true); }
    phases.arrive_and_wait();
    for (auto& worker : workers) { worker.join(); }
    if (shared) { require(shared.try_trim().has_value(), "shared trim"); require(shared.try_close().closed(), "shared close"); }
    const auto final = resource.snapshot();
    require(final.live_allocations == 0 && final.backing_requested_bytes == 0, "case leaked backing");
    require(owner.try_close().closed(), "case close busy");
    if (launch_failure) { std::rethrow_exception(launch_failure); }
    for (const auto& worker : data) { if (worker.failure) { std::rethrow_exception(worker.failure); } }
    require(!failed.load(), "worker operation failed");
    Result result; result.strategy = names[static_cast<std::size_t>(strategy)]; result.cross = cross;
    result.bytes = bytes; result.alignment = alignment; result.threads = threads;
    result.requests = threads * options.rounds * options.batch;
    result.elapsed_ns = nanoseconds(end - begin); result.heap_peak = final.peak_backing_bytes; result.retained = retained;
    result.jobs = result.allocations = final.allocation_count; result.logical_peak_sum = shared_peak;
    std::vector<double> latency; latency.reserve(threads * options.rounds);
    std::uint64_t expected = 0;
    for (std::size_t id = 0; id < threads; ++id) {
        result.checksum += data[id].checksum; result.logical_peak_sum += data[id].logical_peak;
        latency.insert(latency.end(), data[id].latency.begin(), data[id].latency.end());
        const auto count = options.rounds * options.batch;
        expected += count * (1 + id * 100000000ULL + options.warmup * options.batch) + count * (count - 1) / 2;
    }
    require(result.checksum == expected, "case checksum mismatch");
    quantiles(result, std::move(latency)); return result;
}

Result run_pipeline(const Options& options)
{
    DK_PROFILE_ZONE("Memory.Benchmark.Pipeline");
    auto owner = system(); auto assets = heap(owner, DomainCategory::assets); auto jobs = heap(owner, DomainCategory::jobs);
    const auto token = RoutingToken::from_resource(assets);
    struct Output { dk::Vector<std::uint64_t> values; };
    std::shared_ptr<Output> output; std::weak_ptr<Output> final_weak;
    std::barrier sync{2}; std::exception_ptr failure;
    std::size_t retained = 0, logical_peak = 0, stable_backing = 0;
    const auto rounds = options.warmup + options.rounds;
    // Allocate consumer metadata before the producer can wait at a barrier.
    std::uint64_t checksum = 0; std::vector<double> latency(options.rounds);
    std::jthread producer{[&] {
        DK_PROFILE_THREAD_NAME("dk-memory-bench-producer");
        std::unique_ptr<ThreadContextCache> cache;
        try { cache = std::make_unique<ThreadContextCache>(); } catch (...) { failure = std::current_exception(); }
        for (std::size_t round = 0; round < rounds; ++round) {
            sync.arrive_and_wait();
            if (!failure) {
                try {
                    DK_PROFILE_ZONE("Memory.Benchmark.Import");
                    auto& context = cache->acquire(token, {jobs, {}, jobs, {32, 4096}});
                    {
                        ExecutionScope execution{context, token}; ScratchScope scratch;
                        auto temporary = scratch_vector<std::uint64_t>(); temporary.resize(256);
                        ObjectPool<std::uint64_t> intermediate{current_local_pool()};
                        auto factor = intermediate.make(round + 1);
                        for (std::size_t i = 0; i < temporary.size(); ++i) { temporary[i] = *factor * (i + 1); }
                        auto candidate = make_shared<Output>(); candidate->values.assign(temporary.begin(), temporary.end());
                        output = std::move(candidate); // Publish owning result only after construction succeeds.
                    }
                    require(!try_current_resource(), "pipeline TLS residual");
                    require(context.local_pool().try_sample().has_value(), "pipeline pool sample");
                    const auto backing = jobs.snapshot().backing_requested_bytes;
                    if (round == 0) { stable_backing = backing; }
                    require(backing == stable_backing, "fixed pipeline workload keeps growing local backing");
                    logical_peak = context.scratch().snapshot().peak_used_bytes + context.local_pool().snapshot().peak_logical_bytes;
                } catch (...) { failure = std::current_exception(); }
            }
            sync.arrive_and_wait(); sync.arrive_and_wait(); // Consumer has destroyed the result.
        }
        retained = jobs.snapshot().backing_requested_bytes;
    }};
    std::exception_ptr consumer_failure;
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto begin = Clock::now(); sync.arrive_and_wait(); sync.arrive_and_wait();
        try {
            if (output) {
                require(output->values.get_allocator().resource() == assets, "pipeline output domain");
                for (std::size_t i = 0; i < output->values.size(); ++i) {
                    require(output->values[i] == (round + 1) * (i + 1), "pipeline payload mismatch");
                    if (round >= options.warmup) { checksum += output->values[i]; }
                }
                if (round + 1 == rounds) { final_weak = output; }
            }
        } catch (...) { consumer_failure = std::current_exception(); }
        output.reset(); sync.arrive_and_wait();
        if (round >= options.warmup) { latency[round - options.warmup] = nanoseconds(Clock::now() - begin); }
    }
    producer.join();
    require(jobs.snapshot().backing_requested_bytes == 0, "pipeline context cache survived worker exit");
    const auto busy = owner.try_close(); require(!busy.closed() && busy.active_contexts == 0, "pipeline weak did not retain result control");
    final_weak.reset(); require(owner.try_close().closed(), "pipeline close busy after weak release");
    if (failure) { std::rethrow_exception(failure); }
    if (consumer_failure) { std::rethrow_exception(consumer_failure); }
    const auto expected = 256ULL * 257 / 2 * (options.rounds * (options.warmup + 1) + options.rounds * (options.rounds - 1) / 2);
    require(checksum == expected, "pipeline checksum mismatch");
    Result result; result.cross = true; result.threads = 2; result.bytes = 256 * sizeof(std::uint64_t); result.alignment = alignof(std::uint64_t);
    result.requests = options.rounds; result.checksum = checksum;
    result.jobs = jobs.snapshot().allocation_count; result.assets = assets.snapshot().allocation_count;
    result.allocations = result.jobs + result.assets;
    result.heap_peak = jobs.snapshot().peak_backing_bytes + assets.snapshot().peak_backing_bytes;
    result.retained = retained; result.logical_peak_sum = logical_peak;
    result.elapsed_ns = std::accumulate(latency.begin(), latency.end(), 0.0);
    quantiles(result, std::move(latency)); return result;
}

void print_result(const Result& r)
{
    std::cout << "{\"kind\":\"case\",\"strategy\":\"" << r.strategy << "\",\"transfer\":\"" << (r.cross ? "cross" : "same")
        << "\",\"bytes\":" << r.bytes << ",\"alignment\":" << r.alignment << ",\"threads\":" << r.threads
        << ",\"requests\":" << r.requests << ",\"checksum\":" << r.checksum << ",\"elapsed_ns\":" << r.elapsed_ns
        << ",\"requests_per_second\":" << static_cast<double>(r.requests) * 1e9 / r.elapsed_ns
        << ",\"batch_p50_ns\":" << r.p50 << ",\"batch_p95_ns\":" << r.p95 << ",\"batch_p99_ns\":" << r.p99
        << ",\"heap_peak_bytes\":" << r.heap_peak << ",\"retained_bytes\":" << r.retained
        << ",\"heap_peak_kind\":\"" << (std::string_view{r.strategy} == "pipeline" ? "sum_domain_peaks" : "single_heap_peak") << '"'
        << ",\"local_logical_peak_sum\":" << r.logical_peak_sum << ",\"backing_allocations\":" << r.allocations
        << ",\"jobs_allocations\":" << r.jobs << ",\"assets_allocations\":" << r.assets
        << ",\"final_live_allocations\":0,\"final_backing_bytes\":0}\n";
}
std::size_t number(std::string_view text, std::size_t maximum)
{
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0 && value <= maximum, "invalid bounded benchmark parameter");
    return value;
}
}
int main(int argc, char** argv)
{
    try {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{argv[i]};
            if (arg == "--capture") { options.capture = true; continue; }
            require(i + 1 < argc, "missing benchmark argument value");
            const std::string_view value{argv[++i]};
            if (arg == "--rounds") { options.rounds = number(value, 100000); }
            else if (arg == "--warmup") { options.warmup = number(value, 10000); }
            else if (arg == "--batch") { options.batch = number(value, 256); }
            else if (arg == "--max-threads") { options.threads = number(value, 128); }
            else if (arg == "--allocation-budget") { options.budget = number(value, 1024 * 1024 * 1024); }
            else { throw std::runtime_error("unknown benchmark option"); }
        }
        DK_PROFILE_THREAD_NAME("dk-memory-bench-main");
        if (options.capture) {
            require(dk::profiling::enabled(), "capture requires profiling build");
            const auto deadline = Clock::now() + std::chrono::seconds{20};
            while (!dk::profiling::is_connected()) {
                require(Clock::now() < deadline, "capture connection timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
        }
        std::cout << std::setprecision(17);
        std::cout << "{\"kind\":\"metadata\",\"schema\":1,\"configuration\":\"" << DK_BENCH_CONFIGURATION
            << "\",\"profiling\":" << (dk::profiling::enabled() ? "true" : "false")
            << ",\"memory\":" << (dk::profiling::memory_enabled() ? "true" : "false")
            << ",\"connected\":" << (dk::profiling::is_connected() ? "true" : "false")
            << ",\"hardware_threads\":" << std::thread::hardware_concurrency()
            << ",\"rounds\":" << options.rounds << ",\"warmup\":" << options.warmup << ",\"batch\":" << options.batch
            << ",\"max_threads\":" << options.threads << ",\"allocation_budget\":" << options.budget
#ifdef _MSC_FULL_VER
            << ",\"msvc_full_ver\":" << _MSC_FULL_VER
#endif
            << "}\n";
        std::vector<std::size_t> thread_counts{1, 2, 4, options.threads};
        std::erase_if(thread_counts, [&](auto n) { return n > options.threads; });
        std::sort(thread_counts.begin(), thread_counts.end());
        thread_counts.erase(std::unique(thread_counts.begin(), thread_counts.end()), thread_counts.end());
        std::uint64_t jobs = 0, assets = 0; std::size_t cases = 0;
        auto record = [&](const Result& value) { print_result(value); jobs += value.jobs; assets += value.assets; ++cases; };
        for (const auto [bytes, alignment] : {std::pair{32U, 8U}, std::pair{256U, 64U}, std::pair{4096U, 256U}}) {
            for (const auto threads : thread_counts) {
                for (const auto strategy : {Strategy::heap, Strategy::pmr, Strategy::arena, Strategy::local_pool, Strategy::shared_pool}) {
                    record(run_case(strategy, false, bytes, alignment, threads, options));
                    if (threads > 1 && strategy != Strategy::arena && strategy != Strategy::local_pool) {
                        record(run_case(strategy, true, bytes, alignment, threads, options));
                    }
                }
            }
        }
        record(run_pipeline(options));
        require(!options.capture || dk::profiling::is_connected(), "capture disconnected during workload");
        std::cout << "{\"kind\":\"summary\",\"status\":\"passed\",\"case_count\":" << cases
            << ",\"backing_allocations\":" << jobs + assets << ",\"jobs_allocations\":" << jobs
            << ",\"assets_allocations\":" << assets << "}\n";
#if DK_ENABLE_PROFILING
        if (options.capture) {
            // Standalone measurement process only. Drain the pinned Tracy 0.14.1 client before Windows DLL teardown.
            // Every instrumented resource/zone is already gone; this is outside all measured intervals.
            tracy::GetProfiler().RequestShutdown();
            const auto deadline = Clock::now() + std::chrono::seconds{30};
            while (!tracy::GetProfiler().HasShutdownFinished()) {
                require(Clock::now() < deadline, "capture drain timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
        }
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
