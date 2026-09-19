# vertica-graph-udx (vgraph)

A Vertica C++ UDx library for graph traversal on tables Vertica already
stores: k-hop neighbourhoods, shortest paths, connected components, PageRank.

The graph index (a snapshot) lives inside Vertica and is rebuilt on a
schedule. Between rebuilds every query merges the snapshot with the edge
changes made after it, so results are always exact.

Works on single-node and multi-node databases (tested: one node on aarch64,
3-node Eon cluster on x86_64).

| Function / procedure        | Purpose |
|-----------------------------|---------|
| `vgraph.gkhop`              | k-hop neighbourhood (BFS) |
| `vgraph.gpath`              | shortest path, by hops or by weight |
| `vgraph.gcomponents`        | connected components |
| `vgraph.gpagerank`          | PageRank |
| `vgraph.register_graph`     | register an edge table as a graph |
| `vgraph.refresh_graph`      | rebuild the snapshot and load it on every node |
| `vgraph.schedule_refresh`   | run refresh_graph on a cron schedule |
| `vgraph.load_all`           | load the active snapshot on every node again (cache repair) |
| `vgraph.status`             | size and read time of the pending changes |
| `vgraph.unregister_graph`   | remove a graph |
| `vgraph.ginfo`, `vgraph.gversion` | what every node has cached; library version |
| `vgraph.gbuild`, `vgraph.gload`, `vgraph.gnode` | building blocks used by the procedures |

## Requirements

- Vertica 26.x with the C++ SDK in `/opt/vertica/sdk`.
- g++ with C++17 support and GNU make, on one Vertica node.
- Builds on x86_64 and aarch64. Tested on RHEL 8.10 x86_64 with g++ 8.5 and on
  Rocky Linux 9 aarch64 with g++ 11.5.

## Install

Step by step, with versions and expected output: [docs/build-x86.md](docs/build-x86.md).
Short form, on one Vertica node:

    make
    make test
    make deploy              # fenced mode, the default
    make deploy FENCED=no    # unfenced: faster, but inside the Vertica process
    make undeploy            # removes library and functions; schema vgraph stays

The scripts call `vsql`. Connection settings come from the environment:
`VSQL_HOST`, `VSQL_PORT`, `VSQL_USER`, `VSQL_PASSWORD`, `VSQL_DATABASE`.
Nothing is stored in this repository. Every script that changes the database
accepts `--echo_only`: it prints the commands and changes nothing.

Everything is created in schema `vgraph`. Write `vgraph.gkhop(...)`, or run
`SET SEARCH_PATH TO public, vgraph;` and drop the prefix. Build, load and the
procedures need the role `vgraph_admin`; the query functions are open to all.

## The edge table

Any table with two INT columns (source, destination) works. Node ids are
Vertica INT. Business keys stay in your own nodes table.

For exact results between refreshes the table is used as a journal: rows are
only inserted.

    CREATE TABLE app.contacts (
        src  INT NOT NULL,
        dst  INT NOT NULL,
        del  BOOLEAN NOT NULL DEFAULT FALSE,                  -- TRUE = this row deletes the edge
        ts   TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP()   -- version: the moment the row is written
    )
    ORDER BY src, dst SEGMENTED BY HASH(src) ALL NODES
    PARTITION BY (ts AT TIME ZONE 'UTC')::DATE
    GROUP BY CALENDAR_HIERARCHY_DAY((ts AT TIME ZONE 'UTC')::DATE, 2, 2);

- Add an edge: `INSERT` it. Delete an edge: `INSERT` the same (src, dst) with
  `del = TRUE`. The row with the latest version wins.
- The delete flag may be a BOOLEAN (recommended: 1 byte) or an INT with +1 /
  -1. Without a delete column all rows are adds.
- The version column is set by the database when the row is written. Let the
  default do it and never set it in the application. It is not a business
  date: a row that is written today with a date of last year cannot be
  recognised as new by any mechanism.
- Use `DEFAULT CLOCK_TIMESTAMP()`:

  | Default | Value | Effect |
  |---|---|---|
  | `CLOCK_TIMESTAMP()` | the clock when the row is written | best: an add and a delete in one transaction are ordered; exact together with the lock check below |
  | `SYSDATE()`, `GETDATE()` | start of the statement | works; rows of one statement tie; the margin has to cover a statement that waited for a lock |
  | `NOW()`, `CURRENT_TIMESTAMP` | start of the transaction | avoid: a long transaction stamps its rows in the past, and all its rows tie |

  `TIMESTAMPTZ` is an absolute instant. A plain `TIMESTAMP` is the local time
  of the writing session, so it only works if all writers and the refresh use
  the same session time zone. An increasing INT also works (see margin below).
- Vertica's `epoch` pseudo-column is not used: it is not unique and can change.
- A physical `DELETE` or `UPDATE` is allowed, but queries see it only after
  the next refresh.
- Without a version column the graph is static: queries see the snapshot, and
  changes show up at the next refresh.
