// vgraph engine: CSR graph view.
// Non-owning. The arrays live in a snapshot buffer or in an mmap'ed file.
#ifndef VGRAPH_ENGINE_CSR_H
#define VGRAPH_ENGINE_CSR_H

#include <algorithm>
#include <cstdint>

namespace vgraph {

enum class Direction { Out, In, Both };

// Position of a node inside the snapshot: index into ids[]. N fits uint32.
using pos_t = std::uint32_t;
constexpr pos_t NO_POS = 0xFFFFFFFFu;

struct Csr {
    std::uint64_t node_count = 0;
    std::uint64_t edge_count = 0;
    std::int64_t max_ver = 0;
    bool directed = true;           // false: one row per undirected edge; a delta row changes both directions
    bool in_is_out = false;         // in lists equal out lists and are not stored: in_* point to out_*
    bool weighted = false;
    const std::int64_t *ids = nullptr;          // [N] sorted node ids
    const std::int64_t *out_offsets = nullptr;  // [N+1]
    const pos_t *out_nbrs = nullptr;            // [E] sorted inside each list
    const std::int64_t *in_offsets = nullptr;   // [N+1]
    const pos_t *in_nbrs = nullptr;             // [E]
    const float *out_weights = nullptr;         // [E] or null
    const float *in_weights = nullptr;          // [E] or null
};

// Traversal interface used by the algorithms (bfs, path, cc, pagerank).
// The delta overlay offers the same interface.
class CsrGraph {
public:
    explicit CsrGraph(const Csr &c) : c_(c) {}

    pos_t node_count() const { return static_cast<pos_t>(c_.node_count); }
    std::int64_t id_of(pos_t p) const { return c_.ids[p]; }

    bool find(std::int64_t id, pos_t &p) const {
        const std::int64_t *end = c_.ids + c_.node_count;
        const std::int64_t *it = std::lower_bound(c_.ids, end, id);
        if (it == end || *it != id) return false;
        p = static_cast<pos_t>(it - c_.ids);
        return true;
    }

    // f(pos_t neighbour, float weight). Weight is 1 on unweighted graphs.
    template <class F> void for_out(pos_t p, F &&f) const {
        walk(c_.out_offsets, c_.out_nbrs, c_.out_weights, p, f);
    }
    template <class F> void for_in(pos_t p, F &&f) const {
        walk(c_.in_offsets, c_.in_nbrs, c_.in_weights, p, f);
    }
    template <class F> void for_dir(pos_t p, Direction d, F &&f) const {
        if (d != Direction::In) for_out(p, f);
        // If the in list equals the out list, one pass covers both directions.
        if (d == Direction::In || (d == Direction::Both && !c_.in_is_out)) for_in(p, f);
    }

    std::int64_t out_degree(pos_t p) const { return c_.out_offsets[p + 1] - c_.out_offsets[p]; }

    const Csr &csr() const { return c_; }

private:
    template <class F>
    static void walk(const std::int64_t *off, const pos_t *nbr, const float *w, pos_t p, F &f) {
        for (std::int64_t i = off[p], e = off[p + 1]; i < e; ++i)
            f(nbr[i], w ? w[i] : 1.0f);
    }
    Csr c_;
};

} // namespace vgraph

#endif
