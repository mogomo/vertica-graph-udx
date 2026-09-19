// The streaming section writer must produce exactly the bytes of the in-memory builder.
#include "check.h"

#include "../../src/engine/section_writer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

using namespace vgraph;

// Does what the database does for the streaming build: node map, mapped unique sorted edges,
// degrees. Then writes every section in a scrambled order and assembles the file.
static void build_streamed(const TestGraph &reference, std::vector<std::uint64_t> &file, std::uint64_t piece)
{
    const Csr &c = reference.csr;
    CsrGraph g(c);
    struct E { std::int64_t s, d; float w; };
    std::vector<E> out, in;
    for (pos_t p = 0; p < g.node_count(); ++p) {
        g.for_out(p, [&](pos_t v, float w) { out.push_back(E{p, v, w}); });
    }
    in = out;
    std::sort(in.begin(), in.end(), [](const E &x, const E &y) { return x.d != y.d ? x.d < y.d : x.s < y.s; });

    std::uint32_t flags = (c.directed ? FLAG_DIRECTED : 0u) | (c.weighted ? FLAG_WEIGHTED : 0u) |
                          ((c.directed && c.in_is_out) ? FLAG_IN_EQUALS_OUT : 0u);
    SnapshotHeader h = make_header(c.node_count, c.edge_count, flags, c.max_ver);
    file.assign(h.total_bytes / 8, 0);
    auto sink = [&](std::uint64_t off, const char *data, std::uint64_t len) {
        CHECK(off + len <= h.total_bytes && len <= piece);
        std::memcpy(reinterpret_cast<char *>(file.data()) + off, data, len);
    };
    std::vector<std::uint64_t> parts;
    auto degrees = [&](Section sec, const std::vector<E> &edges, bool by_target) {
        SectionWriter w(h, sec, piece, sink);
        for (std::size_t i = 0; i < edges.size();) {
            const std::int64_t key = by_target ? edges[i].d : edges[i].s;
            std::size_t j = i;
            while (j < edges.size() && (by_target ? edges[j].d : edges[j].s) == key) ++j;
            w.add(key, static_cast<std::int64_t>(j - i));
            i = j;
        }
        parts.push_back(w.finish());
    };
    const bool reverse = c.directed && !c.in_is_out;
    // Deliberately not in file order.
    if (reverse) {
        SectionWriter w(h, Section::InNbrs, piece, sink);
        for (const E &e : in) w.add(e.d, e.s);
        parts.push_back(w.finish());
    }
    {
        SectionWriter w(h, Section::OutNbrs, piece, sink);
        for (const E &e : out) w.add(e.s, e.d);
        parts.push_back(w.finish());
    }
    if (c.weighted) {
        SectionWriter w(h, Section::OutWeights, piece, sink);
        for (const E &e : out) w.add(e.s, e.d, e.w);
        parts.push_back(w.finish());
        if (reverse) {
            SectionWriter wi(h, Section::InWeights, piece, sink);
            for (const E &e : in) wi.add(e.d, e.s, e.w);
            parts.push_back(wi.finish());
        }
    }
    degrees(Section::OutOffsets, out, false);
    if (reverse) degrees(Section::InOffsets, in, true);
    {
        SectionWriter w(h, Section::Ids, piece, sink);
        for (pos_t p = 0; p < g.node_count(); ++p) w.add(p, g.id_of(p));
        parts.push_back(w.finish());
    }
    const SnapshotHeader done = finish_header(h, parts);
    std::memcpy(file.data(), &done, sizeof(done));
}

static void same_bytes(const std::vector<Edge> &edges, bool directed, bool weighted, const std::vector<std::int64_t> &nodes,
                       std::uint64_t piece)
{
    TestGraph ref;
    build(ref, edges, directed, weighted, nodes, 4711);
    std::vector<std::uint64_t> file;
    build_streamed(ref, file, piece);
    CHECK(file.size() * 8 == ref.buffer.size());
    CHECK(std::memcmp(file.data(), ref.buffer.data(), ref.buffer.size()) == 0);
    // and it opens with a verified checksum
    bool ok = true;
    try { snapshot_open(reinterpret_cast<const std::uint8_t *>(file.data()), file.size() * 8, true); }
    catch (const std::runtime_error &) { ok = false; }
    CHECK(ok);
}

int main()
{
    Rng rng(5);
    std::vector<Edge> random_edges, mirrored, weighted;
    for (int i = 0; i < 5000; ++i) {
        const std::int64_t s = rng.below(700) * 11 - 300, d = rng.below(700) * 11 - 300;
        random_edges.push_back(Edge(s, d));
        mirrored.push_back(Edge(s, d));
        mirrored.push_back(Edge(d, s));
        weighted.push_back(Edge(s, d, static_cast<float>(rng.below(1000)) / 8.0f));
    }
    for (std::uint64_t piece : {std::uint64_t(8), std::uint64_t(64), std::uint64_t(4096), std::uint64_t(8) << 20}) {
        same_bytes(random_edges, true, false, {100000, -100000}, piece);   // directed, isolated nodes at both ends
        same_bytes(mirrored, true, false, {}, piece);                       // in lists equal out lists
        same_bytes(random_edges, false, false, {}, piece);                  // undirected
        same_bytes(weighted, true, true, {}, piece);                        // weights on both sides
        same_bytes({{1, 2}}, true, false, {}, piece);
    }

    // Bad streams are refused.
    SnapshotHeader h = make_header(3, 2, FLAG_DIRECTED, 0);
    auto nowhere = [](std::uint64_t, const char *, std::uint64_t) {};
    auto refused = [&](Section sec, std::vector<std::pair<std::int64_t, std::int64_t>> rows) {
        try {
            SectionWriter w(h, sec, 4096, nowhere);
            for (auto &r : rows) w.add(r.first, r.second);
            w.finish();
        } catch (const std::runtime_error &) { return true; }
        return false;
    };
    CHECK(!refused(Section::OutNbrs, {{0, 1}, {1, 2}}));
    CHECK(refused(Section::OutNbrs, {{1, 2}, {0, 1}}));          // not sorted
    CHECK(refused(Section::OutNbrs, {{0, 1}, {0, 1}}));          // duplicate edge
    CHECK(refused(Section::OutNbrs, {{0, 1}}));                  // too few rows
    CHECK(refused(Section::OutNbrs, {{0, 1}, {1, 3}}));          // position out of range
    CHECK(refused(Section::Ids, {{0, 5}, {2, 9}}));              // position missing
    CHECK(refused(Section::Ids, {{0, 5}, {1, 5}, {2, 9}}));      // ids not increasing
    CHECK(!refused(Section::OutOffsets, {{0, 1}, {2, 1}}));
    CHECK(refused(Section::OutOffsets, {{0, 1}, {2, 2}}));       // degrees do not add up
    CHECK(refused(Section::OutWeights, {{0, 1}}));               // unweighted snapshot has no such section
    return finish("test_section_writer");
}
