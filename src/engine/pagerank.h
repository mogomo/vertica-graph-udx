// vgraph engine: PageRank by power iteration.
// Works on any graph type with the CsrGraph interface (see csr.h).
#ifndef VGRAPH_ENGINE_PAGERANK_H
#define VGRAPH_ENGINE_PAGERANK_H

#include "csr.h"
#include "parallel.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace vgraph {

// Fixed number of iterations, no convergence test. Ranks sum to 1.
// A node without out edges spreads its rank evenly over all nodes.
// Edge weights are not used. Memory: 32 bytes per node.
// Gather form: a node collects the shares of the nodes that point to it, so every thread writes
// only the ranks of its own block. The result does not depend on the number of threads.
template <class G>
void pagerank(const G &g, int iterations, double damping, std::vector<double> &rank, int threads = 1)
{
    const pos_t n = g.node_count();
    rank.assign(n, n ? 1.0 / n : 0.0);
    if (n == 0) return;

    std::vector<double> next(n), share(n);
    std::vector<std::int64_t> degree(n, 0);
    std::vector<double> part(block_count(n));
    parallel_blocks(n, threads, [&](std::uint64_t, std::uint64_t begin, std::uint64_t end) {
        for (pos_t u = static_cast<pos_t>(begin); u < end; ++u)
            g.for_out(u, [&](pos_t, float) { ++degree[u]; });
    });

    for (int it = 0; it < iterations; ++it) {
        parallel_blocks(n, threads, [&](std::uint64_t b, std::uint64_t begin, std::uint64_t end) {
            double dangling = 0.0;
            for (pos_t u = static_cast<pos_t>(begin); u < end; ++u) {
                if (degree[u] == 0) { dangling += rank[u]; share[u] = 0.0; }
                else share[u] = damping * rank[u] / degree[u];
            }
            part[b] = dangling;
        });
        double dangling = 0.0;
        for (double d : part) dangling += d;
        const double base = (1.0 - damping) / n + damping * dangling / n;
        parallel_blocks(n, threads, [&](std::uint64_t, std::uint64_t begin, std::uint64_t end) {
            for (pos_t v = static_cast<pos_t>(begin); v < end; ++v) {
                double sum = base;
                g.for_in(v, [&](pos_t u, float) { sum += share[u]; });
                next[v] = sum;
            }
        });
        rank.swap(next);
    }
}

} // namespace vgraph

#endif
