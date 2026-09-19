// vgraph engine: fast lookup from node id to position in a sorted id array.
//
// The id range is cut into at most 2^20 buckets of equal width. The table holds
// the first position of every bucket. A bucket whose ids are all present
// ("dense") needs no look at the id array at all: position = first + offset.
// Other buckets need a binary search over that bucket only.
// Memory: at most 4 MB, whatever the graph size.
#ifndef VGRAPH_ENGINE_ID_INDEX_H
#define VGRAPH_ENGINE_ID_INDEX_H

#include "csr.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace vgraph {

class IdIndex {
public:
    // ids must be sorted and unique, and must stay alive and unchanged.
    void build(const std::int64_t *ids, std::uint64_t n)
    {
        ids_ = ids;
        n_ = n;
        table_.clear();
        if (n == 0) return;
        min_ = ids[0];
        const std::uint64_t range = static_cast<std::uint64_t>(ids[n - 1]) - static_cast<std::uint64_t>(min_);
        shift_ = 0;
        while ((range >> shift_) >= MAX_BUCKETS) ++shift_;
        const std::uint64_t buckets = (range >> shift_) + 1;
        table_.assign(buckets + 1, static_cast<pos_t>(n));
        // table_[b] = first position with bucket >= b. One pass, from the end.
        for (std::uint64_t p = n; p-- > 0;) table_[bucket(ids[p])] = static_cast<pos_t>(p);
        for (std::uint64_t b = buckets; b-- > 0;)
            if (table_[b] > table_[b + 1]) table_[b] = table_[b + 1];
    }

    // Position of id, or NO_POS.
    pos_t find(std::int64_t id) const
    {
        if (n_ == 0 || id < min_ || id > ids_[n_ - 1]) return NO_POS;
        const std::uint64_t b = bucket(id);
        const pos_t lo = table_[b], hi = table_[b + 1];
        if (static_cast<std::uint64_t>(hi - lo) == (std::uint64_t(1) << shift_))      // dense bucket
            return lo + static_cast<pos_t>(offset(id) & ((std::uint64_t(1) << shift_) - 1));
        const std::int64_t *it = std::lower_bound(ids_ + lo, ids_ + hi, id);
        return (it != ids_ + hi && *it == id) ? static_cast<pos_t>(it - ids_) : NO_POS;
    }

private:
    static constexpr std::uint64_t MAX_BUCKETS = 1u << 20;
    std::uint64_t offset(std::int64_t id) const
    { return static_cast<std::uint64_t>(id) - static_cast<std::uint64_t>(min_); }
    std::uint64_t bucket(std::int64_t id) const { return offset(id) >> shift_; }

    const std::int64_t *ids_ = nullptr;
    std::uint64_t n_ = 0;
    std::int64_t min_ = 0;
    unsigned shift_ = 0;
    std::vector<pos_t> table_;
};

} // namespace vgraph

#endif