- Optional `weight FLOAT` column for weighted shortest paths.

**Partition the journal by the date of the version column**, as above. New
rows then stay in their own storage and are never merged into the old data,
and reading the changes takes 1 ms on a 100 million row table (measured after
a forced mergeout, with `ENCODING RLE` on the source column). Without
it, the read can take seconds once Vertica has merged new rows into old
storage, most of all with `ENCODING RLE` on the source column (measured:
5 s per query on 100 million rows). `vgraph.status` and `vgraph.refresh_graph`
warn when the read is slow.

`directed = true` means every stored row is one directed edge. A table that
stores both directions of a contact needs nothing else. `directed = false`
means one row per undirected edge; the reverse is added for you.

## Register, refresh, schedule

    -- graph, edge_table, src_col, dst_col, op_col, weight_col, directed, ver_col, margin
    CALL vgraph.register_graph('contacts', 'app.contacts', 'src', 'dst', 'del', NULL, TRUE, 'ts', NULL);
    CALL vgraph.refresh_graph('contacts');
    CALL vgraph.schedule_refresh('contacts', '0 * * * *');     -- every hour

    SELECT * FROM vgraph.manifest;
    SELECT vgraph.ginfo() OVER(PARTITION NODES) FROM vgraph.probe;
    CALL vgraph.status('contacts');

`register_graph` creates the view `app.contacts_delta`. Queries read it.
Grant `SELECT` on it to the users who may query the graph.

How a row that is committed during or after a refresh is never lost: a row
can be missing from the new snapshot only if its transaction was still open
when the build started reading. An open writer holds an insert lock on the
table. `refresh_graph` reads `v_monitor.locks`, and the changes that queries
apply start at the earliest lock request of an open writer (or at the refresh
start, if nobody is writing), minus the margin. No assumption about how long
your transactions run is needed. Run the refresh as a user who can see all
locks (`dbadmin`, or a user with the `SYSMONITOR` role); the schedule does.

`margin` (seconds, default 60) only covers clock differences between nodes and
`SYSDATE()`-style versions. Rows inside it are applied twice, which is
harmless. For an INT version column the lock time cannot be translated into a
version: there the margin is in units of that column and must cover your
longest write transaction.

`refresh_graph` builds the snapshot, stores it in `vgraph.snapshot`, loads it
into a cache file on every node (`/tmp/vgraph/<graph>/` by default), switches
the manifest and the view, and keeps the previous snapshot. The cache
directory can be set per session:

    ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '/data/vgraph';

A missing or stale cache file is never a data loss. The query says
`run gload`; `CALL vgraph.load_all('contacts')` repairs it, for example after
a node was added or a pod restarted.

## Query

All query functions take the eight columns of the delta view:
`(start, target, src, dst, del, weight, ver, snapshot_id)`.

    -- everyone within 3 hops of person 12345
    SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id
                        USING PARAMETERS graph='contacts', start=12345, depth=3) OVER()
    FROM app.contacts_delta;

    -- several start nodes in one call: append request rows
    SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id
                        USING PARAMETERS graph='contacts', depth=2) OVER()
    FROM (SELECT * FROM app.contacts_delta
          UNION ALL SELECT id, NULL, NULL, NULL, NULL, NULL, NULL, NULL FROM my_start_nodes) q;

    -- shortest path
    SELECT vgraph.gpath(start, target, src, dst, del, weight, ver, snapshot_id
                        USING PARAMETERS graph='contacts', start=12345, target=777) OVER()
    FROM app.contacts_delta;

    SELECT vgraph.gcomponents(start, target, src, dst, del, weight, ver, snapshot_id
                              USING PARAMETERS graph='contacts') OVER() FROM app.contacts_delta;

    SELECT vgraph.gpagerank(start, target, src, dst, del, weight, ver, snapshot_id
                            USING PARAMETERS graph='contacts', iterations=20, damping=0.85) OVER()
    FROM app.contacts_delta;

- `gkhop` returns `(start, node, hops)`. Each node appears once, at its
  smallest hop count. `hops` 0 is the start node itself. If you count the
  start person as level 1, then level = hops + 1.
  Parameters: `depth` (required), `exact` (only nodes at exactly `depth`
  hops), `direction` (`out`, `in`, `both`; default `out`), `max_results`,
  `start`.
- `gpath` returns `(start, target, hop_no, node)`, `hop_no` 0 is the start.
  No rows if there is no path. Parameters: `max_depth`, `direction`,
  `weighted` (Dijkstra; weights must not be negative; `max_depth` is ignored),
  `start` and `target`.
- `gcomponents` returns `(node, component)`. `component` is the smallest node
  id in it. Direction is ignored.
- `gpagerank` returns `(node, rank)`. Ranks sum to 1.

### Large results

