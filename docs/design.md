# vgraph design notes

Status: the snapshot path exists (M2). The freshness model (journal, delta
view, overlay, refresh procedures) is the next milestone; this file will be
completed with it.

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
- Stale check: if an input row carries a `snapshot_epoch` higher than the
  cached file's `max_epoch`, the query fails with
  "snapshot cache stale on <node>: run gload".

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

## Limits today

- Query functions take the graph either from the cache (`graph` parameter) or
  from the edge rows of the input (no `graph` parameter), not both. Delta rows
  on top of a snapshot arrive with the freshness milestone.
- gpath with `weighted=true` ignores `max_depth`.
- All query functions run with `OVER()`: one instance, one node.
