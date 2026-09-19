// IdIndex against std::lower_bound on dense, gapped, sparse and extreme id sets.
#include "check.h"

#include "../../src/engine/id_index.h"

#include <limits>

using namespace vgraph;

static void compare(std::vector<std::int64_t> ids, Rng &rng)
{
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    IdIndex index;
    index.build(ids.data(), ids.size());
    auto expect = [&](std::int64_t id) {
        auto it = std::lower_bound(ids.begin(), ids.end(), id);
        return (it != ids.end() && *it == id) ? static_cast<pos_t>(it - ids.begin()) : NO_POS;
    };
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK(index.find(ids[i]) == i);
        if (ids[i] < std::numeric_limits<std::int64_t>::max()) CHECK(index.find(ids[i] + 1) == expect(ids[i] + 1));
        if (ids[i] > std::numeric_limits<std::int64_t>::min()) CHECK(index.find(ids[i] - 1) == expect(ids[i] - 1));
    }
    for (int i = 0; i < 1000; ++i) {
        const std::int64_t id = static_cast<std::int64_t>(rng.next());
        CHECK(index.find(id) == expect(id));
    }
}

int main()
{
    Rng rng(7);
    IdIndex empty;
    empty.build(nullptr, 0);
    CHECK(empty.find(1) == NO_POS);

    compare({5}, rng);
    std::vector<std::int64_t> dense, gapped, sparse, wide;
    for (std::int64_t i = 1; i <= 3000000; ++i) dense.push_back(i);            // more ids than buckets
    for (std::int64_t i = -50000; i <= 50000; ++i) if (i % 7 != 0) gapped.push_back(i);
    for (int i = 0; i < 200000; ++i) sparse.push_back(static_cast<std::int64_t>(rng.next() >> 1));
    for (int i = 0; i < 1000; ++i) wide.push_back(static_cast<std::int64_t>(rng.next()));
    wide.push_back(std::numeric_limits<std::int64_t>::min());
    wide.push_back(std::numeric_limits<std::int64_t>::max());
    compare(dense, rng);
    compare(gapped, rng);
    compare(sparse, rng);
    compare(wide, rng);
    return finish("test_id_index");
}
