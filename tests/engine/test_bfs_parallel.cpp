// The multi-threaded BFS against the single-threaded one: same nodes, same hop counts, for every
// direction, with and without node lists, on graphs large enough to use both kinds of level.
#define VGRAPH_PARALLEL_BLOCK 64      // tiny blocks and levels: small test graphs still use the threads
#define VGRAPH_SERIAL_LEVEL 2
#include "check.h"

#include "../../src/engine/bfs.h"
#include "../../src/engine/bfs_parallel.h"
#include "../../src/engine/delta.h"

using namespace vgraph;
using HopMap = std::map<pos_t, std::int64_t>;

template <class G>
static void compare(const G &g, pos_t start, const KhopOptions &opt, BfsScratch &bs, ParallelBfsScratch &ps)
{
    HopMap expect;
    std::map<std::int64_t, std::int64_t> expect_counts;
    khop(g, start, opt, bs, [&](pos_t p, std::int64_t hops) { expect[p] = hops; ++expect_counts[hops]; });

    for (int threads : {1, 4}) {
        HopMap got;
        std::map<std::int64_t, std::int64_t> counts;
        std::int64_t n = khop_parallel(g, start, opt, ps, threads, true,
            [&](std::int64_t hops, std::int64_t count, const std::vector<std::vector<pos_t>> *lists) {
                std::int64_t in_lists = 0;
                for (const auto &list : *lists) for (pos_t p : list) { CHECK(!got.count(p)); got[p] = hops; ++in_lists; }
                CHECK(in_lists == count);
            });
        CHECK(got == expect);
        CHECK(n == static_cast<std::int64_t>(expect.size()));
        n = khop_parallel(g, start, opt, ps, threads, false,
            [&](std::int64_t hops, std::int64_t count, const std::vector<std::vector<pos_t>> *lists) {
                CHECK(lists == nullptr);
                counts[hops] += count;
            });
        CHECK(counts == expect_counts);
        CHECK(n == static_cast<std::int64_t>(expect.size()));
    }
}

static void randomised(std::uint64_t seed, std::int64_t nodes, std::int64_t edges, bool directed)
{
    Rng rng(seed);
    std::vector<Edge> list;
    for (std::int64_t i = 0; i < edges; ++i) list.push_back(Edge(rng.below(nodes) * 3, rng.below(nodes) * 3));
    TestGraph t;
    build(t, list, directed);
    CsrGraph g = t.graph();
    BfsScratch bs;
    ParallelBfsScratch ps;
    const Direction dirs[] = {Direction::Out, Direction::In, Direction::Both};
    for (int round = 0; round < 30; ++round) {
        KhopOptions opt;
        opt.direction = dirs[round % 3];
        opt.depth = round % 5 == 4 ? 1000 : rng.below(8);
        opt.exact = round % 7 == 3;
        compare(g, static_cast<pos_t>(rng.below(g.node_count())), opt, bs, ps);
    }
}

int main()
{
    randomised(11, 300, 250, true);        // sparse, many components, not a multiple of 64 nodes
    randomised(12, 1000, 4000, true);      // one big component: bottom-up levels
    randomised(13, 1024, 3000, false);     // undirected, node count a multiple of 64
    randomised(14, 5000, 40000, true);     // dense
    return finish("test_bfs_parallel");
}
