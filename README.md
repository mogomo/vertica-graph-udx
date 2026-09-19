# vertica-graph-udx (vgraph)

A Vertica C++ UDx library for graph traversal on tables Vertica already
stores: k-hop neighbourhoods, shortest paths, connected components, PageRank.

Status: early development. Snapshot build, load and the four query functions
work. Automatic refresh and exact results between refreshes (journal, delta
view, procedures) are the next milestone. Until then you rebuild by hand.

| Function      | Purpose                                         | Status |
|---------------|-------------------------------------------------|--------|
| `gversion`    | library version, format version, build flags    | works  |
| `gbuild`      | build a snapshot (CSR index) from an edge table | works  |
| `gload`       | write the snapshot cache file on every node     | works  |
| `ginfo`       | what each node has cached                       | works  |
| `gnode`       | helper: one probe row per node, used with gload | works  |
| `gkhop`       | k-hop neighbourhood (BFS)                       | works  |
| `gpath`       | shortest path, by hops or by weight             | works  |
| `gcomponents` | connected components                            | works  |
| `gpagerank`   | PageRank                                        | works  |
| `vgraph.refresh` and the other procedures | scheduled refresh, exactness between refreshes | planned |

## Requirements

- Vertica 26.x with the C++ SDK in `/opt/vertica/sdk`.
- g++ with C++17 support and GNU make, on a Vertica node.
- Builds on x86_64 and aarch64. Tested on RHEL 8.10 x86_64 with g++ 8.5 (3-node
  Eon cluster) and on Rocky Linux 9 aarch64 with g++ 11.5 (single node).
- Works on single-node and multi-node databases.

## Install

Step by step, with versions and expected output: [docs/build-x86.md](docs/build-x86.md).
Short form:

Run on a Vertica node, as a user who may create libraries:

    make
    make test
    make deploy              # fenced mode, the default
    make deploy FENCED=no    # unfenced, faster, less isolated
    make undeploy            # removes the library and its functions; schema vgraph stays

The scripts call `vsql`. Connection settings come from the environment:
`VSQL_HOST`, `VSQL_PORT`, `VSQL_USER`, `VSQL_PASSWORD`, `VSQL_DATABASE`.
Nothing is stored in this repository.

Every script that changes the database accepts `--echo_only`. It prints the
commands and changes nothing:

    scripts/deploy.sh --echo_only

## Usage

The examples use a table `graph_demo.contact(src_id INT, dst_id INT)`.

### 1. Build and load a snapshot

    INSERT INTO vgraph.snapshot
    SELECT 'demo', 1, chunk_no, chunk FROM (
      SELECT vgraph.gbuild(src, dst, weight, max_epoch
                    USING PARAMETERS graph='demo', directed=true) OVER(ORDER BY src, dst)
      FROM (SELECT src_id AS src, dst_id AS dst, NULL::FLOAT AS weight,
                   (SELECT MAX(epoch) FROM graph_demo.contact) AS max_epoch
            FROM graph_demo.contact) e) b;
    COMMIT;

    SELECT vgraph.gload(chunk_no, chunk USING PARAMETERS graph='demo', snapshot_id=1)
           OVER(PARTITION NODES)
    FROM (SELECT s.chunk_no, s.chunk
          FROM vgraph.snapshot s CROSS JOIN vgraph.probe p
          WHERE s.graph = 'demo' AND s.snapshot_id = 1
            AND p.k IN (SELECT k FROM (SELECT vgraph.gnode(k) OVER(PARTITION NODES)
                                       FROM vgraph.probe) n)) c;

    SELECT vgraph.ginfo() OVER(PARTITION NODES) FROM vgraph.probe;

`directed=true` means every stored row is one directed edge. A table that
stores both directions of a contact needs nothing else. `directed=false` means
one row per undirected edge; gbuild adds the reverse.

The cache file goes to `/tmp/vgraph/<graph>/`. Change it per call with the
parameter `cache_dir`, or per session:

    ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '/data/vgraph';

