// vgraph engine: shortest path. Bidirectional BFS for hop count, Dijkstra for weights.
// Works on any graph type with the CsrGraph interface (see csr.h).
#ifndef VGRAPH_ENGINE_PATH_H
#define VGRAPH_ENGINE_PATH_H

#include "csr.h"
#include "zero_array.h"

#include <algorithm>
#include <cstdint>
#include <limits>
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

// Where a search came from, per visited node. Most searches visit a tiny part of the graph, so
// the links start in a small hash table; writing (or even page-faulting) an array of N entries
// would cost more than the search. A search that grows large moves into an array.
// Values are stored as position + 1, so that 0 means "not visited".
class LinkStore {
public:
    void prepare(pos_t node_count) {
        node_count_ = node_count;
        if (table_.empty()) table_.assign(1u << 12, Slot());
    }
    pos_t get(pos_t p) const {                        // NO_POS when not visited
        if (dense_mode_) return dense_[p] - 1;
        for (std::size_t i = hash(p); ; i = (i + 1) & (table_.size() - 1)) {
            if (table_[i].value == 0) return NO_POS;
            if (table_[i].key == p) return table_[i].value - 1;
        }
    }
    bool has(pos_t p) const { return get(p) != NO_POS; }
    void set(pos_t p, pos_t to) {
        if (dense_mode_) { dense_[p] = to + 1; return; }
        if ((used_ + 1) * 2 > table_.size()) grow();
        if (dense_mode_) { dense_[p] = to + 1; return; }
        put(table_, p, to + 1, used_);
    }
    void reset() {
        if (dense_mode_) { dense_.clear(); dense_mode_ = false; }
        if (used_ * 8 > table_.size() || table_.size() > (1u << 16)) table_.assign(1u << 12, Slot());
        else for (Slot &slot : table_) slot = Slot();
        used_ = 0;
    }

private:
    struct Slot { pos_t key = 0, value = 0; };
    std::size_t hash(pos_t p) const { return (p * 0x9E3779B1u) & (table_.size() - 1); }
    static void put(std::vector<Slot> &table, pos_t key, pos_t value, std::size_t &used) {
        for (std::size_t i = (key * 0x9E3779B1u) & (table.size() - 1); ; i = (i + 1) & (table.size() - 1)) {
            if (table[i].value == 0) { table[i].key = key; table[i].value = value; ++used; return; }
            if (table[i].key == key) { table[i].value = value; return; }
        }
    }
    void grow() {
        if (table_.size() * 2 > static_cast<std::size_t>(node_count_) / 8 + (1u << 12)) {     // large search: use an array
            dense_.ensure(node_count_);
            for (const Slot &slot : table_) if (slot.value) dense_[slot.key] = slot.value;
            dense_mode_ = true;
            return;
        }
        std::vector<Slot> bigger(table_.size() * 2);
        std::size_t used = 0;
        for (const Slot &slot : table_) if (slot.value) put(bigger, slot.key, slot.value, used);
        table_.swap(bigger);
    }
    std::vector<Slot> table_;
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
    LinkStore parent_, back_;             // back_: next node towards the target
    std::vector<pos_t> touched_, frontier, next, frontier_b;
    ZeroArray<double> dist_;
};

// Fills path with the positions from start to target, both included.
// Returns false and leaves path empty when there is no path.
// Among equally short paths the result is deterministic for a given snapshot.
template <class G>
bool shortest_path(const G &g, pos_t start, pos_t target, const PathOptions &opt,
                   PathScratch &s, std::vector<pos_t> &path)
{
    path.clear();
    s.prepare(g.node_count(), opt.weighted);
    bool found = (start == target);

    // parent of start is start itself: marks it visited.
    s.parent_.set(start, start);
    s.touched_.push_back(start);

    if (!found && !opt.weighted) {
        // Two searches, one level at a time: forward from start, backward from target against the
        // edge direction. The side with the smaller frontier moves. A level is always finished, and
        // the shortest of the meetings found in it is the answer: a search from one side alone
        // visits most of a large graph for a far target, the two half searches visit a fraction.
        const Direction back_dir = opt.direction == Direction::Out ? Direction::In
                                 : opt.direction == Direction::In ? Direction::Out : Direction::Both;
        s.back_.set(target, target);
        s.touched_.push_back(target);
        s.frontier.assign(1, start);
        s.frontier_b.assign(1, target);
        std::int64_t df = 0, db = 0, best = -1;
        pos_t meet_u = NO_POS, meet_v = NO_POS;      // edge meet_u -> meet_v joins the two searches
        auto chain = [](const LinkStore &link, pos_t p) {
            std::int64_t n = 0;
            while (link.get(p) != p) { p = link.get(p); ++n; }
            return n;
        };
        while (best < 0 && !s.frontier.empty() && !s.frontier_b.empty() &&
               (opt.max_depth == 0 || df + db < opt.max_depth)) {
            const bool forward = s.frontier.size() <= s.frontier_b.size();
            LinkStore &mine = forward ? s.parent_ : s.back_;
            const LinkStore &other = forward ? s.back_ : s.parent_;
            std::vector<pos_t> &front = forward ? s.frontier : s.frontier_b;
            s.next.clear();
            for (pos_t u : front) {
                g.for_dir(u, forward ? opt.direction : back_dir, [&](pos_t v, float) {
                    if (other.has(v)) {
                        const std::int64_t len = (forward ? df : db) + 1 + chain(other, v);
                        if (best < 0 || len < best) {
                            best = len;
                            meet_u = forward ? u : v;
                            meet_v = forward ? v : u;
                        }
                    } else if (!mine.has(v)) {
                        mine.set(v, u);
                        s.touched_.push_back(v);
                        s.next.push_back(v);
                    }
                });
            }
            front.swap(s.next);
            if (forward) ++df; else ++db;
        }
        if (best >= 0) {
            for (pos_t p = meet_u; ; p = s.parent_.get(p)) {
                path.push_back(p);
                if (p == start) break;
            }
            std::reverse(path.begin(), path.end());
            for (pos_t p = meet_v; ; p = s.back_.get(p)) {
                path.push_back(p);
                if (p == target) break;
            }
        }
        s.reset();
        return best >= 0;
    } else if (!found) {
        using Item = std::pair<double, pos_t>;
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
