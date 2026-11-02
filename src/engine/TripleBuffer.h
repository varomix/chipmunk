#pragma once

#include <array>
#include <atomic>

// Hands the latest value from one writer thread to one reader thread without locks or
// waiting. The reader may skip intermediate values, which is fine for "latest state" data.
template <typename T>
class TripleBuffer
{
public:
    T& writeBuffer() { return buffers[(size_t) back]; }

    void publish() { back = middle.exchange(back | dirtyFlag, std::memory_order_acq_rel) & indexMask; }

    // Returns true if a newer value is now available from readBuffer().
    bool fetch()
    {
        if ((middle.load(std::memory_order_acquire) & dirtyFlag) == 0)
            return false;

        front = middle.exchange(front, std::memory_order_acq_rel) & indexMask;
        return true;
    }

    const T& readBuffer() const { return buffers[(size_t) front]; }

private:
    static constexpr int indexMask = 3;
    static constexpr int dirtyFlag = 4;

    std::array<T, 3> buffers {};
    std::atomic<int> middle { 1 };
    int back = 0;
    int front = 2;
};
