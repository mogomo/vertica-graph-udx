// BFS k-hop, shortest path, components, PageRank: hand-made graphs and a
// randomised comparison against naive implementations.
#define VGRAPH_PARALLEL_BLOCK 5      // tiny blocks: the small test graphs are spread over several threads
#include "check.h"

#include "../../src/engine/bfs.h"
#include "../../src/engine/cc.h"
#include "../../src/engine/pagerank.h"
#include "../../src/engine/path.h"

#include <cmath>
#include <limits>

using namespace vgraph;

using HopMap = std::map<std::int64_t, std::int64_t>;

static HopMap run_khop(const TestGraph &t, std::int64_t start, KhopOptions opt, BfsScratch &s)
{
    HopMap r;
    CsrGraph g = t.graph();
    khop(g, t.pos(start), opt, s, [&](pos_t p, std::int64_t hops) {
        CHECK(!r.count(g.id_of(p)));            // each node once
        r[g.id_of(p)] = hops;
    });
    return r;
}

static KhopOptions opts(std::int64_t depth, Direction dir = Direction::Out, bool exact = false,
                        std::int64_t max_results = 0)
{
    KhopOptions o;
    o.depth = depth; o.direction = dir; o.exact = exact; o.max_results = max_results;
    return o;
}

static std::vector<std::int64_t> run_path(const TestGraph &t, std::int64_t a, std::int64_t b, PathOptions opt,
                                          PathScratch &s)
{
    std::vector<pos_t> p;
    std::vector<std::int64_t> ids;
    CsrGraph g = t.graph();
    bool found = shortest_path(g, t.pos(a), t.pos(b), opt, s, p);
    CHECK(found == !p.empty());
    for (pos_t x : p) ids.push_back(g.id_of(x));
    return ids;
}

