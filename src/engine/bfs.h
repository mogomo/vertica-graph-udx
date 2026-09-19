// vgraph engine: k-hop neighbourhood by breadth-first search.
// Works on any graph type with the CsrGraph interface (see csr.h).
#ifndef VGRAPH_ENGINE_BFS_H
#define VGRAPH_ENGINE_BFS_H

#include "csr.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace vgraph {

struct KhopOptions {
    std::int64_t depth = 1;            // largest hop count to report
    bool exact = false;                // true: only nodes at exactly `depth` hops
    Direction direction = Direction::Out;
    std::int64_t max_results = 0;      // 0 = no limit
};

// Working memory, reused between searches so that a small search on a large
// graph does not pay for clearing N entries.
class BfsScratch {
public:
    void prepare(pos_t node_count) {
        if (seen_.size() < node_count) seen_.resize(node_count, 0);
    }
    bool mark(pos_t p) {
        if (seen_[p]) return false;
        seen_[p] = 1;
        touched_.push_back(p);
        return true;
    }
    void reset() {
        if (touched_.size() > seen_.size() / 16) std::fill(seen_.begin(), seen_.end(), 0);
        else for (pos_t p : touched_) seen_[p] = 0;
        touched_.clear();
    }
    std::vector<pos_t> frontier, next;
private:
    std::vector<std::uint8_t> seen_;
    std::vector<pos_t> touched_;
};

// Calls emit(pos, hops) for every node within `depth` hops of start, each node
// once, at its smallest hop count. hops 0 is the start node itself.
// Returns the number of nodes emitted.
template <class G, class Emit>
std::int64_t khop(const G &g, pos_t start, const KhopOptions &opt, BfsScratch &s, Emit &&emit)
{
    s.prepare(g.node_count());
    std::int64_t emitted = 0;
    auto out = [&](pos_t p, std::int64_t hops) {
        if (opt.exact && hops != opt.depth) return true;
        emit(p, hops);
        ++emitted;
        return opt.max_results == 0 || emitted < opt.max_results;
    };

    s.frontier.clear();
    s.frontier.push_back(start);
    s.mark(start);
    bool more = out(start, 0);

    for (std::int64_t hops = 1; more && hops <= opt.depth && !s.frontier.empty(); ++hops) {
        s.next.clear();
        for (pos_t u : s.frontier) {
            g.for_dir(u, opt.direction, [&](pos_t v, float) {
                if (more && s.mark(v)) {
                    s.next.push_back(v);
                    more = out(v, hops);
                }
            });
            if (!more) break;
        }
        s.frontier.swap(s.next);
    }
    s.reset();
    return emitted;
}

} // namespace vgraph

#endif
