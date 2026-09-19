// Delta overlay: hand-made cases (add, delete, re-add, new nodes, undirected,
// idempotence) and a randomised comparison against a naive graph.
#include "check.h"

#include "../../src/engine/bfs.h"
#include "../../src/engine/cc.h"
#include "../../src/engine/delta.h"
#include "../../src/engine/pagerank.h"
#include "../../src/engine/path.h"

#include <cmath>

using namespace vgraph;

using Ids = std::vector<std::int64_t>;
using HopMap = std::map<std::int64_t, std::int64_t>;

static Ids nbrs(const Overlay &g, std::int64_t id, bool out)
{
    Ids r;
    pos_t p;
    if (!g.find(id, p)) return r;
    auto take = [&](pos_t v, float) { r.push_back(g.id_of(v)); };
    if (out) g.for_out(p, take); else g.for_in(p, take);
    std::sort(r.begin(), r.end());
    return r;
}

static HopMap hops(const Overlay &g, std::int64_t start, std::int64_t depth, Direction dir)
{
    HopMap r;
    pos_t p;
    if (!g.find(start, p)) return r;
    BfsScratch s;
    KhopOptions o;
    o.depth = depth;
    o.direction = dir;
    khop(g, p, o, s, [&](pos_t v, std::int64_t h) { r[g.id_of(v)] = h; });
    return r;
}

static void hand_made()
{
    TestGraph t;                      // 1 -> 2 -> 3, 3 -> 1, 5 isolated
    build(t, {{1, 2}, {2, 3}, {3, 1}}, true, false, {5});
    {
        Overlay g(t.csr);
        CHECK(g.empty() && g.node_count() == 4);
        CHECK(hops(g, 1, 9, Direction::Out) == HopMap({{1, 0}, {2, 1}, {3, 2}}));

        g.apply(2, 3, -1);                                   // delete a snapshot edge
        CHECK(nbrs(g, 2, true).empty() && nbrs(g, 3, false).empty());
        CHECK(hops(g, 1, 9, Direction::Out) == HopMap({{1, 0}, {2, 1}}));
        g.apply(2, 3, -1);                                   // again: no change
        CHECK(g.deleted_edges() == 1);

        g.apply(2, 3, 1);                                    // re-add
        CHECK(nbrs(g, 2, true) == Ids({3}) && g.deleted_edges() == 0 && g.added_edges() == 0);

        g.apply(3, 5, 1);                                    // new edge between known nodes
        g.apply(3, 5, 1);                                    // twice: stored once
        CHECK(nbrs(g, 3, true) == Ids({1, 5}) && nbrs(g, 5, false) == Ids({3}) && g.added_edges() == 1);

        g.apply(5, 100, 1);                                  // new node 100, then -7 (smaller than all ids)
        g.apply(100, -7, 1);
        CHECK(g.node_count() == 6 && g.new_nodes() == 2);
        CHECK(hops(g, 1, 9, Direction::Out) == HopMap({{1, 0}, {2, 1}, {3, 2}, {5, 3}, {100, 4}, {-7, 5}}));
        CHECK(hops(g, -7, 2, Direction::In) == HopMap({{-7, 0}, {100, 1}, {5, 2}}));

        g.apply(3, 5, -1);                                   // delete an edge that only the delta added
        CHECK(nbrs(g, 3, true) == Ids({1}) && nbrs(g, 5, false).empty() && g.added_edges() == 2);
        g.apply(8, 9, -1);                                   // delete between unknown nodes: nothing happens
        CHECK(g.node_count() == 6);

        // Components are named by the smallest id, also when that node came with the delta.
        std::vector<pos_t> comp;
        connected_components(g, comp);
        pos_t p1, p5, p7;
        g.find(1, p1); g.find(5, p5); g.find(-7, p7);
        CHECK(g.id_of(comp[p1]) == 1 && g.id_of(comp[p5]) == -7 && g.id_of(comp[p7]) == -7);

        PathScratch ps;
        std::vector<pos_t> path;
        pos_t p100;
        g.find(100, p100);
        CHECK(shortest_path(g, p5, p7, PathOptions(), ps, path) && path.size() == 3 && path[1] == p100);
        CHECK(!shortest_path(g, p1, p7, PathOptions(), ps, path));
    }
    {   // undirected snapshot: one journal row changes both directions
        TestGraph u;
        build(u, {{1, 2}, {2, 3}}, false);
        Overlay g(u.csr);
        g.apply(3, 2, -1);
        CHECK(nbrs(g, 2, true) == Ids({1}) && nbrs(g, 3, true).empty() && nbrs(g, 2, false) == Ids({1}));
        g.apply(3, 4, 1);
        CHECK(nbrs(g, 4, true) == Ids({3}) && nbrs(g, 3, false) == Ids({4}));
        CHECK(hops(g, 4, 9, Direction::Out) == HopMap({{4, 0}, {3, 1}}));
    }
    {   // weights: a delta edge carries its weight, a re-added snapshot edge keeps the snapshot weight
        TestGraph w;
        build(w, {{1, 2, 5.0f}, {2, 3, 5.0f}}, true, true);
        Overlay g(w.csr);
        g.apply(1, 3, 1, 20.0f);
        PathOptions po;
        po.weighted = true;
        PathScratch ps;
        std::vector<pos_t> path;
        pos_t a, b;
        g.find(1, a); g.find(3, b);
        CHECK(shortest_path(g, a, b, po, ps, path) && path.size() == 3);     // 10 via node 2 beats 20
        g.apply(1, 3, 1, 2.0f);                                                // weight update
        CHECK(shortest_path(g, a, b, po, ps, path) && path.size() == 2);
    }
}

