# vertica-graph-udx (vgraph)

A Vertica C++ UDx library for graph traversal on tables Vertica already
stores: k-hop neighbourhoods, shortest paths, connected components, PageRank.

Status: early development. The list below says what works and what is planned.
Today `gkhop` builds the graph from the edge rows of every query (inline mode).
That is correct but slow on large tables. The stored snapshot that makes it
fast is the next milestone.

| Function      | Purpose                                  | Status  |
|---------------|------------------------------------------|---------|
| `gversion`    | library version, format version, flags   | works   |
| `gkhop`       | k-hop neighbourhood (BFS)                | works, inline mode only |
| `gpath`       | shortest path                            | planned |
| `gcomponents` | connected components                     | planned |
| `gpagerank`   | PageRank                                 | planned |
| `gbuild`, `gload`, `ginfo` | snapshot build, load, status | planned |

## Requirements

- Vertica 26.x with the C++ SDK in `/opt/vertica/sdk`.
- g++ with C++17 support and GNU make, on a Vertica node.
- Builds on aarch64 and x86_64. Tested on Rocky Linux 9, aarch64, g++ 11.5.

## Install

Run on a Vertica node, as a user who may create libraries:

    make
    make test
    make deploy              # fenced mode, the default
    make deploy FENCED=no    # unfenced, faster, less isolated
    make undeploy            # removes the library and its functions

The scripts call `vsql`. Connection settings come from the environment:
`VSQL_HOST`, `VSQL_PORT`, `VSQL_USER`, `VSQL_PASSWORD`, `VSQL_DATABASE`.
Nothing is stored in this repository.

Every script that changes the database accepts `--echo_only`. It prints the
commands and changes nothing:

    scripts/deploy.sh --echo_only

## Usage

    SELECT gversion() OVER();

     library_version | format_version |            build_flags
    -----------------+----------------+-----------------------------------
     0.1.0           |              1 | -O2 -std=c++17 aarch64 g++-11.5.0

### gkhop, inline mode

Input columns are `(start, target, src, dst, op, epoch, snapshot_epoch)`, all
INT. Edge rows set `src` and `dst`. Request rows set `start`. Every stored row
is one directed edge.

    SELECT gkhop(start, target, src, dst, op, epoch, snapshot_epoch
                 USING PARAMETERS depth=3) OVER()
    FROM (SELECT NULL::INT AS start, NULL::INT AS target, src_id AS src, dst_id AS dst,
                 NULL::INT AS op, NULL::INT AS epoch, NULL::INT AS snapshot_epoch
          FROM graph_demo.contact
          UNION ALL SELECT 1, NULL, NULL, NULL, NULL, NULL, NULL) q;

Output is `(start, node, hops)`. Each node appears once, at its smallest hop
count. `hops` 0 is the start node itself. If you count the start person as
level 1, then level = hops + 1.

Parameters: `depth` (required), `exact` (only nodes at exactly `depth` hops),
`direction` (`out`, `in`, `both`; default `out`), `max_results`, and `start`
as an alternative to a request row.

## Tests

    make test                              # engine unit tests, no Vertica needed
    tests/sql/test_khop_reference.sh       # gkhop against the BFS procedure of the demo repo

The integration test needs https://github.com/mogomo/vertica-graphs-and-trees
cloned as a sibling directory. It drops and recreates schema `GRAPH_DEMO`.

## Not supported

- `op = -1` (journaled deletes) in inline mode.

- Node ids other than Vertica INT (int64). Keep business keys in your own
  nodes table.
- Big-endian hosts.

## License

MIT. Author: Mo (github.com/mogomo).
