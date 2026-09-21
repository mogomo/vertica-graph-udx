// vgraph engine: shortest path. Bidirectional BFS for hop count (large levels on several threads),
// Dijkstra for weights.
// Works on any graph type with the CsrGraph interface (see csr.h).
#ifndef VGRAPH_ENGINE_PATH_H
#define VGRAPH_ENGINE_PATH_H

#include "csr.h"
#include "parallel.h"
#include "zero_array.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vgraph {

struct PathOptions {
    std::int64_t max_depth = 0;        // 0 = no limit. Not applied when weighted.
    Direction direction = Direction::Out;
    bool weighted = false;             // Dijkstra. Weights must be >= 0.
};

// One value per visited node. Most searches visit a tiny part of the graph, so the values start
// in a small hash table; writing (or even page-faulting) an array of N entries would cost more
// than the search. A search that grows large moves into an array.
// Dijkstra stores the node a node was reached from. The breadth-first search stores a stamp that
// tells the level in which the node was found (see shortest_path).
// Values are stored as value + 1, so that 0 means "not visited". A table slot is one word, key in
// the high half: claim() can then be called from several threads at once.
class LinkStore {
public:
    void prepare(pos_t node_count) {
        node_count_ = node_count;
        if (table_.size() == 0) table_.renew(1u << 12);
    }
    pos_t get(pos_t p) const {                        // NO_POS when not visited
        if (dense_mode_) return load(dense_[p]) - 1;
        for (std::size_t i = hash(p, table_.size()); ; i = (i + 1) & (table_.size() - 1)) {
            const std::uint64_t slot = load(table_[i]);
            if (slot == 0) return NO_POS;
            if (key_of(slot) == p) return value_of(slot) - 1;
        }
    }
    bool has(pos_t p) const { return get(p) != NO_POS; }
    void set(pos_t p, pos_t to) {
        reserve(1);
        if (dense_mode_) { dense_[p] = to + 1; return; }
        put(table_, p, to + 1, used_);
    }

    // Room for `extra` more nodes, so that claim() never has to grow the table.
    void reserve(std::size_t extra) {
        if (dense_mode_) return;
        std::size_t want = table_.size();
        while ((used_ + extra) * 2 > want) want *= 2;
        if (want == table_.size()) return;
        if (want > static_cast<std::size_t>(node_count_) / 8 + (1u << 12)) {     // large search: use an array
            dense_.ensure(node_count_);
            for (std::size_t i = 0; i < table_.size(); ++i)
                if (table_[i]) dense_[key_of(table_[i])] = value_of(table_[i]);
            dense_mode_ = true;
            return;
        }
        ZeroArray<std::uint64_t> bigger;
        bigger.renew(want);
        std::size_t used = 0;
        for (std::size_t i = 0; i < table_.size(); ++i)
            if (table_[i]) put(bigger, key_of(table_[i]), value_of(table_[i]), used);
        table_.swap(bigger);
    }

    // Marks p with `stamp` unless it carries a stamp below `first` (found in an earlier level) or a
    // stamp <= `stamp` already. Of several threads that claim p in one level the smallest stamp
    // stays. Returns true when this call wrote the stamp; adds 1 to fresh when p was new.
    // Safe to call from several threads after reserve(); get() may run at the same time.
    bool claim(pos_t p, pos_t stamp, pos_t first, std::size_t &fresh) {
        const pos_t value = stamp + 1;
        if (dense_mode_) {
            pos_t seen = load(dense_[p]);
            while (seen == 0 || (seen > first && seen > value))
                if (exchange(dense_[p], seen, value)) return true;
            return false;
        }
        const std::uint64_t mine = pack(p, value);
        for (std::size_t i = hash(p, table_.size()); ; i = (i + 1) & (table_.size() - 1)) {
            std::uint64_t slot = load(table_[i]);
            if (slot == 0) {
                if (exchange(table_[i], slot, mine)) { ++fresh; return true; }
            }                                         // lost the slot: slot now holds the winner
            if (key_of(slot) != p) continue;
            while (value_of(slot) > first && value_of(slot) > value)
                if (exchange(table_[i], slot, mine)) return true;
            return false;
        }
    }
    void add_used(std::size_t fresh) { used_ += fresh; }

