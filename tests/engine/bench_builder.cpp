// Engine benchmark: build a graph shaped like the demo data (every person picks
// 3 random contacts, both directions stored) and time the builder phases and a BFS.
//   bench_builder [edges]      default 100000000
#include "check.h"

#include "../../src/engine/bfs.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

using namespace vgraph;

static double since(std::chrono::steady_clock::time_point &t)
{
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - t).count();
    t = now;
    return s;
}

int main(int argc, char **argv)
{
    const std::int64_t edges = argc > 1 ? std::atoll(argv[1]) : 100000000;
    const std::int64_t people = edges / 6;
    auto clock = std::chrono::steady_clock::now();

    // Sorted by (src, dst), as gbuild receives it.
    std::vector<std::pair<std::int64_t, std::int64_t>> list;
    list.reserve(people * 6);
    Rng rng(42);
    for (std::int64_t p = 1; p <= people; ++p)
        for (int k = 0; k < 3; ++k) {
            const std::int64_t other = rng.below(people) + 1;
            list.emplace_back(p, other);
            list.emplace_back(other, p);
        }
    std::sort(list.begin(), list.end());
    std::printf("generate + sort input  %7.2f s  (%zu edges, %lld people)\n", since(clock), list.size(), (long long)people);

    GraphBuilder b(true, false);
    for (const auto &e : list) b.add_edge(e.first, e.second);
    std::vector<std::pair<std::int64_t, std::int64_t>>().swap(list);
    std::printf("add_edge               %7.2f s\n", since(clock));

    SnapshotBuffer buffer;
    b.finish(0, buffer);
    const BuildTimings &t = b.timings();
    std::printf("finish                 %7.2f s  (ids %.2f, keys %.2f, sort %.2f, write %.2f)\n",
                since(clock), t.ids, t.keys, t.sort, t.write);

    const Csr csr = snapshot_open(buffer.data(), buffer.size(), true);
    std::printf("open + checksum        %7.2f s  (%llu nodes, %llu edges, %.1f MB)\n", since(clock),
                (unsigned long long)csr.node_count, (unsigned long long)csr.edge_count, buffer.size() / 1048576.0);

    CsrGraph g(csr);
    BfsScratch scratch;
    KhopOptions opt;
    opt.depth = 9;
    pos_t start;
    g.find(1, start);
    std::int64_t found = khop(g, start, opt, scratch, [](pos_t, std::int64_t) {});
    std::printf("bfs depth 9            %7.2f s  (%lld nodes)\n", since(clock), (long long)found);
    return 0;
}