A deep k-hop returns millions of rows, and most of the query time is spent
moving them. Keep them in the database and join there:

    CREATE LOCAL TEMPORARY TABLE reached ON COMMIT PRESERVE ROWS AS
    SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id
                        USING PARAMETERS graph='contacts', start=12345, depth=9) OVER()
    FROM app.contacts_delta
    KSAFE 0;

    SELECT p.name, r.hops FROM reached r JOIN app.people p ON p.id = r.node WHERE r.hops = 9;

`KSAFE 0` avoids a buddy projection for the temporary table.

### Without registration

Without the `graph` parameter a query function builds the graph from the edge
rows of its input. This needs no snapshot, but reads the whole table on every
call. It is meant for tests and small tables.

## Measurements

100 million edge rows (16.7 million people, each contact stored in both
directions), data and SQL reference from
https://github.com/mogomo/vertica-graphs-and-trees. Server time from
`v_monitor.query_requests`; results counted inside the database, not sent to a
client. The k-hop question: everyone within 9 hops of person 1, about
5 million people.

Environment A: one node, aarch64, 8 cores, 15 GB RAM, Vertica 26.2.0-1.
Environment B: 3-node Eon cluster, x86_64, 2 cores and 15 GB RAM per node
(a small and busy machine, swapping during the test), Vertica 26.2.0-2.

| Step                                             | A fenced | A unfenced | B fenced | B unfenced |
|--------------------------------------------------|---------:|-----------:|---------:|-----------:|
| build the snapshot (once per refresh)            | 38.5 s   | 32.7 s     | 129.3 s  | not run    |
| load it on all nodes                             | 2.9 s    | 3.0 s      | 10.8 s   | not run    |
| gkhop depth 9                                    | 0.93 s   | 0.74 s     | 2.03 s   | 1.29 s     |
| gkhop depth 2 (about 40 people)                  | 6 ms     | 3 ms       | 10 ms    | 9 ms       |
| SQL BFS procedure of the demo repository, 9 hops | 2.45 s   |            | 5.56 s   |            |

gkhop and the SQL procedure return the same people in every run. The traversal
itself takes about 0.15 s in environment A; the rest is returning 5 million rows.
The build times were measured before the build input was reduced from four
columns to two; they have not been measured again yet. A complete
`refresh_graph` of the 100 million row journal (consolidation with 2383
journaled deletes, build, load, view) took 63.7 s in environment A, fenced.
Through the delta view, with 1000 pending changes: gkhop depth 2 takes 14 ms,
depth 9 takes 1.36 s.

Memory: queries map the snapshot file read-only. All concurrent queries share
one copy in the operating system's page cache, and the file may be larger than
free memory. Private memory per query: gkhop 1 byte per node, gpath 4,
gcomponents 8, gpagerank 24. The build holds about 16 bytes per edge plus 32
per node in the UDx process; see docs/design.md.

## Possible later: a changes table

Today the pending changes are read from the edge table itself. With the
recommended partitioning this takes milliseconds. If a journal cannot be
partitioned and refreshing more often is not enough, a separate small changes
table per graph would make the read independent of the edge table. It is not
implemented. It has a price: Vertica has no triggers, so every program that
writes edges would have to write each change to both tables in one transaction.

How it would be built:

1. `register_graph` gets an opt-in parameter and creates
   `vgraph.changes_<graph> (src INT, dst INT, del BOOLEAN, weight FLOAT, ts TIMESTAMP DEFAULT SYSDATE())`.
2. `make_delta_view` in `sql/procedures.sql` reads that table instead of the edge table.
   Nothing changes in the C++ code: the view keeps its eight columns.
3. `refresh_graph` deletes the rows older than the new boundary after it has switched the manifest.
4. `tests/sql/test_freshness.sh` gets a second pass that journals through the changes table.

Related and already usable on your side: keep the journal small with a Top-K
live aggregate projection (`LIMIT 1 OVER (PARTITION BY src, dst ORDER BY ts DESC)`)
that always holds the latest row per edge, and from time to time replace the
journal by its latest state (create the new table from that projection, swap
the tables, recreate the projection).

## Tests

    make test                              # engine unit tests, no Vertica needed
    make bench                             # engine build and BFS timings, no Vertica needed
    tests/sql/test_khop_reference.sh       # gkhop without snapshot against the BFS procedure of the demo repository
    tests/sql/test_snapshot.sh             # build, load on every node, all query functions, cache rules
    tests/sql/test_freshness.sh            # journal, exactness without refresh, refresh, repair, schedule

The integration tests need https://github.com/mogomo/vertica-graphs-and-trees
cloned as a sibling directory. They drop and recreate schema `GRAPH_DEMO`
(`--schema=NAME` picks another, `--keep` reuses it). Run them on a database node.

## Not supported

- More than 2^32 - 2 nodes in one graph.
- Node ids other than Vertica INT.
- Parallel execution of one query over several nodes: a query runs on the node that started it.
- Two refreshes of the same graph at the same time.
- Ties: two rows for the same edge with the same version have no defined order.
- Big-endian hosts.

More detail: [docs/design.md](docs/design.md), [docs/format.md](docs/format.md).

## License

MIT. Author: Mo (github.com/mogomo).
