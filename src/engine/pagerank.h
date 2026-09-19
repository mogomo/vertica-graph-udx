// vgraph engine: PageRank by power iteration.
// Works on any graph type with the CsrGraph interface (see csr.h).
#ifndef VGRAPH_ENGINE_PAGERANK_H
#define VGRAPH_ENGINE_PAGERANK_H

#include "csr.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace vgraph {

// Fixed number of iterations, no convergence test. Ranks sum to 1.
// A node without out edges spreads its rank evenly over all nodes.
// Edge weights are not used. Memory: 24 bytes per node.
template <class G>
void pagerank(const G &g, int iterations, double damping, std::vector<double> &rank)
{
    const pos_t n = g.node_count();
    rank.assign(n, n ? 1.0 / n : 0.0);
    if (n == 0) return;

    std::vector<double> next(n);
    std::vector<std::int64_t> degree(n, 0);
    for (pos_t u = 0; u < n; ++u)
        g.for_out(u, [&](pos_t, float) { ++degree[u]; });

    for (int it = 0; it < iterations; ++it) {
        double dangling = 0.0;
        for (pos_t u = 0; u < n; ++u)
            if (degree[u] == 0) dangling += rank[u];
        const double base = (1.0 - damping) / n + damping * dangling / n;
        std::fill(next.begin(), next.end(), base);
        for (pos_t u = 0; u < n; ++u) {
            if (degree[u] == 0) continue;
            const double share = damping * rank[u] / degree[u];
            g.for_out(u, [&](pos_t v, float) { next[v] += share; });
        }
        rank.swap(next);
    }
}

} // namespace vgraph

#endif
