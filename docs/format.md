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
| 12     | uint32    | flags             | bit 0 directed, bit 1 weighted, bit 2 in lists equal out lists |
| 16     | uint64    | node_count        | N |
| 24     | uint64    | edge_count        | E, stored directed edges, duplicates removed |
| 32     | int64     | max_ver           | highest journal version at build time (microseconds for a timestamp column); informational |
| 40     | uint64    | checksum          | see below |
| 48     | uint64    | total_bytes       | file size |
| 56     | uint64    | off_ids           | section offsets from the start of the file |
| 64     | uint64    | off_out_offsets   | |
| 72     | uint64    | off_out_nbrs      | |
| 80     | uint64    | off_in_offsets    | 0 if no reverse CSR is stored |
| 88     | uint64    | off_in_nbrs       | 0 if no reverse CSR is stored |
| 96     | uint64    | off_out_weights   | 0 if not weighted |
| 104    | uint64    | off_in_weights    | 0 if not weighted or no reverse CSR is stored |
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

The reverse CSR (in_offsets, in_nbrs, in_weights) is stored only when bit 0 is
set and bit 2 is not. "directed" in the tables above means exactly that.

- Bit 0 off: one input row per undirected edge. The builder added the reverse
  of every edge, so the in lists equal the out lists.
- Bit 0 and bit 2 on: every row is a directed edge, but the table stores both
  directions of every edge (with equal weights), so the in lists equal the out
  lists as well. The builder finds this by comparing the two and leaves the
  reverse CSR out. The demo graph shrinks from 1.2 GB to 0.8 GB.

The difference between the two matters for changes after the snapshot: with bit
0 off a journal row changes both directions, with bit 0 on it changes one.

Self loops are kept. An edge that is given twice is stored once; with weights
the first one wins.

## Checksum

64-bit. The file is taken as little-endian 8-byte words; word i is at byte
offset 8 * i. The checksum is the XOR of one value per non-zero word, mixed
with the word's position (splitmix64 finalizer). The checksum field itself and
all zero words contribute nothing:

    sum = 0
    for each word w at index i, w != 0, i != 5:
        z = w + (i + 1) * 0x9E3779B97F4A7C15
        z = (z XOR (z >> 30)) * 0xBF58476D1CE4E5B9
        z = (z XOR (z >> 27)) * 0x94D049BB133111EB
        sum = sum XOR z XOR (z >> 31)

Because it is an XOR over positions, the checksum of a file is the XOR of the
checksums of its parts, in any order. The streaming build uses this: every
section is written by its own statement, and the header is made last from the
sections' parts. A missing piece reads as zeros and changes the sum.

`gload` verifies it before a file becomes active. Query functions do not
(it would read the whole file on every query). They check the magic, the
version, the size, the section offsets and the first and last CSR offset.

## Pieces

`vgraph.snapshot` holds the file as pieces `(byte_offset, chunk)` of at most
8 MB (8388608 bytes). `gload` writes every piece at its offset, so pieces may
arrive in any order; together they must cover the file exactly once.
`gbuild` cuts the file every 8 MB. `gbuild_mapped` cuts every section
separately, and `gbuild_header` adds the 128 header bytes at offset 0.

## Size

    128 + 8N + 8(N+1) + 4E                    symmetric, unweighted
    + 8(N+1) + 4E                             directed
    + 4E (+ 4E if directed)                   weighted

The demo graph (16.7 million nodes, 100 million directed edges) takes 1.2 GB.