gload returns one row per node. It works the same on one node and on a
cluster: `vgraph.probe` is a small segmented table with rows on every node,
`vgraph.gnode` picks one of its rows per node, and the join gives every node
one full copy of the chunks. (Reading `vgraph.snapshot` alone would load one
node only, because Vertica reads an unsegmented table on a single node.)

gload can run again at any time, for example after a node restart.

### 2. Query

The functions live in schema `vgraph`. Write `vgraph.gkhop(...)`, or run
`SET SEARCH_PATH TO public, vgraph;` and drop the prefix.

All query functions take the columns `(start, target, src, dst, op, epoch,
snapshot_epoch)`, all INT. Today only `start` and `target` are used with a
snapshot; pass NULL for the rest. The functions need at least one input row.

    -- everyone within 3 hops of person 1
    SELECT vgraph.gkhop(1, NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT
                 USING PARAMETERS graph='demo', depth=3) OVER()
    FROM dual;

    -- shortest path from 1 to 4711
    SELECT vgraph.gpath(1, 4711, NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT
                 USING PARAMETERS graph='demo') OVER()
    FROM dual;

    SELECT vgraph.gcomponents(NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT
                       USING PARAMETERS graph='demo') OVER() FROM dual;

    SELECT vgraph.gpagerank(NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT
                     USING PARAMETERS graph='demo', iterations=20, damping=0.85) OVER() FROM dual;

Several request rows in one call are fine: every row with `start` set is one
request.

- `gkhop` returns `(start, node, hops)`. Each node appears once, at its
  smallest hop count. `hops` 0 is the start node itself. If you count the
  start person as level 1, then level = hops + 1.
  Parameters: `depth` (required), `exact` (only nodes at exactly `depth`
  hops), `direction` (`out`, `in`, `both`; default `out`), `max_results`,
  `start` (instead of a request row).
- `gpath` returns `(start, target, hop_no, node)`, `hop_no` 0 is the start.
  No rows if there is no path. Parameters: `max_depth`, `direction`,
  `weighted` (Dijkstra on the weight column given to gbuild; weights must not
  be negative), `start` and `target`.
- `gcomponents` returns `(node, component)`. `component` is the smallest node
  id in it. Direction is ignored.
- `gpagerank` returns `(node, rank)`. Ranks sum to 1. Parameters:
  `iterations` (20), `damping` (0.85).

Without the `graph` parameter a query function builds the graph from the edge
rows of its input (rows with `src` and `dst` set). This needs no snapshot, but
it reads the whole table on every call. It is meant for tests and small tables.

### First measurements

100 million edge rows, 16.7 million nodes, one node (8 cores, 15 GB, aarch64),
Vertica 26.2. Server time from `v_monitor.query_requests`.

| Step                                              | fenced  | unfenced |
|---------------------------------------------------|--------:|---------:|
| gbuild into `vgraph.snapshot` (once per refresh)  | 38.5 s  | 32.7 s   |
| gload                                             | 2.9 s   | 3.0 s    |
| gkhop depth 9 from person 1 (5.55 million nodes)  | 0.93 s  | 0.74 s   |
| gkhop depth 2 (44 nodes)                          | 6 ms    | 3 ms     |
| same depth 9 question with the SQL BFS procedure of the demo repository | 2.45 s | |

## Tests

    make test                              # engine unit tests, no Vertica needed
    make bench                             # engine build and BFS timings, no Vertica needed
    tests/sql/test_khop_reference.sh       # inline gkhop against the BFS procedure of the demo repo
    tests/sql/test_snapshot.sh             # build, load, all query functions, cache rules

The integration tests need https://github.com/mogomo/vertica-graphs-and-trees
cloned as a sibling directory. They drop and recreate schema `GRAPH_DEMO` (use `--keep` to reuse it).

## Not supported

- Changes to the edge table after gbuild are not seen until the next gbuild
  and gload. (The next milestone removes this limit.)
- More than 2^32 - 2 nodes in one graph.
- Parallel execution of one query over several nodes.
- Node ids other than Vertica INT (int64). Keep business keys in your own
  nodes table.
- Big-endian hosts.

## License

MIT. Author: Mo (github.com/mogomo).