static void hand_made()
{
    BfsScratch bs;
    PathScratch ps;
    using Ids = std::vector<std::int64_t>;

    // 1 -> 2 -> 3 -> 1 (cycle), 3 -> 4, 5 -> 5 (self loop), 6 isolated, 7 -> 8
    TestGraph d;
    build(d, {{1, 2}, {2, 3}, {3, 1}, {3, 4}, {5, 5}, {7, 8}, {1, 2}}, true, false, {6});

    CHECK(run_khop(d, 1, opts(0), bs) == HopMap({{1, 0}}));
    CHECK(run_khop(d, 1, opts(1), bs) == HopMap({{1, 0}, {2, 1}}));
    CHECK(run_khop(d, 1, opts(9), bs) == HopMap({{1, 0}, {2, 1}, {3, 2}, {4, 3}}));   // cycle ends
    CHECK(run_khop(d, 1, opts(3, Direction::Out, true), bs) == HopMap({{4, 3}}));
    CHECK(run_khop(d, 1, opts(5, Direction::Out, true), bs).empty());
    CHECK(run_khop(d, 1, opts(1, Direction::In), bs) == HopMap({{1, 0}, {3, 1}}));
    CHECK(run_khop(d, 4, opts(1, Direction::Both), bs) == HopMap({{4, 0}, {3, 1}}));
    CHECK(run_khop(d, 3, opts(1, Direction::Both), bs) == HopMap({{3, 0}, {1, 1}, {2, 1}, {4, 1}}));
    CHECK(run_khop(d, 5, opts(4), bs) == HopMap({{5, 0}}));                            // self loop
    CHECK(run_khop(d, 6, opts(4, Direction::Both), bs) == HopMap({{6, 0}}));           // isolated
    CHECK(run_khop(d, 1, opts(9, Direction::Out, false, 2), bs).size() == 2);          // max_results
    CHECK(run_khop(d, 1, opts(9), bs).size() == 4);                                    // scratch is clean again

    PathOptions po;
    CHECK(run_path(d, 1, 4, po, ps) == Ids({1, 2, 3, 4}));
    CHECK(run_path(d, 1, 1, po, ps) == Ids({1}));
    CHECK(run_path(d, 4, 1, po, ps).empty());
    CHECK(run_path(d, 1, 8, po, ps).empty());
    po.direction = Direction::In;
    CHECK(run_path(d, 4, 1, po, ps) == Ids({4, 3, 2, 1}));
    po.direction = Direction::Both;
    CHECK(run_path(d, 4, 1, po, ps) == Ids({4, 3, 1}));
    po = PathOptions();
    po.max_depth = 2;
    CHECK(run_path(d, 1, 4, po, ps).empty());
    po.max_depth = 3;
    CHECK(run_path(d, 1, 4, po, ps) == Ids({1, 2, 3, 4}));

    // Weighted: the direct edge is more expensive than the detour.
    TestGraph w;
    build(w, {{1, 4, 10.0f}, {1, 2, 1.0f}, {2, 3, 1.0f}, {3, 4, 1.0f}, {4, 5, 0.0f}}, true, true);
    po = PathOptions();
    CHECK(run_path(w, 1, 4, po, ps) == Ids({1, 4}));               // by hops
    po.weighted = true;
    CHECK(run_path(w, 1, 5, po, ps) == Ids({1, 2, 3, 4, 5}));      // by weight
    CHECK(run_path(w, 5, 1, po, ps).empty());
    CHECK(run_path(d, 1, 4, po, ps) == Ids({1, 2, 3, 4}));         // unweighted graph: weight 1

    TestGraph neg;
    build(neg, {{1, 2, -1.0f}}, true, true);
    bool thrown = false;
    try { run_path(neg, 1, 2, po, ps); } catch (const std::runtime_error &) { thrown = true; }
    CHECK(thrown);

    // Components: weakly connected, named by the smallest id.
    std::vector<pos_t> comp;
    CsrGraph g = d.graph();
    connected_components(g, comp);
    HopMap by_id;
    for (pos_t p = 0; p < g.node_count(); ++p) by_id[g.id_of(p)] = g.id_of(comp[p]);
    CHECK(by_id == HopMap({{1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 5}, {6, 6}, {7, 7}, {8, 7}}));

    // PageRank: ranks sum to 1; on a directed cycle all ranks are equal.
    TestGraph cyc;
    build(cyc, {{1, 2}, {2, 3}, {3, 1}});
    std::vector<double> rank;
    pagerank(cyc.graph(), 20, 0.85, rank);
    CHECK(rank.size() == 3);
    for (double r : rank) CHECK(std::fabs(r - 1.0 / 3) < 1e-12);
    pagerank(g, 20, 0.85, rank);                // has dangling nodes 4, 6, 8
    double sum = 0;
    for (double r : rank) sum += r;
    CHECK(std::fabs(sum - 1.0) < 1e-9);
    CHECK(rank[d.pos(4)] > rank[d.pos(6)]);
}

// Naive PageRank on dense id-indexed structures, same definition as the engine.
static std::map<std::int64_t, double> naive_pagerank(const Naive &nv, int iterations, double damping)
{
    const double n = static_cast<double>(nv.nodes.size());
    std::map<std::int64_t, double> rank, next;
    for (std::int64_t id : nv.nodes) rank[id] = 1.0 / n;
    for (int it = 0; it < iterations; ++it) {
        double dangling = 0;
        for (std::int64_t id : nv.nodes)
            if (!nv.out.count(id)) dangling += rank[id];
        for (std::int64_t id : nv.nodes) next[id] = (1.0 - damping) / n + damping * dangling / n;
        for (const auto &kv : nv.out)
            for (std::int64_t v : kv.second) next[v] += damping * rank[kv.first] / kv.second.size();
        rank.swap(next);
    }
    return rank;
}

