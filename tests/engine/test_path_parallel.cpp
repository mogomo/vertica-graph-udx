// The shortest path by hops on several threads: the same path with any number of threads, a real
// path, as short as a plain breadth-first search says, for every direction, with max_depth, on a
// snapshot and on a snapshot with a delta. Graphs large enough to use the threads and the array.
#define VGRAPH_SERIAL_LEVEL 2         // tiny levels and ranges: small test graphs still use the threads
#define VGRAPH_PATH_RANGE 3
#include "check.h"

#include "../../src/engine/bfs.h"
#include "../../src/engine/delta.h"
#include "../../src/engine/path.h"

using namespace vgraph;

template <class G>
static bool has_edge(const G &g, pos_t u, pos_t v, Direction dir)
{
    bool hit = false;
    g.for_dir(u, dir, [&](pos_t w, float) { if (w == v) hit = true; });
    return hit;
}

template <class G>
static void compare(const G &g, pos_t start, pos_t target, const PathOptions &opt, BfsScratch &bs, PathScratch &ps)
{
    std::int64_t expect = -1;                   // hops by a plain search from one side
    KhopOptions ko;
    ko.direction = opt.direction;
    ko.depth = opt.max_depth == 0 ? 1000000 : opt.max_depth;
    khop(g, start, ko, bs, [&](pos_t p, std::int64_t hops) { if (p == target) expect = hops; });

    std::vector<pos_t> first;
    for (int threads : {1, 2, 4, 8}) {
        std::vector<pos_t> path;
        const bool found = shortest_path(g, start, target, opt, ps, path, threads);
        CHECK(found == (expect >= 0));
        CHECK(static_cast<std::int64_t>(path.size()) == expect + 1);
        if (found) {
            CHECK(path.front() == start && path.back() == target);
            for (std::size_t i = 0; i + 1 < path.size(); ++i) CHECK(has_edge(g, path[i], path[i + 1], opt.direction));
        }
        if (threads == 1) first = path;
        CHECK(path == first);
    }
}

template <class G>
static void rounds(const G &g, Rng &rng, int count)
{
    BfsScratch bs;
    PathScratch ps;
    const Direction dirs[] = {Direction::Out, Direction::In, Direction::Both};
    for (int round = 0; round < count; ++round) {
        PathOptions opt;
        opt.direction = dirs[round % 3];
        opt.max_depth = round % 4 == 3 ? 1 + rng.below(6) : 0;
        compare(g, static_cast<pos_t>(rng.below(g.node_count())), static_cast<pos_t>(rng.below(g.node_count())), opt, bs, ps);
    }
}

static void randomised(std::uint64_t seed, std::int64_t nodes, std::int64_t edges, bool directed)
{
    Rng rng(seed);
    std::vector<Edge> list;
    for (std::int64_t i = 0; i < edges; ++i) list.push_back(Edge(rng.below(nodes) * 3, rng.below(nodes) * 3));
    TestGraph t;
    build(t, list, directed);
    rounds(t.graph(), rng, 60);

    Overlay delta(t.csr);                       // deletes, adds, new nodes
    for (std::int64_t i = 0; i < edges / 10; ++i) {
        const Edge &e = list[rng.below(edges)];
        delta.apply(e.src, e.dst, -1);
        delta.apply(rng.below(nodes + 20) * 3, rng.below(nodes + 20) * 3, 1);
    }
    rounds(delta, rng, 60);
}

int main()
{
    randomised(21, 300, 250, true);         // sparse, many components: often no path
    randomised(22, 2000, 5000, true);       // long paths
    randomised(23, 1500, 4000, false);      // undirected
    randomised(24, 3000, 30000, true);      // dense: the table moves into the array
    return finish("test_path_parallel");
}
