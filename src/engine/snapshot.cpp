#include "snapshot.h"
#include "version.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace vgraph {

static std::uint64_t align8(std::uint64_t v) { return (v + 7) & ~std::uint64_t(7); }

void snapshot_layout(SnapshotHeader &h)
{
    const std::uint64_t n = h.node_count, e = h.edge_count;
    const bool directed = (h.flags & FLAG_DIRECTED) != 0 && (h.flags & FLAG_IN_EQUALS_OUT) == 0;   // reverse CSR stored
    const bool weighted = (h.flags & FLAG_WEIGHTED) != 0;

    std::uint64_t at = sizeof(SnapshotHeader);
    auto place = [&at](std::uint64_t bytes) {
        std::uint64_t off = at;
        at = align8(at + bytes);
        return off;
    };
    h.off_ids = place(n * 8);
    h.off_out_offsets = place((n + 1) * 8);
    h.off_out_nbrs = place(e * 4);
    h.off_in_offsets = directed ? place((n + 1) * 8) : 0;
    h.off_in_nbrs = directed ? place(e * 4) : 0;
    h.off_out_weights = weighted ? place(e * 4) : 0;
    h.off_in_weights = (weighted && directed) ? place(e * 4) : 0;
    h.total_bytes = at;
}

std::uint64_t snapshot_checksum(const std::uint8_t *data, std::uint64_t size)
{
    // FNV-1a style mix over 8-byte words. size is a multiple of 8.
    const std::uint64_t checksum_word = offsetof(SnapshotHeader, checksum) / 8;
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::uint64_t i = 0; i < size / 8; ++i) {
        std::uint64_t w = 0;
        if (i != checksum_word) std::memcpy(&w, data + i * 8, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    return h;
}

bool snapshot_has_magic(const std::uint8_t *data, std::uint64_t size)
{
    return size >= sizeof(SNAPSHOT_MAGIC) && std::memcmp(data, SNAPSHOT_MAGIC, sizeof(SNAPSHOT_MAGIC)) == 0;
}

static void fail(const std::string &why) { throw std::runtime_error("bad snapshot: " + why); }

Csr snapshot_open(const std::uint8_t *data, std::uint64_t size, bool verify_checksum)
{
    if (size < sizeof(SnapshotHeader)) fail("shorter than the header");
    if (reinterpret_cast<std::uintptr_t>(data) % 8 != 0) fail("buffer is not 8-byte aligned");
    if (!snapshot_has_magic(data, size)) fail("wrong magic");

    SnapshotHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (h.format_version != static_cast<std::uint32_t>(FORMAT_VERSION))
        fail("format version " + std::to_string(h.format_version) + ", this library reads " +
             std::to_string(FORMAT_VERSION));
    if (h.node_count >= NO_POS) fail("node_count does not fit uint32");

    // The offsets must be exactly what the layout rule gives for these counts.
    SnapshotHeader expect = h;
    snapshot_layout(expect);
    if (std::memcmp(&expect, &h, sizeof(h)) != 0) fail("section offsets do not match the counts");
    if (h.total_bytes != size) fail("size is " + std::to_string(size) + " bytes, header says " +
                                    std::to_string(h.total_bytes));
    if (verify_checksum && snapshot_checksum(data, size) != h.checksum) fail("checksum mismatch");

    Csr c;
    c.node_count = h.node_count;
    c.edge_count = h.edge_count;
    c.max_ver = h.max_ver;
    c.directed = (h.flags & FLAG_DIRECTED) != 0;
    c.weighted = (h.flags & FLAG_WEIGHTED) != 0;
    c.in_is_out = !c.directed || (h.flags & FLAG_IN_EQUALS_OUT) != 0;
    c.ids = reinterpret_cast<const std::int64_t *>(data + h.off_ids);
    c.out_offsets = reinterpret_cast<const std::int64_t *>(data + h.off_out_offsets);
    c.out_nbrs = reinterpret_cast<const pos_t *>(data + h.off_out_nbrs);
    if (!c.in_is_out) {
        c.in_offsets = reinterpret_cast<const std::int64_t *>(data + h.off_in_offsets);
        c.in_nbrs = reinterpret_cast<const pos_t *>(data + h.off_in_nbrs);
    } else {
        c.in_offsets = c.out_offsets;
        c.in_nbrs = c.out_nbrs;
    }
    if (c.weighted) {
        c.out_weights = reinterpret_cast<const float *>(data + h.off_out_weights);
        c.in_weights = c.in_is_out ? c.out_weights : reinterpret_cast<const float *>(data + h.off_in_weights);
    }

    // Cheap structural checks, so a damaged file cannot send a query out of bounds.
    if (c.out_offsets[0] != 0 || c.out_offsets[c.node_count] != static_cast<std::int64_t>(c.edge_count))
        fail("out offsets do not span the edges");
    if (c.in_offsets[0] != 0 || c.in_offsets[c.node_count] != static_cast<std::int64_t>(c.edge_count))
        fail("in offsets do not span the edges");
    return c;
}

} // namespace vgraph
