// vgraph engine: connected components (weakly connected on directed graphs).
// Works on any graph type with the CsrGraph interface (see csr.h).
#ifndef VGRAPH_ENGINE_CC_H
#define VGRAPH_ENGINE_CC_H

#include "csr.h"

#include <vector>

namespace vgraph {

// component[p] = position of the smallest node id in p's component.
// Positions follow id order, so the smallest position is the smallest id.
// Union-find with path halving; the smaller position always becomes the root.
// Memory: 4 bytes per node.
template <class G>
void connected_components(const G &g, std::vector<pos_t> &component)
{
    const pos_t n = g.node_count();
    component.resize(n);
    for (pos_t p = 0; p < n; ++p) component[p] = p;

    auto root = [&component](pos_t p) {
        while (component[p] != p) {
            component[p] = component[component[p]];
            p = component[p];
        }
        return p;
    };
    for (pos_t u = 0; u < n; ++u) {
        g.for_out(u, [&](pos_t v, float) {
            pos_t a = root(u), b = root(v);
            if (a < b) component[b] = a;
            else if (b < a) component[a] = b;
        });
    }
    for (pos_t p = 0; p < n; ++p) component[p] = root(p);
}

} // namespace vgraph

#endif
