#include "builder.h"
#include "id_index.h"
#include "version.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace vgraph {

GraphBuilder::GraphBuilder(bool directed, bool weighted) : directed_(directed), weighted_(weighted) {}

void GraphBuilder::push(std::int64_t src, std::int64_t dst, float weight)
{
    if (chunks_.empty() || chunks_.back().n == CHUNK_EDGES) {
        Chunk c;
        c.sd.reset(new std::int64_t[CHUNK_EDGES * 2]);
        if (weighted_) c.w.reset(new float[CHUNK_EDGES]);
        chunks_.push_back(std::move(c));
    }
    Chunk &c = chunks_.back();
    c.sd[c.n * 2] = src;
    c.sd[c.n * 2 + 1] = dst;
    if (weighted_) c.w[c.n] = weight;
    ++c.n;
    ++raw_count_;
}

void GraphBuilder::add_edge(std::int64_t src, std::int64_t dst, float weight)
{
    push(src, dst, weight);
    if (!directed_ && src != dst) push(dst, src, weight);
}

void GraphBuilder::enable_weights()
{
    if (weighted_) return;
    weighted_ = true;
    for (Chunk &c : chunks_) {
        c.w.reset(new float[CHUNK_EDGES]);
        std::fill(c.w.get(), c.w.get() + c.n, 1.0f);
    }
}

void GraphBuilder::add_node(std::int64_t id) { extra_nodes_.push_back(id); }

// ids := sorted union of ids and block. block is consumed.
void GraphBuilder::merge_ids(std::vector<std::int64_t> &ids, std::vector<std::int64_t> &block)
{
    std::sort(block.begin(), block.end());
    block.erase(std::unique(block.begin(), block.end()), block.end());
    std::vector<std::int64_t> fresh;
    std::set_difference(block.begin(), block.end(), ids.begin(), ids.end(), std::back_inserter(fresh));
    block.clear();
    if (fresh.empty()) return;
    std::vector<std::int64_t> merged(ids.size() + fresh.size());
    std::merge(ids.begin(), ids.end(), fresh.begin(), fresh.end(), merged.begin());
    ids.swap(merged);
}

namespace {
struct KeyW {
    std::uint64_t key;
    float w;
};
double seconds_since(std::chrono::steady_clock::time_point &t)
{
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - t).count();
    t = now;
    return s;
}
} // namespace

