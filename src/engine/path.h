// vgraph engine: shortest path. BFS for hop count, Dijkstra for weights.
// Works on any graph type with the CsrGraph interface (see csr.h).
#ifndef VGRAPH_ENGINE_PATH_H
#define VGRAPH_ENGINE_PATH_H

#include "csr.h"

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

class PathScratch {
public:
    void prepare(pos_t node_count, bool weighted) {
        if (parent_.size() < node_count) parent_.resize(node_count, NO_POS);
        if (weighted && dist_.size() < node_count)
            dist_.resize(node_count, std::numeric_limits<double>::infinity());
    }
    void reset() {
        for (pos_t p : touched_) {
            parent_[p] = NO_POS;
            if (p < dist_.size()) dist_[p] = std::numeric_limits<double>::infinity();
        }
        touched_.clear();
    }
    std::vector<pos_t> parent_, touched_, frontier, next;
    std::vector<double> dist_;
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
    s.parent_[start] = start;
    s.touched_.push_back(start);

    if (!found && !opt.weighted) {
        s.frontier.assign(1, start);
        for (std::int64_t hops = 1; !found && !s.frontier.empty() &&
                                    (opt.max_depth == 0 || hops <= opt.max_depth); ++hops) {
            s.next.clear();
            for (pos_t u : s.frontier) {
                g.for_dir(u, opt.direction, [&](pos_t v, float) {
                    if (found || s.parent_[v] != NO_POS) return;
                    s.parent_[v] = u;
                    s.touched_.push_back(v);
                    if (v == target) found = true;
                    else s.next.push_back(v);
                });
                if (found) break;
            }
            s.frontier.swap(s.next);
        }
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
                if (nd < s.dist_[v]) {
                    if (s.parent_[v] == NO_POS) s.touched_.push_back(v);
                    s.dist_[v] = nd;
                    s.parent_[v] = u;
                    heap.push(Item(nd, v));
                }
            });
        }
    }

    if (found) {
        for (pos_t p = target; ; p = s.parent_[p]) {
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
