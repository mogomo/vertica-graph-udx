// Small helpers shared by the engine unit tests.
#ifndef VGRAPH_TESTS_CHECK_H
#define VGRAPH_TESTS_CHECK_H

#include "../../src/engine/builder.h"
#include "../../src/engine/csr.h"
#include "../../src/engine/snapshot.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <utility>
#include <vector>

static int failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

inline int finish(const char *name)
{
    if (failures == 0) std::printf("%s: OK\n", name);
    else std::printf("%s: %d FAILURES\n", name, failures);
    return failures == 0 ? 0 : 1;
}

struct Edge {
    std::int64_t src, dst;
    float w;
    Edge(std::int64_t s, std::int64_t d, float weight = 1.0f) : src(s), dst(d), w(weight) {}
};

// A built snapshot together with the bytes it points into.
struct TestGraph {
    vgraph::SnapshotBuffer buffer;
    vgraph::Csr csr;
    vgraph::CsrGraph graph() const { return vgraph::CsrGraph(csr); }
    vgraph::pos_t pos(std::int64_t id) const {
        vgraph::pos_t p = vgraph::NO_POS;
        graph().find(id, p);
        return p;
    }
};

inline void build(TestGraph &t, const std::vector<Edge> &edges, bool directed = true, bool weighted = false,
                  const std::vector<std::int64_t> &nodes = {}, std::int64_t max_ver = 0)
{
    vgraph::GraphBuilder b(directed, weighted);
    for (const Edge &e : edges) b.add_edge(e.src, e.dst, e.w);
    for (std::int64_t id : nodes) b.add_node(id);
    b.finish(max_ver, t.buffer);
    t.csr = vgraph::snapshot_open(t.buffer.data(), t.buffer.size(), true);
}

// Naive reference graph: adjacency sets keyed by node id.
struct Naive {
    std::map<std::int64_t, std::set<std::int64_t>> out, in;
    std::set<std::int64_t> nodes;
    void add(std::int64_t s, std::int64_t d, bool directed) {
        nodes.insert(s); nodes.insert(d);
        out[s].insert(d); in[d].insert(s);
        if (!directed) { out[d].insert(s); in[s].insert(d); }
    }
    std::set<std::int64_t> nbrs(std::int64_t u, vgraph::Direction dir) const {
        std::set<std::int64_t> r;
        if (dir != vgraph::Direction::In && out.count(u)) r.insert(out.at(u).begin(), out.at(u).end());
        if (dir != vgraph::Direction::Out && in.count(u)) r.insert(in.at(u).begin(), in.at(u).end());
        return r;
    }
    // Hop count of every node reachable from start.
    std::map<std::int64_t, std::int64_t> distances(std::int64_t start, vgraph::Direction dir) const {
        std::map<std::int64_t, std::int64_t> dist;
        std::vector<std::int64_t> frontier(1, start);
        dist[start] = 0;
        while (!frontier.empty()) {
            std::vector<std::int64_t> next;
            for (std::int64_t u : frontier)
                for (std::int64_t v : nbrs(u, dir))
                    if (!dist.count(v)) { dist[v] = dist[u] + 1; next.push_back(v); }
            frontier.swap(next);
        }
        return dist;
    }
};

// Deterministic pseudo random numbers, same on every platform.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    std::int64_t below(std::int64_t n) { return static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(n)); }
};

#endif
