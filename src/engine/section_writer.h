// vgraph engine: writes one section of a snapshot from a sorted row stream.
//
// For graphs that do not fit the in-memory builder. The database does the heavy
// work (node map, mapping edges to positions, removing duplicates, sorting) and
// streams the result. Every section of the file is written by its own
// SectionWriter, in any order, even at the same time; the header comes last.
// Memory: one chunk, whatever the graph size.
//
// The bytes are exactly those the in-memory builder (builder.h) produces.
//
//   section        rows (a, b [, w]), sorted by a, b        one row per
//   IDS            a = position, b = node id                 node, positions 0..N-1
//   OUT_OFFSETS    a = position, b = out degree              node with out degree > 0
//   OUT_NBRS       a = source position, b = target position  edge
//   IN_OFFSETS     a = position, b = in degree               node with in degree > 0
//   IN_NBRS        a = target position, b = source position  edge
//   OUT_WEIGHTS    a = source, b = target, w                 edge
//   IN_WEIGHTS     a = target, b = source, w                 edge
#ifndef VGRAPH_ENGINE_SECTION_WRITER_H
#define VGRAPH_ENGINE_SECTION_WRITER_H

#include "snapshot.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vgraph {

enum class Section { Ids, OutOffsets, OutNbrs, InOffsets, InNbrs, OutWeights, InWeights };

// Returns false if the name is unknown. Names: ids, out_offsets, out_nbrs, in_offsets, in_nbrs,
// out_weights, in_weights.
bool parse_section(const std::string &name, Section &section);

// Header of a snapshot with these counts and flags, offsets filled, checksum 0.
SnapshotHeader make_header(std::uint64_t node_count, std::uint64_t edge_count, std::uint32_t flags,
                           std::int64_t max_ver);

class SectionWriter {
public:
    // sink(byte_offset, data, len) receives the finished pieces, each at most piece_bytes long.
    using Sink = std::function<void(std::uint64_t, const char *, std::uint64_t)>;

    SectionWriter(const SnapshotHeader &header, Section section, std::uint64_t piece_bytes, Sink sink);

    // Rows must come sorted by (a, b). Throws std::runtime_error when the stream is not what the
    // section needs (wrong order, position out of range, duplicate edge, wrong number of rows).
    void add(std::int64_t a, std::int64_t b, float w = 1.0f);

    // Flushes the last piece and checks the totals. Returns the checksum part of this section.
    std::uint64_t finish();

private:
    void put(const void *value, std::size_t size);
    void put_offsets_until(std::int64_t pos);
    [[noreturn]] void fail(const std::string &why) const;

    SnapshotHeader h_;
    Section section_;
    std::uint64_t piece_bytes_;
    Sink sink_;
    std::vector<std::uint64_t> buffer_;      // 8-byte aligned piece under construction
    std::uint64_t fill_ = 0;                 // bytes used in buffer_
    std::uint64_t file_offset_ = 0;          // file offset of buffer_[0]
    std::uint64_t section_end_ = 0;
    std::uint64_t checksum_ = 0;
    std::uint64_t rows_ = 0;
    std::int64_t last_a_ = -1, last_b_ = -1;
    std::int64_t next_pos_ = 0;              // offsets sections: next position to write
    std::int64_t running_ = 0;               // offsets sections: prefix sum so far
};

// The 128 header bytes, with the checksum made from the parts of all sections.
SnapshotHeader finish_header(const SnapshotHeader &header, const std::vector<std::uint64_t> &section_checksums);

} // namespace vgraph

#endif
