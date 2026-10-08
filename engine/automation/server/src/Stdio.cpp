#include <dk/automation/JsonLines.hpp>
#include <atomic>
#include <deque>
#include <iostream>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace dk {
#ifdef _WIN32
namespace {
struct Line { std::string text; bool oversized = false; };
class Reader {
public:
    explicit Reader(std::shared_ptr<RuntimeEvents> events) : events_(std::move(events)) {
        done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done_) throw std::runtime_error("Cannot create stdin completion event");
        try { thread_ = std::jthread([this] { run(); }); }
        catch (...) { CloseHandle(done_); throw; }
    }
    ~Reader() {
        { std::lock_guard lock(mutex_); stop_ = true; }
        space_.notify_all();
        // Retrying cancellation closes the check-stop/enter-ReadFile race.
        while (WaitForSingleObject(done_, 0) != WAIT_OBJECT_0) {
            (void)CancelSynchronousIo(thread_.native_handle());
            (void)WaitForSingleObject(done_, 10);
        }
        thread_.join(); CloseHandle(done_);
    }
    struct Next { std::optional<Line> line; bool eof; };
    Next pop() {
        std::lock_guard lock(mutex_);
        if (fatal_) std::rethrow_exception(fatal_);
        if (lines_.empty()) return {{}, eof_};
        auto line = std::move(lines_.front()); lines_.pop_front(); space_.notify_one();
        return {std::move(line), false};
    }
private:
    std::shared_ptr<RuntimeEvents> events_;
    HANDLE done_ = nullptr;
    std::jthread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable space_;
    std::deque<Line> lines_;
    bool eof_ = false;
    std::exception_ptr fatal_;
    bool push(Line line) {
        { std::unique_lock lock(mutex_);
          space_.wait(lock, [&] { return stop_ || lines_.size() < 8; });
          if (stop_) return false;
          lines_.push_back(std::move(line)); }
        events_->notify(); return true;
    }
    void run() noexcept {
        try {
            const auto input = GetStdHandle(STD_INPUT_HANDLE);
            char buffer[4096]; Line line; bool received = false;
            while (!stop_) {
                DWORD count = 0;
                if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr)) {
                    const auto error = GetLastError();
                    if (stop_ || error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) break;
                    throw std::runtime_error("Input stream failure: " + std::to_string(error));
                }
                if (!count) break;
                for (DWORD i = 0; i < count && !stop_; ++i) {
                    if (buffer[i] == '\n') {
                        if (!push(std::move(line))) break;
                        line = {}; received = false;
                    } else {
                        received = true;
                        if (line.text.size() < 1024 * 1024) line.text += buffer[i];
                        else line.oversized = true;
                    }
                }
            }
            if (received && !stop_) (void)push(std::move(line));
            { std::lock_guard lock(mutex_); eof_ = true; }
        } catch (...) { std::lock_guard lock(mutex_); fatal_ = std::current_exception(); }
        events_->notify(); SetEvent(done_);
    }
};
}
#endif
int run_stdio(Runtime& runtime, std::ostream& output, std::ostream& diagnostics)
{
#ifdef _WIN32
    const auto events = runtime.events(); Reader reader(events);
    while (!runtime.stopping()) {
        const auto sequence = events->sequence();
        runtime.pump();
        auto next = reader.pop(); if (next.eof) break;
        if (!next.line) { events->wait(sequence, std::chrono::steady_clock::now() + std::chrono::seconds(60)); continue; }
        const auto& line = *next.line;
        if (!line.oversized && line.text.find_first_not_of(" \t\r") == std::string::npos) continue;
        const auto response = line.oversized ? RpcOutcome{rpc_error(nullptr, -32700, "JSON line exceeds 1 MiB"), true}
                                            : dispatch_json_line(runtime, line.text);
        if (response.response) {
            output << response.response->dump() << '\n'; output.flush();
            if (!output) { diagnostics << "Output stream failure\n"; return 3; }
        }
    }
    return 0;
#else
    return run_json_lines(runtime, std::cin, output, diagnostics, false, true);
#endif
}
}
