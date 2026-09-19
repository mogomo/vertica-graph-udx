// vgraph engine: delta overlay. A snapshot plus the edge changes committed
// after it, seen as one graph. Same traversal interface as CsrGraph (csr.h),
// so bfs, path, cc and pagerank run on it unchanged.
//
// Apply the journal rows in epoch order: the last op for an edge wins.
// Applying the same rows again changes nothing (idempotent), so rows that are
// already contained in the snapshot do no harm.
//
// New nodes get positions >= the snapshot's node_count, in order of arrival.
// Memory grows with the number of changes, not with the graph.
#ifndef VGRAPH_ENGINE_DELTA_H
#define VGRAPH_ENGINE_DELTA_H

#include "csr.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vgraph {

class Overlay {
public:
    explicit Overlay(const Csr &base) : base_(base), c_(base_.csr()) {}
    Overlay(const Overlay &) = delete;
    Overlay &operator=(const Overlay &) = delete;

    // op +1 = edge added, -1 = edge deleted. On an undirected graph (built with
    // directed=false) the reverse edge follows automatically.
    void apply(std::int64_t src, std::int64_t dst, int op, float weight = 1.0f)
    {
        if (op > 0) {
            const pos_t s = position_or_new(src), d = position_or_new(dst);
            add(s, d, weight);
            if (!c_.directed && s != d) add(d, s, weight);
        } else {
            pos_t s, d;
            if (!find(src, s) || !find(dst, d)) return;      // unknown node: nothing to delete
            remove(s, d);
            if (!c_.directed && s != d) remove(d, s);
        }
    }

    bool empty() const { return new_ids_.empty() && added_out_.empty() && deleted_.empty(); }
    std::size_t added_edges() const { return added_count_; }
    std::size_t deleted_edges() const { return deleted_.size(); }
    std::size_t new_nodes() const { return new_ids_.size(); }

    // ---- traversal interface, as in CsrGraph
    pos_t node_count() const { return static_cast<pos_t>(c_.node_count + new_ids_.size()); }
    std::int64_t id_of(pos_t p) const { return p < c_.node_count ? c_.ids[p] : new_ids_[p - c_.node_count]; }
    bool weighted() const { return c_.weighted; }

    bool find(std::int64_t id, pos_t &p) const
    {
        if (base_.find(id, p)) return true;
        auto it = new_pos_.find(id);
        if (it == new_pos_.end()) return false;
        p = it->second;
        return true;
    }

    template <class F> void for_out(pos_t p, F &&f) const { walk(p, true, f); }
    template <class F> void for_in(pos_t p, F &&f) const { walk(p, c_.directed ? false : true, f); }
    template <class F> void for_dir(pos_t p, Direction d, F &&f) const
    {
        if (d != Direction::In) for_out(p, f);
        if (d == Direction::In || (d == Direction::Both && c_.directed)) for_in(p, f);
    }

private:
    using Added = std::unordered_map<pos_t, std::vector<std::pair<pos_t, float>>>;

    static std::uint64_t key(pos_t s, pos_t d) { return (static_cast<std::uint64_t>(s) << 32) | d; }

    pos_t position_or_new(std::int64_t id)
    {
        pos_t p;
        if (find(id, p)) return p;
        p = node_count();
        new_ids_.push_back(id);
        new_pos_[id] = p;
        return p;
    }

    // Is s -> d an edge of the snapshot? Out lists are sorted.
    bool in_base(pos_t s, pos_t d) const
    {
        if (s >= c_.node_count || d >= c_.node_count) return false;
        const pos_t *lo = c_.out_nbrs + c_.out_offsets[s], *hi = c_.out_nbrs + c_.out_offsets[s + 1];
        return std::binary_search(lo, hi, d);
    }

    static bool set_in_list(Added &lists, pos_t from, pos_t to, float w)
    {
        auto &list = lists[from];
        for (auto &e : list)
            if (e.first == to) { e.second = w; return false; }
        list.push_back(std::make_pair(to, w));
        return true;
    }
    static bool drop_from_list(Added &lists, pos_t from, pos_t to)
    {
        auto it = lists.find(from);
        if (it == lists.end()) return false;
        auto &list = it->second;
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].first == to) {
                list[i] = list.back();
                list.pop_back();
                if (list.empty()) lists.erase(it);
                return true;
            }
        return false;
    }

    void add(pos_t s, pos_t d, float w)
    {
        if (in_base(s, d)) {            // back to the snapshot's edge (and its weight)
            if (deleted_.erase(key(s, d))) { touched_out_.insert(s); touched_in_.insert(d); }
            return;
        }
        if (set_in_list(added_out_, s, d, w)) ++added_count_;
        if (c_.directed) set_in_list(added_in_, d, s, w);
    }

    void remove(pos_t s, pos_t d)
    {
        if (in_base(s, d)) {
            deleted_.insert(key(s, d));
            touched_out_.insert(s);
            touched_in_.insert(d);
            return;
        }
        if (drop_from_list(added_out_, s, d)) --added_count_;
        if (c_.directed) drop_from_list(added_in_, d, s);
    }

    template <class F> void walk(pos_t p, bool out, F &f) const
    {
        if (p < c_.node_count) {
            const std::int64_t *off = out ? c_.out_offsets : c_.in_offsets;
            const pos_t *nbr = out ? c_.out_nbrs : c_.in_nbrs;
            const float *w = out ? c_.out_weights : c_.in_weights;
            // Only nodes that lost an edge pay for the deleted-set lookups.
            const bool check = !deleted_.empty() && (out ? touched_out_ : touched_in_).count(p) != 0;
            for (std::int64_t i = off[p], e = off[p + 1]; i < e; ++i) {
                if (check && deleted_.count(out ? key(p, nbr[i]) : key(nbr[i], p))) continue;
                f(nbr[i], w ? w[i] : 1.0f);
            }
        }
        const Added &added = out ? added_out_ : added_in_;
        if (added.empty()) return;
        auto it = added.find(p);
        if (it == added.end()) return;
        for (const auto &e : it->second) f(e.first, e.second);
    }

    CsrGraph base_;
    const Csr &c_;
    std::vector<std::int64_t> new_ids_;
    std::unordered_map<std::int64_t, pos_t> new_pos_;
    Added added_out_, added_in_;
    std::unordered_set<std::uint64_t> deleted_;
    std::unordered_set<pos_t> touched_out_, touched_in_;
    std::size_t added_count_ = 0;
};

} // namespace vgraph

#endif