static void randomised(std::uint64_t seed, std::int64_t nodes, std::int64_t edges, bool directed)
{
    Rng rng(seed);
    Naive nv;
    std::vector<Edge> list;
    for (std::int64_t i = 0; i < edges; ++i) {
        // Sparse ids, so that positions differ from ids.
        const std::int64_t s = rng.below(nodes) * 7 - 50, t = rng.below(nodes) * 7 - 50;
        list.push_back(Edge(s, t));
        nv.add(s, t, directed);
    }
    TestGraph t;
    build(t, list, directed);
    CsrGraph g = t.graph();
    CHECK(g.node_count() == nv.nodes.size());

    BfsScratch bs;
    PathScratch ps;
    const Direction dirs[] = {Direction::Out, Direction::In, Direction::Both};
    std::vector<std::int64_t> ids(nv.nodes.begin(), nv.nodes.end());

    for (int round = 0; round < 40; ++round) {
        const std::int64_t start = ids[rng.below(ids.size())];
        const std::int64_t target = ids[rng.below(ids.size())];
        const Direction dir = dirs[round % 3];
        const std::int64_t depth = rng.below(6);
        const HopMap dist = nv.distances(start, dir);

        HopMap expect, expect_exact;
        for (const auto &kv : dist) {
            if (kv.second <= depth) expect[kv.first] = kv.second;
            if (kv.second == depth) expect_exact[kv.first] = kv.second;
        }
        CHECK(run_khop(t, start, opts(depth, dir), bs) == expect);
        CHECK(run_khop(t, start, opts(depth, dir, true), bs) == expect_exact);

        PathOptions po;
        po.direction = dir;
        const std::vector<std::int64_t> p = run_path(t, start, target, po, ps);
        if (!dist.count(target)) {
            CHECK(p.empty());
        } else {
            CHECK(static_cast<std::int64_t>(p.size()) == dist.at(target) + 1);
            CHECK(p.front() == start && p.back() == target);
            for (std::size_t i = 1; i < p.size(); ++i) CHECK(nv.nbrs(p[i - 1], dir).count(p[i]) == 1);
            po.max_depth = dist.at(target);     // exactly enough
            CHECK(run_path(t, start, target, po, ps).size() == p.size());
            if (dist.at(target) > 1) {          // 0 means no limit
                po.max_depth = dist.at(target) - 1;
                CHECK(run_path(t, start, target, po, ps).empty());
            }
            po.max_depth = 0;
            po.weighted = true;                 // all weights 1: same length
            CHECK(run_path(t, start, target, po, ps).size() == p.size());
        }
    }

    // Components against BFS over both directions.
    std::vector<pos_t> comp;
    connected_components(g, comp);
    for (int round = 0; round < 20; ++round) {
        const std::int64_t id = ids[rng.below(ids.size())];
        const HopMap dist = nv.distances(id, Direction::Both);
        CHECK(g.id_of(comp[t.pos(id)]) == dist.begin()->first);    // smallest id reachable
    }

    std::vector<pos_t> comp4;
    connected_components(g, comp4, 4);
    CHECK(comp4 == comp);                       // threads do not change the result

    std::vector<double> rank, rank4;
    pagerank(g, 20, 0.85, rank);
    pagerank(g, 20, 0.85, rank4, 4);
    CHECK(rank4 == rank);                       // bit for bit
    const std::map<std::int64_t, double> expect = naive_pagerank(nv, 20, 0.85);
    for (pos_t p = 0; p < g.node_count(); ++p) CHECK(std::fabs(rank[p] - expect.at(g.id_of(p))) < 1e-12);
}

// A long chain: the search visits every node, so its links outgrow the hash table and move
// into an array on the way (see LinkStore in path.h). Then a small search on the same scratch.
static void long_chain()
{
    const std::int64_t n = 40000;
    std::vector<Edge> list;
    for (std::int64_t i = 0; i + 1 < n; ++i) list.push_back(Edge(i * 3, (i + 1) * 3));
    TestGraph t;
    build(t, list, true);
    PathScratch ps;
    PathOptions po;
    for (int round = 0; round < 2; ++round) {
        po.weighted = (round == 1);
        std::vector<std::int64_t> p = run_path(t, 0, (n - 1) * 3, po, ps);
        CHECK(static_cast<std::int64_t>(p.size()) == n);
        for (std::size_t i = 0; i < p.size(); ++i) CHECK(p[i] == static_cast<std::int64_t>(i) * 3);
        CHECK(run_path(t, (n - 1) * 3, 0, po, ps).empty());          // against the direction
        CHECK(run_path(t, 300, 330, po, ps).size() == 11);
    }
    po.weighted = false;
    po.direction = Direction::Both;
    CHECK(static_cast<std::int64_t>(run_path(t, (n - 1) * 3, 0, po, ps).size()) == n);
}

int main()
{
    hand_made();
    long_chain();
    randomised(1, 50, 60, true);        // sparse, many components
    randomised(2, 50, 400, true);       // dense
    randomised(3, 300, 900, false);     // undirected
    randomised(4, 2000, 6000, true);
    return finish("test_algorithms");
}
