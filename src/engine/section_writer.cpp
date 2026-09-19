#include "section_writer.h"
#include "version.h"

#include <cstring>
#include <stdexcept>

namespace vgraph {

namespace {
const char *const NAMES[] = {"ids", "out_offsets", "out_nbrs", "in_offsets", "in_nbrs", "out_weights", "in_weights"};
}

bool parse_section(const std::string &name, Section &section)
{
    for (int i = 0; i < 7; ++i)
        if (name == NAMES[i]) { section = static_cast<Section>(i); return true; }
    return false;
}

SnapshotHeader make_header(std::uint64_t node_count, std::uint64_t edge_count, std::uint32_t flags, std::int64_t max_ver)
{
    SnapshotHeader h;
    std::memset(&h, 0, sizeof(h));
    std::memcpy(h.magic, SNAPSHOT_MAGIC, sizeof(h.magic));
    h.format_version = static_cast<std::uint32_t>(FORMAT_VERSION);
    h.flags = flags;
    h.node_count = node_count;
    h.edge_count = edge_count;
    h.max_ver = max_ver;
    snapshot_layout(h);
    return h;
}

SnapshotHeader finish_header(const SnapshotHeader &header, const std::vector<std::uint64_t> &section_checksums)
{
    SnapshotHeader h = header;
    h.checksum = 0;
    std::uint64_t sum = snapshot_checksum(reinterpret_cast<const std::uint8_t *>(&h), sizeof(h));
    for (std::uint64_t part : section_checksums) sum ^= part;
    h.checksum = sum;
    return h;
}

SectionWriter::SectionWriter(const SnapshotHeader &header, Section section, std::uint64_t piece_bytes, Sink sink)
    : h_(header), section_(section), piece_bytes_(piece_bytes & ~std::uint64_t(7)), sink_(std::move(sink))
{
    const std::uint64_t n = h_.node_count, e = h_.edge_count;
    std::uint64_t start = 0, bytes = 0;
    switch (section_) {
        case Section::Ids:        start = h_.off_ids;         bytes = n * 8; break;
        case Section::OutOffsets: start = h_.off_out_offsets; bytes = (n + 1) * 8; break;
        case Section::OutNbrs:    start = h_.off_out_nbrs;    bytes = e * 4; break;
        case Section::InOffsets:  start = h_.off_in_offsets;  bytes = (n + 1) * 8; break;
        case Section::InNbrs:     start = h_.off_in_nbrs;     bytes = e * 4; break;
        case Section::OutWeights: start = h_.off_out_weights; bytes = e * 4; break;
        case Section::InWeights:  start = h_.off_in_weights;  bytes = e * 4; break;
    }
    if (start == 0) fail("a snapshot with these flags has no such section");
    if (piece_bytes_ < 8) fail("piece size too small");
    file_offset_ = start;
    section_end_ = start + bytes;
    buffer_.assign(piece_bytes_ / 8, 0);
}

void SectionWriter::fail(const std::string &why) const
{
    throw std::runtime_error(std::string("section ") + NAMES[static_cast<int>(section_)] + ": " + why);
}

void SectionWriter::put(const void *value, std::size_t size)
{
    std::memcpy(reinterpret_cast<char *>(buffer_.data()) + fill_, value, size);
    fill_ += size;
    if (fill_ == piece_bytes_) {
        checksum_ ^= snapshot_checksum_part(reinterpret_cast<const std::uint8_t *>(buffer_.data()), fill_, file_offset_ / 8);
        sink_(file_offset_, reinterpret_cast<const char *>(buffer_.data()), fill_);
        file_offset_ += fill_;
        fill_ = 0;
        std::fill(buffer_.begin(), buffer_.end(), 0);
    }
}

// Offsets of all positions up to and including pos: each is the prefix sum so far.
void SectionWriter::put_offsets_until(std::int64_t pos)
{
    for (; next_pos_ <= pos; ++next_pos_) put(&running_, 8);
}

void SectionWriter::add(std::int64_t a, std::int64_t b, float w)
{
    const std::int64_t n = static_cast<std::int64_t>(h_.node_count);
    if (a < 0 || a >= n) fail("position " + std::to_string(a) + " is outside 0.." + std::to_string(n - 1));
    if (a < last_a_ || (a == last_a_ && b <= last_b_ && section_ != Section::Ids))
        fail("rows are not sorted or not unique at (" + std::to_string(a) + ", " + std::to_string(b) + ")");

    switch (section_) {
        case Section::Ids:
            if (a != static_cast<std::int64_t>(rows_)) fail("position " + std::to_string(rows_) + " is missing");
            if (rows_ > 0 && b <= last_b_) fail("node ids are not increasing at position " + std::to_string(a));
            put(&b, 8);
            break;
        case Section::OutOffsets:
        case Section::InOffsets:
            if (a == last_a_) fail("two rows for position " + std::to_string(a));
            if (b <= 0) fail("degree of position " + std::to_string(a) + " must be positive");
            put_offsets_until(a);           // offset of a = edges before a
            running_ += b;
            break;
        case Section::OutNbrs:
        case Section::InNbrs: {
            if (b < 0 || b >= n) fail("position " + std::to_string(b) + " is outside 0.." + std::to_string(n - 1));
            const pos_t p = static_cast<pos_t>(b);
            put(&p, 4);
            break;
        }
        case Section::OutWeights:
        case Section::InWeights:
            put(&w, 4);
            break;
    }
    last_a_ = a;
    last_b_ = b;
    ++rows_;
}

std::uint64_t SectionWriter::finish()
{
    const std::uint64_t n = h_.node_count, e = h_.edge_count;
    switch (section_) {
        case Section::Ids:
            if (rows_ != n) fail(std::to_string(rows_) + " rows for " + std::to_string(n) + " nodes");
            break;
        case Section::OutOffsets:
        case Section::InOffsets:
            put_offsets_until(static_cast<std::int64_t>(n));        // the rest, and the final total
            if (running_ != static_cast<std::int64_t>(e))
                fail("degrees add up to " + std::to_string(running_) + ", not to " + std::to_string(e) + " edges");
            break;
        default:
            if (rows_ != e) fail(std::to_string(rows_) + " rows for " + std::to_string(e) + " edges");
    }
    if (file_offset_ + fill_ != section_end_) fail("wrong section length");
    if (fill_ > 0) {
        const std::uint64_t padded = (fill_ + 7) & ~std::uint64_t(7);      // alignment padding is 0 already
        checksum_ ^= snapshot_checksum_part(reinterpret_cast<const std::uint8_t *>(buffer_.data()), padded, file_offset_ / 8);
        sink_(file_offset_, reinterpret_cast<const char *>(buffer_.data()), padded);
        fill_ = 0;
    }
    return checksum_;
}

} // namespace vgraph
