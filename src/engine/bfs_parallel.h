// vgraph engine: breadth-first search on several threads, for searches that visit a large part
// of the graph. Same result as khop() in bfs.h: every node once, at its smallest hop count.
//
// A level is computed in one of two ways (direction-optimizing BFS, Beamer et al. 2012):
//   top-down   the nodes of the frontier look at their neighbours and claim the unvisited ones
//              (an atomic test-and-set in the bitmap of visited nodes);
//   bottom-up  when the frontier is a large part of what is left, the unvisited nodes look for a
//              neighbour in the frontier instead, against the edge direction. A node stops being
//              interesting as soon as one is found, and no two threads write the same word.
// Small levels run on the calling thread: a search of a few thousand nodes never starts a thread.
#ifndef VGRAPH_ENGINE_BFS_PARALLEL_H
#define VGRAPH_ENGINE_BFS_PARALLEL_H

#include "bfs.h"
#include "parallel.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace vgraph {

class ParallelBfsScratch {
public:
    void prepare(pos_t node_count) {
        const std::size_t words = (static_cast<std::size_t>(node_count) + 63) / 64;
        if (words > words_) {
            seen_.reset(new std::atomic<std::uint64_t>[words]);
            for (std::size_t w = 0; w < words; ++w) seen_[w].store(0, std::memory_order_relaxed);
            front_.clear();
            next_.clear();               // allocated by the first bottom-up level
            words_ = words;
        }
    }
    std::unique_ptr<std::atomic<std::uint64_t>[]> seen_;
    std::vector<std::uint64_t> front_, next_;          // bottom-up: frontier and next frontier as bitmaps
    std::vector<pos_t> frontier, touched;
    std::vector<std::vector<pos_t>> found;             // per thread: the nodes of the level just computed
    std::size_t words_ = 0;
};

static_assert(PARALLEL_BLOCK % 64 == 0, "a block must own whole words of the bitmaps");