void GraphBuilder::finish(std::int64_t max_ver, SnapshotBuffer &out)
{
    auto clock = std::chrono::steady_clock::now();

    // 1. Node ids: sorted union of all endpoints. Ids not yet known are collected
    //    in a block; a full block is merged into ids and the index is rebuilt.
    std::vector<std::int64_t> ids;
    IdIndex index;
    {
        std::vector<std::int64_t> block;
        auto flush = [&]() {
            merge_ids(ids, block);
            index.build(ids.data(), ids.size());
        };
        block.swap(extra_nodes_);
        flush();
        for (const Chunk &c : chunks_) {
            for (std::size_t i = 0; i < c.n * 2; ++i) {
                // Same src as the row before: already handled.
                if ((i & 1) == 0 && i >= 2 && c.sd[i] == c.sd[i - 2]) continue;
                if (index.find(c.sd[i]) == NO_POS) {
                    block.push_back(c.sd[i]);
                    if (block.size() >= ID_BLOCK) flush();
                }
            }
        }
        flush();
    }
    if (ids.size() >= NO_POS) throw std::runtime_error("graph has more nodes than fit uint32");
    const std::uint64_t n = ids.size();
    timings_.ids = seconds_since(clock);

    // 2. Edges as keys (src position << 32 | dst position). Raw chunks are freed on the way.
    std::vector<std::uint64_t> keys;
    std::vector<float> weights;
    keys.reserve(raw_count_);
    if (weighted_) weights.reserve(raw_count_);
    for (Chunk &c : chunks_) {
        std::int64_t last_src = 0;
        pos_t last_pos = NO_POS;
        for (std::size_t i = 0; i < c.n; ++i) {
            const std::int64_t s = c.sd[i * 2], d = c.sd[i * 2 + 1];
            if (last_pos == NO_POS || s != last_src) { last_src = s; last_pos = index.find(s); }
            keys.push_back((static_cast<std::uint64_t>(last_pos) << 32) | index.find(d));
        }
        if (weighted_) weights.insert(weights.end(), c.w.get(), c.w.get() + c.n);
        c.sd.reset();
        c.w.reset();
    }
    std::vector<Chunk>().swap(chunks_);
    raw_count_ = 0;
    timings_.keys = seconds_since(clock);

    // 3. Sort and remove duplicates, unless the input came sorted and unique (gbuild does).
    bool strictly_sorted = true;
    for (std::size_t i = 1; i < keys.size() && strictly_sorted; ++i)
        strictly_sorted = keys[i - 1] < keys[i];
    if (!strictly_sorted) {
        if (!weighted_) {
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        } else {
            std::vector<KeyW> kw(keys.size());
            for (std::size_t i = 0; i < keys.size(); ++i) kw[i] = KeyW{keys[i], weights[i]};
            std::stable_sort(kw.begin(), kw.end(), [](const KeyW &a, const KeyW &b) { return a.key < b.key; });
            kw.erase(std::unique(kw.begin(), kw.end(), [](const KeyW &a, const KeyW &b) { return a.key == b.key; }),
                     kw.end());
            keys.resize(kw.size());
            weights.resize(kw.size());
            for (std::size_t i = 0; i < kw.size(); ++i) { keys[i] = kw[i].key; weights[i] = kw[i].w; }
        }
    }
    const std::uint64_t e = keys.size();
    timings_.sort = seconds_since(clock);

    // 4. Write the snapshot.
    SnapshotHeader h;
    std::memset(&h, 0, sizeof(h));
    std::memcpy(h.magic, SNAPSHOT_MAGIC, sizeof(h.magic));
    h.format_version = static_cast<std::uint32_t>(FORMAT_VERSION);
    h.flags = (directed_ ? FLAG_DIRECTED : 0u) | (weighted_ ? FLAG_WEIGHTED : 0u);
    h.node_count = n;
    h.edge_count = e;
    h.max_ver = max_ver;
    snapshot_layout(h);
    out.allocate(h.total_bytes);
    std::uint8_t *base = out.data();

    if (n) std::memcpy(base + h.off_ids, ids.data(), n * 8);
    std::vector<std::int64_t>().swap(ids);

    std::int64_t *out_off = reinterpret_cast<std::int64_t *>(base + h.off_out_offsets);
    pos_t *out_nbr = reinterpret_cast<pos_t *>(base + h.off_out_nbrs);
    for (std::uint64_t i = 0; i < e; ++i) {
        ++out_off[(keys[i] >> 32) + 1];
        out_nbr[i] = static_cast<pos_t>(keys[i] & 0xFFFFFFFFu);
    }
    for (std::uint64_t p = 0; p < n; ++p) out_off[p + 1] += out_off[p];
    if (weighted_ && e) std::memcpy(base + h.off_out_weights, weights.data(), e * 4);

    if (directed_) {
        // Reverse CSR. Keys are sorted by src, so every in list comes out sorted.
        std::int64_t *in_off = reinterpret_cast<std::int64_t *>(base + h.off_in_offsets);
        pos_t *in_nbr = reinterpret_cast<pos_t *>(base + h.off_in_nbrs);
        float *in_w = weighted_ ? reinterpret_cast<float *>(base + h.off_in_weights) : nullptr;
        for (std::uint64_t i = 0; i < e; ++i) ++in_off[(keys[i] & 0xFFFFFFFFu) + 1];
        for (std::uint64_t p = 0; p < n; ++p) in_off[p + 1] += in_off[p];
        std::vector<std::int64_t> cursor(in_off, in_off + n);
        for (std::uint64_t i = 0; i < e; ++i) {
            const std::int64_t at = cursor[keys[i] & 0xFFFFFFFFu]++;
            in_nbr[at] = static_cast<pos_t>(keys[i] >> 32);
            if (in_w) in_w[at] = weights[i];
        }
    }

    h.checksum = 0;
    std::memcpy(base, &h, sizeof(h));
    h.checksum = snapshot_checksum(base, h.total_bytes);
    std::memcpy(base, &h, sizeof(h));
    timings_.write = seconds_since(clock);
}

} // namespace vgraph
