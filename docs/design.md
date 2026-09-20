# vgraph design notes

This file explains the decisions. The user's view is in the README.

## Pieces

    edge table --gbuild--> vgraph.snapshot --gload--> node cache file --mmap--> query functions
    (customer)             (UNSEGMENTED ALL NODES)    (/tmp/vgraph/<graph>/)     gkhop gpath gcomponents gpagerank

- `src/engine` is plain C++17 without Vertica includes. It can be linked by
  other programs. `src/udx` holds thin Vertica adapters only.
- The snapshot table is unsegmented on all nodes. It is backed up and
  replicated with the database. The cache file is only a copy of it.
- A missing or damaged cache file is never a data loss: run gload again.

## gload on every node

`gload(...) OVER(PARTITION NODES)` runs one function instance on every node
that receives input rows. Measured on Vertica 26.2:

- With `vgraph.snapshot` (unsegmented) as the only input, Vertica reads the
  replicated table on one node only. On a 3-node Eon cluster only one node ran
  gload. On a single node this cannot be seen.
- So the input is driven by a segmented table. `vgraph.probe(k)` holds 1024
  rows, segmented by hash, so every node has some. `vgraph.gnode(k)
  OVER(PARTITION NODES)` returns the smallest k stored on each node. The chunks
  are cross joined to those probe rows: one probe row per node, so every node
  gets every chunk exactly once. The join is local because the snapshot table
  is replicated.
- The mapping is computed inside the same statement, so it follows node and
  shard changes. gload returns one row per node that loaded.
- ginfo reads `vgraph.probe` for the same reason and answers once per node.

Tested on a single node (aarch64) and on a 3-node Eon cluster (x86_64).
`tests/sql/test_snapshot.sh` fails if the number of nodes that report `loaded`
differs from the number of UP nodes.

Eon note: a statement runs on the nodes that take part in the session. A node
that did not take part (for example one added later) has no cache file. A query
that starts there fails with a clear message, and running gload again repairs it.

## Cache rules

- Layout: `<cache_dir>/<graph>/<snapshot_id>.vg` and `<cache_dir>/<graph>/ACTIVE`.
- cache_dir: function parameter, then session parameter
  (`ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '...'`), then `/tmp/vgraph`.
- gload writes to a temporary file, verifies size, structure and checksum,
  renames it into place and then replaces ACTIVE by rename. A failed load
  leaves the cache as it was.
- gload keeps the new and the previously active snapshot file. It removes
  other files in the graph's directory only if they start with the vgraph
  magic. It never touches anything else. Graph names are limited to letters,
  digits and underscore, so a name cannot point outside cache_dir.
- Two gload runs for the same graph at the same time are not supported.
- Directories are created with mode 0700, files with 0600.

## Memory

gbuild, peak, unweighted: about 16 bytes per input edge while rows arrive,
then 16 bytes per edge plus 32 bytes per node while the snapshot is written.
Weighted adds 4 and 8 bytes per edge. With `directed=false` count every input
edge twice. Example: 100 million edges, 16.7 million nodes: about 2.1 GB.

In fenced mode this memory belongs to the fenced process. The limit is the
configuration parameter `FencedUDxMemoryLimitMB` (-1 = no limit).

Query functions mmap the snapshot and copy nothing. Working memory per call:
gkhop 1 byte per node plus the frontier; gpath 4 bytes per node (12 with
weights); gcomponents 4 bytes per node; gpagerank 24 bytes per node.

## Freshness: exact results between refreshes

The edge table is a journal: rows are only inserted, a delete is a row with the
delete flag, and the row with the latest version wins. The version is a column
of the customer's table (insertion time or an increasing INT).

Vertica's `epoch` pseudo-column is deliberately not used: it is not unique, it
can change, and it cannot be part of a projection ("Column name epoch is reserved").

Every query applies the journal rows that may be newer than the snapshot on top
of the mapped snapshot (src/engine/delta.h) and runs the algorithm on the result.

- The rows come from the view `<schema>.<graph>_delta`: `ver_col > boundary`
  plus one sentinel row, because Vertica does not call a transform function on
  empty input. Every row carries the id of the snapshot the view belongs to.
- The boundary is a literal, written into the view by refresh_graph, so Vertica
  can prune partitions and storage containers.