// on_level(hops, count, lists) is called on the calling thread, once per level that has nodes to
// report. lists is nullptr unless want_nodes; otherwise it holds the nodes of the level, spread
// over several vectors. max_results is not supported here (the caller uses khop() for it).
// Returns the number of nodes reported.
template <class G, class Level>
std::int64_t khop_parallel(const G &g, pos_t start, const KhopOptions &opt, ParallelBfsScratch &s,
                           int threads, bool want_nodes, Level &&on_level)
{
    const pos_t n = g.node_count();
    s.prepare(n);
    if (threads < 1) threads = 1;
    if (s.found.size() < static_cast<std::size_t>(threads)) s.found.resize(threads);
    const Direction back_dir = opt.direction == Direction::Out ? Direction::In
                             : opt.direction == Direction::In ? Direction::Out : Direction::Both;
    auto bit = [](pos_t p) { return std::uint64_t(1) << (p & 63); };

    std::int64_t reported = 0;
    auto report = [&](std::int64_t hops, std::int64_t count) {
        if (count == 0 || (opt.exact && hops != opt.depth)) return;
        on_level(hops, count, want_nodes ? &s.found : nullptr);
        reported += count;
    };

    for (auto &list : s.found) list.clear();
    s.seen_[start >> 6].store(bit(start), std::memory_order_relaxed);
    s.frontier.assign(1, start);
    s.found[0].push_back(start);
    report(0, 1);

    std::uint64_t visited = 1;
    const std::uint64_t SMALL = 1u << 16;          // up to here the visited bits are cleared one by one
    s.touched.assign(1, start);
    std::uint64_t frontier_size = 1;
    bool frontier_is_bitmap = false, used_bottom_up = false;

    for (std::int64_t hops = 1; hops <= opt.depth && frontier_size > 0; ++hops) {
        const std::uint64_t unvisited = n - visited;
        const bool bottom_up = frontier_size > SERIAL_LEVEL * 8 && frontier_size > unvisited / 12;
        for (auto &list : s.found) list.clear();
        std::uint64_t count = 0;

        if (bottom_up) {
            used_bottom_up = true;
            if (s.next_.size() < s.words_) { s.front_.assign(s.words_, 0); s.next_.assign(s.words_, 0); }
            if (!frontier_is_bitmap) {
                std::memset(s.front_.data(), 0, s.words_ * sizeof(std::uint64_t));
                for (pos_t p : s.frontier) s.front_[p >> 6] |= bit(p);
            }
            std::vector<std::uint64_t> per_block(block_count(n), 0);
            parallel_blocks_on(n, threads, [&](int thread, std::uint64_t b, std::uint64_t begin, std::uint64_t end) {
                std::vector<pos_t> &mine = s.found[thread];
                std::uint64_t block_found = 0;
                for (std::uint64_t w = begin >> 6; w << 6 < end; ++w) {
                    std::uint64_t seen = s.seen_[w].load(std::memory_order_relaxed);
                    std::uint64_t open = ~seen;
                    if ((w << 6) + 64 > n) open &= (std::uint64_t(1) << (n & 63)) - 1;      // past the last node
                    std::uint64_t fresh = 0;
                    while (open) {
                        const int k = __builtin_ctzll(open);
                        open &= open - 1;
                        const pos_t v = static_cast<pos_t>((w << 6) + k);
                        bool hit = false;
                        g.for_dir(v, back_dir, [&](pos_t u, float) {
                            if (!hit && (s.front_[u >> 6] & bit(u))) hit = true;
                        });
                        if (hit) {
                            fresh |= std::uint64_t(1) << k;
                            if (want_nodes) mine.push_back(v);
                        }
                    }
                    s.next_[w] = fresh;
                    if (fresh) {
                        s.seen_[w].store(seen | fresh, std::memory_order_relaxed);
                        block_found += __builtin_popcountll(fresh);
                    }
                }
                per_block[b] = block_found;
            });
            for (std::uint64_t c : per_block) count += c;
            s.front_.swap(s.next_);
            frontier_is_bitmap = true;
        } else {
            if (frontier_is_bitmap) {               // back to a list
                s.frontier.clear();
                for (std::size_t w = 0; w < s.words_; ++w)
                    for (std::uint64_t left = s.front_[w]; left; left &= left - 1)
                        s.frontier.push_back(static_cast<pos_t>((w << 6) + __builtin_ctzll(left)));
                frontier_is_bitmap = false;
            }
            auto expand = [&](pos_t u, std::vector<pos_t> &mine) {
                g.for_dir(u, opt.direction, [&](pos_t v, float) {
                    std::atomic<std::uint64_t> &word = s.seen_[v >> 6];
                    const std::uint64_t mask = bit(v);
                    if (word.load(std::memory_order_relaxed) & mask) return;
                    if (!(word.fetch_or(mask, std::memory_order_relaxed) & mask)) mine.push_back(v);
                });
            };
            if (threads == 1 || s.frontier.size() < SERIAL_LEVEL) {
                for (pos_t u : s.frontier) expand(u, s.found[0]);
            } else {
                const std::size_t chunk = 1024;
                std::atomic<std::size_t> cursor(0);
                std::atomic<int> thread_ids(0);
                auto loop = [&]() {
                    std::vector<pos_t> &mine = s.found[thread_ids.fetch_add(1)];
                    for (std::size_t from = cursor.fetch_add(chunk); from < s.frontier.size(); from = cursor.fetch_add(chunk)) {
                        const std::size_t to = from + chunk < s.frontier.size() ? from + chunk : s.frontier.size();
                        for (std::size_t i = from; i < to; ++i) expand(s.frontier[i], mine);
                    }
                };
                std::vector<std::thread> pool;
                for (int t = 1; t < threads; ++t) pool.emplace_back(loop);
                loop();
                for (std::thread &t : pool) t.join();
            }
            s.frontier.clear();
            for (const auto &list : s.found) {
                count += list.size();
                s.frontier.insert(s.frontier.end(), list.begin(), list.end());
            }
        }
        visited += count;
        if (visited <= SMALL && !used_bottom_up) s.touched.insert(s.touched.end(), s.frontier.begin(), s.frontier.end());
        frontier_size = count;
        report(hops, static_cast<std::int64_t>(count));
    }

    // all zero again for the next search
    if (visited <= SMALL && !used_bottom_up) {
        for (pos_t p : s.touched) s.seen_[p >> 6].store(0, std::memory_order_relaxed);
    } else {
        parallel_blocks(n, threads, [&](std::uint64_t, std::uint64_t begin, std::uint64_t end) {
            for (std::uint64_t w = begin >> 6; w << 6 < end; ++w) s.seen_[w].store(0, std::memory_order_relaxed);
        });
    }
    s.touched.clear();
    return reported;
}

} // namespace vgraph

#endif
