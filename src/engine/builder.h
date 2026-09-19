// vgraph engine: builds a snapshot from an edge list.
//
// Peak memory, unweighted: about 16 bytes per edge while edges arrive, then
// 16 bytes per edge + 32 bytes per node while the snapshot is written.
// Weighted adds 4 bytes per edge while edges arrive and 8 in the snapshot.
// Input that is not sorted by (src, dst) is sorted here; weighted unsorted
// input needs 16 more bytes per edge for that sort.
// With directed=false every edge is stored in both directions, so count
// each input edge twice.
#ifndef VGRAPH_ENGINE_BUILDER_H
#define VGRAPH_ENGINE_BUILDER_H

#include "snapshot.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace vgraph {

// Seconds spent in each phase of finish(). For tuning and for build reports.
struct BuildTimings {
    double ids = 0, keys = 0, sort = 0, write = 0;
};

class GraphBuilder {
public:
    // directed=true: every edge is one directed edge, a reverse CSR is stored.
    // directed=false: the reverse of every edge is added, the graph is symmetric.
    GraphBuilder(bool directed, bool weighted);

    void add_edge(std::int64_t src, std::int64_t dst, float weight = 1.0f);
    // Turns an unweighted builder into a weighted one. Edges added so far get weight 1.
    void enable_weights();
    bool weighted() const { return weighted_; }
    // A node without edges. Nodes that appear in an edge need no add_node.
    void add_node(std::int64_t id);

    std::uint64_t raw_edge_count() const { return raw_count_; }

    // Duplicate edges are stored once (the first weight wins). Self loops are kept.
    // The builder is empty afterwards.
    void finish(std::int64_t max_epoch, SnapshotBuffer &out);
    const BuildTimings &timings() const { return timings_; }

private:
    static constexpr std::size_t CHUNK_EDGES = 1u << 20;
    static constexpr std::size_t ID_BLOCK = 1u << 22;     // unknown ids collected before a merge
    struct Chunk {
        std::unique_ptr<std::int64_t[]> sd;   // src, dst pairs
        std::unique_ptr<float[]> w;
        std::size_t n = 0;
    };
    void push(std::int64_t src, std::int64_t dst, float weight);
    static void merge_ids(std::vector<std::int64_t> &ids, std::vector<std::int64_t> &block);

    bool directed_;
    bool weighted_;
    std::vector<Chunk> chunks_;
    std::vector<std::int64_t> extra_nodes_;
    std::uint64_t raw_count_ = 0;
    BuildTimings timings_;
};

} // namespace vgraph

#endif