- Boundary for a timestamp version. A version says when a row was written;
  visibility depends on when it was committed. The gap is the open
  transaction. A row is missing from the snapshot only if it was committed
  after the build started reading, so it was written either after the refresh
  started or by a transaction that was open at that moment. An open writer
  holds an insert lock, visible in `v_monitor.locks` with its request time
  (checked for INSERT and COPY, from another session, on one node and on Eon).
  Hence: boundary = LEAST(clock at refresh start, earliest lock request of an
  open writer) - margin. With `CLOCK_TIMESTAMP()` versions a row is stamped
  after its lock was granted, so the argument is exact. With `SYSDATE()` the
  stamp is the statement start, which can be earlier than the lock grant if the
  statement waited; the margin (default 60 s) covers that and clock skew.
  `NOW()` stamps the transaction start and is not suitable.
  Measured example: refresh started 21:15:24.80, an open writer had asked for
  its lock at 21:15:19.7933; the boundary became 21:15:19.7933, the late row's
  version was 21:15:19.7940, so the row was in the delta.
  For an INT version the lock time cannot be mapped to a version: boundary =
  highest version minus the margin, and the margin must cover open writers.
- Consolidation for the build: unweighted graphs take all add rows minus the
  edges whose latest row is a delete; only edges that were ever deleted are
  ranked (100 million rows: refresh 160 s -> 64 s). Weighted graphs rank every
  edge, because the latest weight must win.
- Rows inside the margin are in the snapshot and in the delta. That is
  harmless: rows are applied in version order and the last op wins, so applying
  a suffix of the journal again always ends in the same state.
- An overlay that changes nothing is dropped, and the query runs on the plain
  snapshot.

Reading the delta, measured on a 100 million row journal with 1000 new rows:

| Journal layout | before mergeout | after new rows were merged into old storage |
|---|---|---|
| default encoding | 1 to 3 ms | 12 to 46 ms |
| `ENCODING RLE` on src | 1 to 3 ms | 5 s |
| partitioned by version date (any encoding) | 1 to 2 ms | 1 ms: they are never merged |
| projection sorted by the version column | | 5 s: the optimizer does not choose it; 10 ms with a table-level `PROJS` hint |

Hence the recommendation to partition the journal by the version date, and the
warning in `vgraph.status` when the read takes more than 500 ms.

Stale check: snapshot ids come from the sequence `vgraph.snapshot_seq` and never
repeat. If the id in a node's cache is lower than the id carried by the view
rows, the query fails with "snapshot cache stale on <node>: run gload". A higher
id is fine: that happens for a moment during a refresh, when the node is loaded
but the view still has the older, wider boundary.

Refresh order: boundary -> build -> insert chunks -> gload on all nodes (checked:
every node that holds probe rows must report `loaded`) -> manifest and view ->
delete snapshots older than the previous one.

Names: `vgraph.refresh_graph`, because `refresh` is a built-in Vertica function
name and cannot be used for a procedure.

## Two build paths, one file

The in-memory builder (src/engine/builder.cpp) needs all edges at once: the
sorted node ids, the degrees and the edges grouped by target are only known
when the last row has arrived, and a transform function sees its input once.

The streaming build moves that work into Vertica, which sorts and spills under
its own memory management:

1. consolidated edges into a table, once;
2. node map: `id, ROW_NUMBER() OVER(ORDER BY id) - 1 AS pos` over the distinct ids;
3. edges as positions, unique, stored sorted by (s, d); both directions for an
   undirected graph;
4. counts; and the test whether the in lists equal the out lists: first an
   order-independent hash sum over (s, d) and (d, s), and only if they are
   equal the exact anti join. `vgraph.manifest.both_directions = TRUE` declares
   the answer: the anti join is skipped, the hash sums must still agree or the
   refresh fails;
5. one statement per section feeds `gbuild_mapped` with a sorted stream
   (src/engine/section_writer.h). It validates the stream (order, uniqueness,
   ranges, totals) and keeps one 8 MB piece in memory;
6. `gbuild_header` reads the checksum parts of the sections and writes the header.

The sections do not depend on each other, so they could run at the same time
and on different nodes later.

The combinable checksum (docs/format.md) and pieces addressed by byte offset
make this possible. Both paths must give the same bytes: tests/engine/
test_section_writer.cpp checks it without Vertica, tests/sql/test_build_paths.sh
with Vertica, for graphs stored both ways, one way, undirected, and weighted
journals with deletes. Measured at 100 million rows: whole refresh 21 s in
memory, 82 s streaming, identical 667 MB file.

## Limits today

- Without the `graph` parameter a query function builds the graph from its
  input rows; deleted edges are not accepted there.
- gpath with `weighted=true` ignores `max_depth`.
- All query functions run with `OVER()`: one instance, one node.
