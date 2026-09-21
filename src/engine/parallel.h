// vgraph engine: a small helper to run a loop over node blocks on several threads.
// The threads only touch engine data and are joined before the call returns.
#ifndef VGRAPH_ENGINE_PARALLEL_H
#define VGRAPH_ENGINE_PARALLEL_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace vgraph {

#ifndef VGRAPH_PARALLEL_BLOCK
#define VGRAPH_PARALLEL_BLOCK (1u << 16)     // nodes per block; the unit tests use a tiny one
#endif
constexpr std::uint64_t PARALLEL_BLOCK = VGRAPH_PARALLEL_BLOCK;

#ifndef VGRAPH_SERIAL_LEVEL
#define VGRAPH_SERIAL_LEVEL 4096             // the unit tests use a tiny one
#endif
constexpr std::size_t SERIAL_LEVEL = VGRAPH_SERIAL_LEVEL;      // searches: smaller frontiers are expanded on the calling thread

inline std::uint64_t block_count(std::uint64_t n) { return (n + PARALLEL_BLOCK - 1) / PARALLEL_BLOCK; }

// Calls fn(thread, range, begin, end) for every range of `size` elements of [0, n); thread is
// 0 .. threads-1 and tells the function which thread runs it (for per-thread buffers). Ranges are
// handed out one by one in rising order, so uneven ranges balance, and when a range runs, every
// range before it has been handed out. Range borders do not depend on the number of threads.
// An exception in a thread is rethrown here.
template <class F>
void parallel_ranges_on(std::uint64_t n, std::uint64_t size, int threads, F &&fn)
{
    const std::uint64_t ranges = (n + size - 1) / size;
    auto work = [&](int thread, std::uint64_t r) {
        const std::uint64_t begin = r * size;
        fn(thread, r, begin, begin + size < n ? begin + size : n);
    };
    if (threads <= 1 || ranges <= 1) {
        for (std::uint64_t r = 0; r < ranges; ++r) work(0, r);
        return;
    }
    if (static_cast<std::uint64_t>(threads) > ranges) threads = static_cast<int>(ranges);
    std::atomic<std::uint64_t> next(0);
    std::exception_ptr error;
    std::mutex error_lock;
    auto loop = [&](int thread) {
        try {
            for (std::uint64_t r = next.fetch_add(1); r < ranges; r = next.fetch_add(1)) work(thread, r);
        } catch (...) {
            std::lock_guard<std::mutex> hold(error_lock);
            if (!error) error = std::current_exception();
            next.store(ranges);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(loop, t);
    loop(0);
    for (std::thread &t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

// The same for the node blocks of [0, n): fn(thread, block, begin, end). A sum that is kept per
// block and added in block order gives the same result with any number of threads.
template <class F>
void parallel_blocks_on(std::uint64_t n, int threads, F &&fn)
{
    parallel_ranges_on(n, PARALLEL_BLOCK, threads, fn);
}

// The same without the thread number: fn(block, begin, end).
template <class F>
void parallel_blocks(std::uint64_t n, int threads, F &&fn)
{
    parallel_blocks_on(n, threads, [&fn](int, std::uint64_t b, std::uint64_t begin, std::uint64_t end) { fn(b, begin, end); });
}

} // namespace vgraph

#endif
