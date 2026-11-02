#pragma once

#include <array>
#include <atomic>
#include <cstdint>

// The most recent output samples. The audio thread writes and the editor reads, with no locks.
// A reader that falls behind sees newer samples in place of older ones, which only affects a picture.
class ScopeBuffer
{
public:
    static constexpr int capacity = 4096;

    void write(const float* samples, int count)
    {
        auto position = written.load(std::memory_order_relaxed);

        for (int i = 0; i < count; ++i)
            data[(size_t) ((position + i) % capacity)].store(samples[i], std::memory_order_relaxed);

        written.store(position + count, std::memory_order_release);
    }

    // Copies the newest "count" samples, oldest first.
    void read(float* destination, int count) const
    {
        const auto end = written.load(std::memory_order_acquire);

        for (int i = 0; i < count; ++i)
            destination[i] = data[(size_t) ((end - count + i + capacity) % capacity)].load(std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<float>, capacity> data {};
    std::atomic<int64_t> written { 0 };
};
