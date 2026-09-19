# vgraph design notes

Status: the snapshot path exists (M2). The freshness model (journal, delta
view, overlay, refresh procedures) is the next milestone;
this file will be completed with it.

## Pieces

    edge table --gbuild--> vgraph.snapshot --gload--> node cache file --mmap--> query functions
    (customer)             (UNSEGMENTED ALL NODES)    (/tmp/vgraph/<graph>/)     gkhop gpath gcomponents gpagerank

- `src/engine` is plain C++17 without Vertica includes. It can be linked by
  other programs. `src/udx` holds thin Vertica adapters only.
- The snapshot table is unsegmented on all nodes. It is backed up and
  replicated with the database. The cache file is only a copy of it.
- A missing or damaged cache file is never a data loss: run gload again.

## gload and PARTITION NODES

`gload(...) OVER(PARTITION NODES)` runs one function instance on every node.
Because `vgraph.snapshot` is unsegmented, each instance reads all chunks from
its local copy and writes its own cache file.

Verified so far: single node (Vertica 26.2, aarch64). Open: the same check on
a multi-node Eon cluster. `tests/sql/test_snapshot.sh` compares the number of
nodes that report `loaded` with the number of UP nodes, so it fails on a
cluster where a node gets no chunks.

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
