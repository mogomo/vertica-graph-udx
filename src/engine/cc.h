// vgraph engine: connected components (weakly connected on directed graphs).
// Works on any graph type with the CsrGraph interface (see csr.h).
#ifndef VGRAPH_ENGINE_CC_H
#define VGRAPH_ENGINE_CC_H

#include "csr.h"
#include "parallel.h"

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace vgraph {

// component[p] = position of the node with the smallest id in p's component.
// Union-find with path halving; the smaller position always becomes the root, so links only
// ever point to smaller positions and cannot form a cycle. Links and halving use compare and
// swap, so several threads can work on it at the same time; the result does not depend on them.
// In a snapshot positions follow id order. Nodes added by a delta overlay do
// not, so the smallest id of every component is found in a second pass.
// Memory: 12 bytes per node.
template <class G>
void connected_components(const G &g, std::vector<pos_t> &component, int threads = 1)
{
    const pos_t n = g.node_count();
    std::unique_ptr<std::atomic<pos_t>[]> parent(new std::atomic<pos_t>[n]);
    parallel_blocks(n, threads, [&](std::uint64_t, std::uint64_t begin, std::uint64_t end) {
        for (pos_t p = static_cast<pos_t>(begin); p < end; ++p) parent[p].store(p, std::memory_order_relaxed);
    });

    auto root = [&parent](pos_t p) {
        for (;;) {
            pos_t up = parent[p].load(std::memory_order_relaxed);
            if (up == p) return p;
            const pos_t above = parent[up].load(std::memory_order_relaxed);
            parent[p].compare_exchange_weak(up, above, std::memory_order_relaxed);     // halving; losing the race is fine
            p = above;
        }
    };
    parallel_blocks(n, threads, [&](std::uint64_t, std::uint64_t begin, std::uint64_t end) {
        for (pos_t u = static_cast<pos_t>(begin); u < end; ++u) {
            g.for_out(u, [&](pos_t v, float) {
                for (;;) {
                    pos_t a = root(u), b = root(v);
                    if (a == b) return;
                    if (a > b) std::swap(a, b);
                    pos_t expect = b;                  // b must still be a root
                    if (parent[b].compare_exchange_strong(expect, a, std::memory_order_relaxed)) return;
                }
            });
        }
    });

    component.resize(n);
    std::vector<pos_t> smallest(n);
    for (pos_t p = 0; p < n; ++p) smallest[p] = p;
    for (pos_t p = 0; p < n; ++p) {
        const pos_t r = root(p);
        component[p] = r;
        if (g.id_of(p) < g.id_of(smallest[r])) smallest[r] = p;
    }
    parallel_blocks(n, threads, [&](std::uint64_t, std::uint64_t begin, std::uint64_t end) {
        for (pos_t p = static_cast<pos_t>(begin); p < end; ++p) component[p] = smallest[component[p]];
    });
}

} // namespace vgraph

#endif