    void reset() {
        if (dense_mode_) { dense_.clear(); dense_mode_ = false; }
        if (used_ * 8 > table_.size() || table_.size() > (1u << 16)) table_.renew(1u << 12);
        else for (std::size_t i = 0; i < table_.size(); ++i) table_[i] = 0;
        used_ = 0;
    }

private:
    static std::uint64_t pack(pos_t key, pos_t value) { return (static_cast<std::uint64_t>(key) << 32) | value; }
    static pos_t key_of(std::uint64_t slot) { return static_cast<pos_t>(slot >> 32); }
    static pos_t value_of(std::uint64_t slot) { return static_cast<pos_t>(slot); }
    static std::size_t hash(pos_t p, std::size_t size) { return (p * 0x9E3779B1u) & (size - 1); }
    // compiler builtins, not CPU intrinsics: the same source on every platform
    template <class T> static T load(const T &at) { return __atomic_load_n(&at, __ATOMIC_RELAXED); }
    template <class T> static bool exchange(T &at, T &expected, T desired) {
        return __atomic_compare_exchange_n(&at, &expected, desired, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }
    static void put(ZeroArray<std::uint64_t> &table, pos_t key, pos_t value, std::size_t &used) {
        for (std::size_t i = hash(key, table.size()); ; i = (i + 1) & (table.size() - 1)) {
            if (table[i] == 0) { table[i] = pack(key, value); ++used; return; }
            if (key_of(table[i]) == key) { table[i] = pack(key, value); return; }
        }
    }
    ZeroArray<std::uint64_t> table_;
    ZeroArray<pos_t> dense_;
    std::size_t used_ = 0;
    pos_t node_count_ = 0;
    bool dense_mode_ = false;
};

class PathScratch {
public:
    void prepare(pos_t node_count, bool weighted) {
        parent_.prepare(node_count);
        back_.prepare(node_count);
        if (weighted) dist_.ensure(node_count);      // valid only where parent_ is set
    }
    void reset() {
        parent_.reset();
        back_.reset();
        if (touched_.size() > dist_.size() / 16) dist_.clear();
        else for (pos_t p : touched_) if (p < dist_.size()) dist_[p] = 0.0;
        touched_.clear();
    }
    LinkStore parent_, back_;             // the search from the start, the search from the target
    std::vector<pos_t> touched_, frontier, next, frontier_b;
    std::vector<pos_t> levels_, levels_b_;           // per side: the first stamp of every level
    std::vector<std::vector<pos_t>> found_;          // per range of the frontier: the nodes it found
    ZeroArray<double> dist_;
};

#ifndef VGRAPH_PATH_RANGE
#define VGRAPH_PATH_RANGE 1024                         // the unit tests use a tiny one
#endif
constexpr std::size_t PATH_RANGE = VGRAPH_PATH_RANGE; // frontier nodes that share a stamp

// Fills path with the positions from start to target, both included.
// Returns false and leaves path empty when there is no path.
// Among equally short paths the result is the same for a given graph, with any number of threads.
// threads is used by the search by hops only; Dijkstra runs on the calling thread.
template <class G>
bool shortest_path(const G &g, pos_t start, pos_t target, const PathOptions &opt,
                   PathScratch &s, std::vector<pos_t> &path, int threads = 1)
{
    path.clear();
    s.prepare(g.node_count(), opt.weighted);
    bool found = (start == target);

    if (!found && !opt.weighted) {
        // Two searches, one level at a time: forward from start, backward from target against the
        // edge direction. The side with the smaller frontier moves: a search from one side alone
        // visits most of a large graph for a far target, the two half searches visit a fraction.
        //
        // The search stops at the first meeting. Every node is tested against the other side when
        // it is discovered, so a node of the other side that is met now lies in its newest level,
        // db hops from its end (forward case): a node v at k < db hops was expanded earlier, and
        // that expansion looked at u. Either u was already visited from this side, a meeting then,
        // or u joined the other side, a meeting when this side discovered u. So all meetings of a
        // level have the same length, df + 1 + db, and the first one is a shortest path.
        //
        // Several threads. The frontier is cut into ranges of PATH_RANGE nodes, and every range of
        // every level gets the next stamp of its side. A node keeps the smallest stamp that claims
        // it: that is the range that finds it first when the ranges run one after the other. So the
        // next frontier (the ranges in order, each with the nodes that kept its stamp) and the
        // first meeting (the one in the earliest range) do not depend on the threads. No parent is
        // stored: the stamp tells the level of a node, and the path is read back at the end through
        // the smallest neighbour that lies one level nearer.
        const Direction back_dir = opt.direction == Direction::Out ? Direction::In
                                 : opt.direction == Direction::In ? Direction::Out : Direction::Both;
        std::size_t fresh = 0;
        s.parent_.claim(start, 0, 0, fresh);
        s.parent_.add_used(fresh);
        fresh = 0;
        s.back_.claim(target, 0, 0, fresh);
        s.back_.add_used(fresh);
        s.levels_.assign(1, 0);
        s.levels_b_.assign(1, 0);
        pos_t stamps_f = 1, stamps_b = 1;            // the next free stamp of each side
        s.frontier.assign(1, start);
        s.frontier_b.assign(1, target);
        std::int64_t df = 0, db = 0, best = -1;
        pos_t meet_u = NO_POS, meet_v = NO_POS;      // edge meet_u -> meet_v joins the two searches
        if (threads < 1) threads = 1;

        while (best < 0 && !s.frontier.empty() && !s.frontier_b.empty() &&
               (opt.max_depth == 0 || df + db < opt.max_depth)) {
            const bool forward = s.frontier.size() <= s.frontier_b.size();
            LinkStore &mine = forward ? s.parent_ : s.back_;
            const LinkStore &other = forward ? s.back_ : s.parent_;
            std::vector<pos_t> &front = forward ? s.frontier : s.frontier_b;
            const Direction dir = forward ? opt.direction : back_dir;
            pos_t &stamps = forward ? stamps_f : stamps_b;
            const pos_t first = stamps;              // stamps below it: found in an earlier level
            const std::size_t ranges = (front.size() + PATH_RANGE - 1) / PATH_RANGE;
            if (ranges >= NO_POS - 1 - first) throw std::runtime_error("search too large");
            (forward ? s.levels_ : s.levels_b_).push_back(first);
            stamps += static_cast<pos_t>(ranges);
            const bool parallel = threads > 1 && front.size() >= SERIAL_LEVEL;

            // One range of the frontier. Returns true at a meeting, with the edge in a and b.
            auto expand = [&](std::size_t r, std::size_t begin, std::size_t end, std::vector<pos_t> &list,
                              std::size_t &new_nodes, const std::atomic<std::size_t> *met_range,
                              pos_t &a, pos_t &b) {
                const pos_t stamp = first + static_cast<pos_t>(r);
                bool met = false;
                for (std::size_t i = begin; i < end && !met; ++i) {
                    if (met_range && met_range->load(std::memory_order_relaxed) < r) return false;
                    const pos_t u = front[i];
                    g.for_dir(u, dir, [&](pos_t v, float) {
                        if (met) return;
                        if (other.has(v)) { met = true; a = u; b = v; return; }
                        if (!met_range) mine.reserve(new_nodes + 1);     // one thread: the table may grow
                        if (mine.claim(v, stamp, first, new_nodes)) list.push_back(v);
                    });
                }
                return met;
            };

            pos_t a = NO_POS, b = NO_POS;
            bool met = false;
            s.next.clear();
            if (!parallel) {
                for (std::size_t r = 0; r < ranges && !met; ++r) {
                    std::size_t new_nodes = 0;
                    const std::size_t begin = r * PATH_RANGE;
                    met = expand(r, begin, std::min(begin + PATH_RANGE, front.size()), s.next, new_nodes, nullptr, a, b);
                    mine.add_used(new_nodes);
                }
            } else {
                // room for every node this level can find: the table must not grow under the threads
                std::vector<std::size_t> count(ranges, 0);
                parallel_ranges_on(front.size(), PATH_RANGE, threads, [&](int, std::uint64_t r, std::uint64_t begin, std::uint64_t end) {
                    std::size_t edges = 0;
                    for (std::uint64_t i = begin; i < end; ++i) g.for_dir(front[i], dir, [&](pos_t, float) { ++edges; });
                    count[r] = edges;
                });
                std::size_t edges = 0;
                for (std::size_t c : count) edges += c;
                mine.reserve(edges);

                if (s.found_.size() < ranges) s.found_.resize(ranges);
                std::atomic<std::size_t> met_range(ranges);          // the earliest range with a meeting
                std::mutex met_lock;
                parallel_ranges_on(front.size(), PATH_RANGE, threads, [&](int, std::uint64_t r, std::uint64_t begin, std::uint64_t end) {
                    std::vector<pos_t> &list = s.found_[r];
                    list.clear();
                    count[r] = 0;
                    if (met_range.load(std::memory_order_relaxed) < r) return;
                    pos_t ra = NO_POS, rb = NO_POS;
                    std::size_t new_nodes = 0;
                    const bool range_met = expand(r, begin, end, list, new_nodes, &met_range, ra, rb);
                    count[r] = new_nodes;
                    if (!range_met) return;
                    std::lock_guard<std::mutex> hold(met_lock);
                    if (r < met_range.load(std::memory_order_relaxed)) { met_range.store(r); a = ra; b = rb; }
                });
                met = met_range.load() < ranges;
                for (std::size_t c : count) mine.add_used(c);
                if (!met) {
                    // a node stays in the range whose stamp it kept
                    parallel_ranges_on(ranges, 1, threads, [&](int, std::uint64_t r, std::uint64_t, std::uint64_t) {
                        std::vector<pos_t> &list = s.found_[r];
                        const pos_t stamp = first + static_cast<pos_t>(r);
                        std::size_t kept = 0;
                        for (pos_t v : list) if (mine.get(v) == stamp) list[kept++] = v;
                        list.resize(kept);
                    });
                    for (std::size_t r = 0; r < ranges; ++r) s.next.insert(s.next.end(), s.found_[r].begin(), s.found_[r].end());
                }
            }
            if (met) {
                best = df + db + 1;
                meet_u = forward ? a : b;
                meet_v = forward ? b : a;
            }
            front.swap(s.next);
            if (forward) ++df; else ++db;
        }
        if (best >= 0) {
            // from p to the end of its side: always the smallest neighbour one level nearer
            auto walk = [&](const LinkStore &link, const std::vector<pos_t> &levels, pos_t p, Direction nearer) {
                auto level_of = [&](pos_t stamp) {
                    return static_cast<std::int64_t>(std::upper_bound(levels.begin(), levels.end(), stamp) - levels.begin()) - 1;
                };
                path.push_back(p);
                for (std::int64_t level = level_of(link.get(p)); level > 0; --level) {
                    pos_t pick = NO_POS;
                    g.for_dir(p, nearer, [&](pos_t q, float) {
                        if (q >= pick) return;
                        const pos_t stamp = link.get(q);
                        if (stamp != NO_POS && level_of(stamp) == level - 1) pick = q;
                    });
                    if (pick == NO_POS) throw std::logic_error("shortest_path: a level has no link to the level before");
                    path.push_back(p = pick);
                }
            };
            walk(s.parent_, s.levels_, meet_u, back_dir);
            std::reverse(path.begin(), path.end());
            walk(s.back_, s.levels_b_, meet_v, opt.direction);
        }
        s.reset();
        return best >= 0;
    } else if (!found) {
        using Item = std::pair<double, pos_t>;
        s.parent_.set(start, start);                 // parent of start is start itself: marks it visited
        s.touched_.push_back(start);
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
        s.dist_[start] = 0.0;
        heap.push(Item(0.0, start));
        while (!heap.empty()) {
            const Item top = heap.top();
            heap.pop();
            const pos_t u = top.second;
            if (top.first > s.dist_[u]) continue;
            if (u == target) { found = true; break; }
            g.for_dir(u, opt.direction, [&](pos_t v, float w) {
                if (w < 0.0f) throw std::runtime_error("negative edge weight");
                const double nd = top.first + w;
                const bool first = !s.parent_.has(v);
                if (first || nd < s.dist_[v]) {
                    if (first) s.touched_.push_back(v);
                    s.dist_[v] = nd;
                    s.parent_.set(v, u);
                    heap.push(Item(nd, v));
                }
            });
        }
    }

    if (found) {
        for (pos_t p = target; ; p = s.parent_.get(p)) {
            path.push_back(p);
            if (p == start) break;
        }
        std::reverse(path.begin(), path.end());
    }
    s.reset();
    return found;
}

} // namespace vgraph

#endif
