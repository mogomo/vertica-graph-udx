// vgraph engine: snapshot binary format. See docs/format.md.
// Little-endian only. Every section starts on an 8-byte boundary.
#ifndef VGRAPH_ENGINE_SNAPSHOT_H
#define VGRAPH_ENGINE_SNAPSHOT_H

#include "csr.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vgraph snapshots are little-endian only"
#endif

namespace vgraph {

constexpr char SNAPSHOT_MAGIC[8] = {'V', 'G', 'R', 'A', 'P', 'H', 'S', '1'};

constexpr std::uint32_t FLAG_DIRECTED = 1u;      // every stored row is one directed edge
constexpr std::uint32_t FLAG_WEIGHTED = 2u;      // weight sections are stored
constexpr std::uint32_t FLAG_IN_EQUALS_OUT = 4u; // directed, but the in lists equal the out lists
                                                 // (the table stores both directions): no reverse CSR
// The reverse CSR is stored only if FLAG_DIRECTED is set and FLAG_IN_EQUALS_OUT is not.

// 128 bytes. Section offsets are from the start of the file; 0 = section absent.
struct SnapshotHeader {
    char magic[8];
    std::uint32_t format_version;
    std::uint32_t flags;
    std::uint64_t node_count;
    std::uint64_t edge_count;
    std::int64_t max_ver;
    std::uint64_t checksum;      // over the whole file, with this field as 0
    std::uint64_t total_bytes;   // multiple of 8
    std::uint64_t off_ids;
    std::uint64_t off_out_offsets;
    std::uint64_t off_out_nbrs;
    std::uint64_t off_in_offsets;
    std::uint64_t off_in_nbrs;
    std::uint64_t off_out_weights;
    std::uint64_t off_in_weights;
    std::uint64_t reserved[2];
};
static_assert(sizeof(SnapshotHeader) == 128, "snapshot header must be 128 bytes");

// Owning, 8-byte aligned snapshot bytes.
class SnapshotBuffer {
public:
    void allocate(std::uint64_t bytes) { words_.assign(bytes / 8, 0); }
    void shrink(std::uint64_t bytes) { words_.resize(bytes / 8); }
    std::uint8_t *data() { return reinterpret_cast<std::uint8_t *>(words_.data()); }
    const std::uint8_t *data() const { return reinterpret_cast<const std::uint8_t *>(words_.data()); }
    std::uint64_t size() const { return words_.size() * 8; }
private:
    std::vector<std::uint64_t> words_;
};

// Fills the section offsets and total_bytes of h from its counts and flags.
void snapshot_layout(SnapshotHeader &h);

// Checksum of a complete snapshot. The stored checksum field counts as 0.
std::uint64_t snapshot_checksum(const std::uint8_t *data, std::uint64_t size);

// The checksum is the XOR of one value per 8-byte word, mixed with the word's position in the file.
// So the checksum of a file is the XOR of the checksums of its parts, in any order: sections that
// are written by separate statements can be summed up afterwards. A zero word contributes nothing.
// first_word = file offset of data / 8. size is a multiple of 8.
std::uint64_t snapshot_checksum_part(const std::uint8_t *data, std::uint64_t size, std::uint64_t first_word);

// Validates the bytes and returns a view on them. data must be 8-byte aligned
// and must outlive the view. Throws std::runtime_error with the cause.
// verify_checksum reads the whole file: use it in gload, not in every query.
Csr snapshot_open(const std::uint8_t *data, std::uint64_t size, bool verify_checksum);

// True if the bytes start with the vgraph magic. Used before deleting cache files.
bool snapshot_has_magic(const std::uint8_t *data, std::uint64_t size);

} // namespace vgraph

#endif
