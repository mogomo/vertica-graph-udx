// vgraph engine: a small helper to run a loop over node blocks on several threads.
// The threads only touch engine data and are joined before the call returns.
#ifndef VGRAPH_ENGINE_PARALLEL_H
#define VGRAPH_ENGINE_PARALLEL_H

#include <atomic>
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

inline std::uint64_t block_count(std::uint64_t n) { return (n + PARALLEL_BLOCK - 1) / PARALLEL_BLOCK; }

// Calls fn(thread, block, begin, end) for every block of [0, n); thread is 0 .. threads-1 and
// tells the function which thread runs it (for per-thread buffers). Blocks are handed out one by
// one, so uneven blocks balance. Block borders do not depend on the number of threads: a sum that
// is kept per block and added in block order gives the same result with any number of threads.
// An exception in a thread is rethrown here.
template <class F>
void parallel_blocks_on(std::uint64_t n, int threads, F &&fn)
{
    const std::uint64_t blocks = block_count(n);
    auto work = [&](int thread, std::uint64_t b) {
        const std::uint64_t begin = b * PARALLEL_BLOCK;
        fn(thread, b, begin, begin + PARALLEL_BLOCK < n ? begin + PARALLEL_BLOCK : n);
    };
    if (threads <= 1 || blocks <= 1) {
        for (std::uint64_t b = 0; b < blocks; ++b) work(0, b);
        return;
    }
    if (static_cast<std::uint64_t>(threads) > blocks) threads = static_cast<int>(blocks);
    std::atomic<std::uint64_t> next(0);
    std::exception_ptr error;
    std::mutex error_lock;
    auto loop = [&](int thread) {
        try {
            for (std::uint64_t b = next.fetch_add(1); b < blocks; b = next.fetch_add(1)) work(thread, b);
        } catch (...) {
            std::lock_guard<std::mutex> hold(error_lock);
            if (!error) error = std::current_exception();
            next.store(blocks);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(loop, t);
    loop(0);
    for (std::thread &t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

// The same without the thread number: fn(block, begin, end).
template <class F>
void parallel_blocks(std::uint64_t n, int threads, F &&fn)
{
    parallel_blocks_on(n, threads, [&fn](int, std::uint64_t b, std::uint64_t begin, std::uint64_t end) { fn(b, begin, end); });
}

} // namespace vgraph

#endif
