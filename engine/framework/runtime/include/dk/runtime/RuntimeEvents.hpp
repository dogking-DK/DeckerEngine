#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace dk {
// Shared by input and worker producers. No Runtime or service references.
class RuntimeEvents final {
public:
    void notify() { std::lock_guard lock(mutex_); ++sequence_; changed_.notify_all(); }
    [[nodiscard]] std::uint64_t sequence() const { std::lock_guard lock(mutex_); return sequence_; }
    void wait(std::uint64_t previous, std::chrono::steady_clock::time_point deadline) const {
        std::unique_lock lock(mutex_); changed_.wait_until(lock, deadline, [&] { return sequence_ != previous; });
    }
private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::uint64_t sequence_ = 0;
};
}
