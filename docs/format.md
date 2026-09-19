# vgraph snapshot format, version 1

One snapshot is one graph in CSR form, serialised to bytes. The same bytes are
stored in `vgraph.snapshot` (cut into chunks) and in the per-node cache file
`<cache_dir>/<graph>/<snapshot_id>.vg`. Query functions mmap the file and read
it in place. Nothing is parsed or copied.

The code is in `src/engine/snapshot.h`, `snapshot.cpp` and `builder.cpp`.
It has no Vertica dependency.

## Rules

- Little-endian only. The library refuses to compile on a big-endian host.
- Every section starts on an 8-byte boundary. Padding bytes are 0.
- The file size is a multiple of 8.
- A node is addressed by its position: the index of its id in `ids`.
  Positions are `uint32`, so a graph has at most 2^32 - 2 nodes.

## Header, 128 bytes

| Offset | Type      | Field             | Meaning |
|-------:|-----------|-------------------|---------|
| 0      | char[8]   | magic             | `VGRAPHS1` |
| 8      | uint32    | format_version    | 1 |
| 12     | uint32    | flags             | bit 0 directed, bit 1 weighted |
| 16     | uint64    | node_count        | N |
| 24     | uint64    | edge_count        | E, stored directed edges, duplicates removed |
| 32     | int64     | max_ver           | highest journal version at build time (microseconds for a timestamp column); informational |
| 40     | uint64    | checksum          | see below |
| 48     | uint64    | total_bytes       | file size |
| 56     | uint64    | off_ids           | section offsets from the start of the file |
| 64     | uint64    | off_out_offsets   | |
| 72     | uint64    | off_out_nbrs      | |
| 80     | uint64    | off_in_offsets    | 0 if not directed |
| 88     | uint64    | off_in_nbrs       | 0 if not directed |
| 96     | uint64    | off_out_weights   | 0 if not weighted |
| 104    | uint64    | off_in_weights    | 0 if not weighted or not directed |
| 112    | uint64[2] | reserved          | 0 |

## Sections, in this order

| Section      | Type         | Present       | Content |
|--------------|--------------|---------------|---------|
| ids          | int64[N]     | always        | node ids, sorted, unique |
| out_offsets  | int64[N+1]   | always        | out edges of position p are `out_nbrs[out_offsets[p] .. out_offsets[p+1])` |
| out_nbrs     | uint32[E]    | always        | target positions, sorted inside each list |
| in_offsets   | int64[N+1]   | directed      | reverse CSR |
| in_nbrs      | uint32[E]    | directed      | source positions, sorted inside each list |
| out_weights  | float32[E]   | weighted      | parallel to out_nbrs |
| in_weights   | float32[E]   | weighted and directed | parallel to in_nbrs |

Section offsets are fully determined by N, E and the flags. A reader computes
them again and rejects a file whose header says something else.

`directed` flag off means the graph is symmetric: the builder added the reverse
of every edge, so the in lists equal the out lists and are not stored.

Self loops are kept. An edge that is given twice is stored once; with weights
the first one wins.

## Checksum

64-bit, over the whole file taken as little-endian 8-byte words, with the
checksum field itself counted as 0:

    h = 0xcbf29ce484222325
    for each word w:  h = (h XOR w) * 0x100000001b3;  h = h XOR (h >> 29)

`gload` verifies it before a file becomes active. Query functions do not
(it would read the whole file on every query). They check the magic, the
version, the size, the section offsets and the first and last CSR offset.

## Chunks

`gbuild` returns the bytes in chunks of 8 MB (8388608 bytes), numbered from 0.
Only the last chunk is shorter. `gload` writes chunk k at file offset k * 8 MB,
so chunks may arrive in any order.

## Size

    128 + 8N + 8(N+1) + 4E                    symmetric, unweighted
    + 8(N+1) + 4E                             directed
    + 4E (+ 4E if directed)                   weighted

The demo graph (16.7 million nodes, 100 million directed edges) takes 1.2 GB.
