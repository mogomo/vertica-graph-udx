// Builder and snapshot format: hand-made graphs.
#include "check.h"

#include <cstring>
#include <stdexcept>

using namespace vgraph;

static std::vector<std::int64_t> out_ids(const TestGraph &t, std::int64_t id)
{
    std::vector<std::int64_t> r;
    CsrGraph g = t.graph();
    g.for_out(t.pos(id), [&](pos_t v, float) { r.push_back(g.id_of(v)); });
    return r;
}
static std::vector<std::int64_t> in_ids(const TestGraph &t, std::int64_t id)
{
    std::vector<std::int64_t> r;
    CsrGraph g = t.graph();
    g.for_in(t.pos(id), [&](pos_t v, float) { r.push_back(g.id_of(v)); });
    return r;
}
using Ids = std::vector<std::int64_t>;

int main()
{
    {   // directed, unsorted input, duplicate edge, self loop, isolated node, negative and large ids
        TestGraph t;
        build(t, {{30, 10}, {10, 20}, {10, 30}, {10, 20}, {20, 20}, {-5, 10}, {10, 9000000000LL}},
              true, false, {77}, 42);
        CHECK(t.csr.node_count == 6);
        CHECK(t.csr.edge_count == 6);           // duplicate 10->20 stored once
        CHECK(t.csr.max_epoch == 42);
        CHECK(t.csr.directed && !t.csr.weighted);
        CHECK(out_ids(t, 10) == Ids({20, 30, 9000000000LL}));
        CHECK(in_ids(t, 10) == Ids({-5, 30}));
        CHECK(out_ids(t, 20) == Ids({20}));     // self loop kept
        CHECK(in_ids(t, 20) == Ids({10, 20}));
        CHECK(out_ids(t, 77).empty() && in_ids(t, 77).empty());
        CHECK(t.pos(12345) == NO_POS);
    }
    {   // undirected: reverse edges are added, no separate reverse CSR
        TestGraph t;
        build(t, {{1, 2}, {2, 3}, {3, 3}}, false);
        CHECK(!t.csr.directed);
        CHECK(t.csr.edge_count == 5);           // 1-2, 2-1, 2-3, 3-2, 3-3
        CHECK(out_ids(t, 2) == Ids({1, 3}));
        CHECK(in_ids(t, 2) == Ids({1, 3}));
        CHECK(t.csr.in_nbrs == t.csr.out_nbrs);
    }
    {   // weighted: first weight of a duplicate wins, reverse CSR carries the weights
        TestGraph t;
        build(t, {{2, 1, 5.0f}, {1, 2, 1.5f}, {1, 2, 9.0f}, {1, 3, 2.5f}}, true, true);
        CHECK(t.csr.weighted && t.csr.edge_count == 3);
        CsrGraph g = t.graph();
        std::vector<float> w;
        g.for_out(t.pos(1), [&](pos_t, float x) { w.push_back(x); });
        CHECK(w == std::vector<float>({1.5f, 2.5f}));
        w.clear();
        g.for_in(t.pos(2), [&](pos_t, float x) { w.push_back(x); });
        CHECK(w == std::vector<float>({1.5f}));
    }
    {   // empty graph
        TestGraph t;
        build(t, {});
        CHECK(t.csr.node_count == 0 && t.csr.edge_count == 0);
    }
    {   // more than one builder chunk, sorted input
        TestGraph t;
        std::vector<Edge> edges;
        const std::int64_t n = 1200000;
        for (std::int64_t i = 0; i < n; ++i) edges.push_back(Edge(i, (i + 1) % n));
        build(t, edges);
        CHECK(t.csr.node_count == static_cast<std::uint64_t>(n));
        CHECK(t.csr.edge_count == static_cast<std::uint64_t>(n));
        CHECK(out_ids(t, n - 1) == Ids({0}));
        CHECK(in_ids(t, 0) == Ids({n - 1}));
    }
    {   // damaged snapshots are rejected
        TestGraph t;
        build(t, {{1, 2}, {2, 3}});
        std::vector<std::uint64_t> copy(t.buffer.size() / 8);
        std::memcpy(copy.data(), t.buffer.data(), t.buffer.size());
        std::uint8_t *bytes = reinterpret_cast<std::uint8_t *>(copy.data());
        CHECK(snapshot_has_magic(bytes, t.buffer.size()));

        auto rejected = [&](std::uint64_t size, bool verify) {
            try { snapshot_open(bytes, size, verify); } catch (const std::runtime_error &) { return true; }
            return false;
        };
        CHECK(!rejected(t.buffer.size(), true));
        CHECK(rejected(t.buffer.size() - 8, false));    // truncated
        bytes[t.buffer.size() - 1] ^= 1;                // flipped bit in the last section
        CHECK(rejected(t.buffer.size(), true));
        CHECK(!rejected(t.buffer.size(), false));       // only the checksum notices it
        bytes[t.buffer.size() - 1] ^= 1;
        bytes[0] = 'X';                                 // wrong magic
        CHECK(rejected(t.buffer.size(), false));
        CHECK(!snapshot_has_magic(bytes, t.buffer.size()));
    }
    return finish("test_builder");
}