static void randomised(std::uint64_t seed, bool directed)
{
    Rng rng(seed);
    const std::int64_t nodes = 120;
    auto id = [&]() { return rng.below(nodes) * 3 - 40; };

    // State of truth: the set of live edges, as given (not mirrored).
    std::set<std::pair<std::int64_t, std::int64_t>> live;
    auto norm = [&](std::int64_t s, std::int64_t d) {
        return (!directed && d < s) ? std::make_pair(d, s) : std::make_pair(s, d);
    };
    std::vector<Edge> base;
    for (int i = 0; i < 300; ++i) {
        const std::int64_t s = id(), d = id();
        base.push_back(Edge(s, d));
        live.insert(norm(s, d));
    }
    TestGraph t;
    build(t, base, directed);
    Overlay g(t.csr);

    std::vector<std::pair<std::pair<std::int64_t, std::int64_t>, int>> journal;
    for (int i = 0; i < 400; ++i) {
        // Half of the ids come from a wider range: new nodes.
        const std::int64_t s = (i % 2) ? id() : rng.below(nodes * 2) * 3 - 40;
        const std::int64_t d = (i % 3) ? id() : rng.below(nodes * 2) * 3 - 40;
        const int op = rng.below(3) == 0 ? 1 : -1;
        const auto e = (op < 0 && !live.empty() && rng.below(2))
                           ? *std::next(live.begin(), rng.below(live.size()))    // delete something that exists
                           : std::make_pair(s, d);
        journal.push_back(std::make_pair(e, op));
        g.apply(e.first, e.second, op);
        if (op > 0) live.insert(norm(e.first, e.second)); else live.erase(norm(e.first, e.second));
    }
    // Idempotent: the whole journal once more, in order, changes nothing.
    for (const auto &j : journal) g.apply(j.first.first, j.first.second, j.second);

    Naive nv;
    for (const auto &e : live) nv.add(e.first, e.second, directed);

    const Direction dirs[] = {Direction::Out, Direction::In, Direction::Both};
    std::vector<std::int64_t> ids(nv.nodes.begin(), nv.nodes.end());
    for (int round = 0; round < 60; ++round) {
        const std::int64_t start = ids[rng.below(ids.size())];
        const Direction dir = dirs[round % 3];
        const std::set<std::int64_t> want_out = nv.nbrs(start, Direction::Out), want_in = nv.nbrs(start, Direction::In);
        CHECK(nbrs(g, start, true) == Ids(want_out.begin(), want_out.end()));
        CHECK(nbrs(g, start, false) == Ids(want_in.begin(), want_in.end()));
        const HopMap expect = nv.distances(start, dir);
        CHECK(hops(g, start, 1000, dir) == expect);
    }

    // PageRank over the nodes of the overlay. Nodes that lost all edges still count as nodes.
    std::vector<double> rank;
    pagerank(g, 10, 0.85, rank);
    double sum = 0;
    for (double r : rank) sum += r;
    CHECK(std::fabs(sum - 1.0) < 1e-9);
}

int main()
{
    hand_made();
    randomised(11, true);
    randomised(12, false);
    randomised(13, true);
    return finish("test_delta");
}
