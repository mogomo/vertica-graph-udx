# vertica-graph-udx (vgraph)

Graph analytics inside Vertica: a C++ UDx library with k-hop neighbourhoods,
shortest paths, connected components and PageRank, on tables Vertica already
stores. Every function is called from SQL like a built-in function.

This repository holds the library, the benchmark that compares it with Neo4j
on the same machine, and everything needed to repeat that benchmark yourself.

![The headline numbers](blog/blog_figures/01_headline_numbers.png)

- **Side by side with Neo4j** on the same machine and the same data, at
  **100 million and at 1 billion rows**, with the same answers on both sides.
- **One billion rows**: everyone within 9 hops of a person, 7.3 million people,
  in **0.08 s** against Neo4j's 5.25 s. Vertica is also **13x** faster at
  PageRank, **5x** at the shortest path and **3x** at connected components, and
  its index is **6.7 GB** where Neo4j's store for the same graph is **26 GB**.
- The graph index (a snapshot) lives inside Vertica. Every query merges it with
  the rows written after it, so results are always exact, also between rebuilds.
- Works on single-node and multi-node databases (tested: one node on aarch64,
  3-node Eon cluster on x86_64).

Contents:

1. [The sample data: David and his contacts](#1-the-sample-data-david-and-his-contacts)
2. [Benchmark results](#2-benchmark-results)
3. [The files in this repository](#3-the-files-in-this-repository)
4. [Run the benchmark yourself](#4-run-the-benchmark-yourself)
5. [Use it on your own tables](#5-use-it-on-your-own-tables)
6. [Reference](#6-reference)

The same story as an article: [blog/README.md](blog/README.md). As a notebook
with live runs: [notebook/vgraph_demo.ipynb](notebook/vgraph_demo.ipynb).

## 1. The sample data: David and his contacts

All tests use the contact generator of the earlier repository
https://github.com/mogomo/vertica-graphs-and-trees (file
`graph_contacts_demo.sql`):

- Table `contact(src_id, dst_id)`: one row means "src knows dst". Person 1 is
  David.
- Every person picks 3 random contacts, and each contact is stored in both
  directions: 6 rows per person. 100 million rows are 16.7 million people;
  1 billion rows are 166.7 million people.
- The question: who is within k hops of David? 1 hop is the people David knows,
  2 hops is the people they know, and so on. Every person is counted once, at
  the smallest number of hops. If you count David as level 1, then
  level = hops + 1.

![David and his contacts](blog/blog_figures/02_david_graph.png)

Left: the first three hops, drawn from a small sample of the same data. Right:
the same question on a billion-row table. The generator picks random contacts
on every run, so every billion-row graph is a different graph and person 1
reaches a different number of people in each; the time follows the number of
people reached, not the size of the table.

## 2. Benchmark results

### Environment

| | |
|---|---|
| Machine | one virtual machine on a laptop: aarch64, 8 cores, 35 GB RAM, Rocky Linux 9 |
| Vertica | 26.2, one node; vgraph unfenced, 8 threads (the default: one per core) |
| Neo4j | 5.26 Community, APOC, Graph Data Science 2.13.4, JDK 21; loaded with `neo4j-admin database import` |
| Data | the same generated rows in both systems, at 100 million and at 1 billion rows. Vertica stores both directions; Neo4j stores every contact once and is asked without a direction |
| Memory | 100 million rows: 8 GB heap and 8 GB page cache, which holds Neo4j's whole 2.8 GB store. 1 billion rows: the store is 26 GB and does not fit, so Neo4j got the setting each question needs (see below) |
| Times | server times on both sides, best of several runs after a warm-up. Vertica: `v_monitor.query_requests`. Neo4j: the times `cypher-shell` reports |
| Answers | compared for every question by `scripts/neo4j_compare.sh`: identical |

On this machine `SELECT 1;` takes up to 2 ms in Vertica. A time at that level
is the cost of a statement, not of a graph search. Such rows are marked with *
below. They are not relevant for the comparison and are shown for completeness.

### 100 million rows: Neo4j and Vertica side by side

| Question (100 million rows, 16.7 million people) | Neo4j | Vertica + vgraph | Vertica is |
|---|---:|---:|---|
| people within 2 hops of David (28)   | 1 to 2 ms | 2 ms | equal * |
| people within 3 hops (144)           | 1 to 3 ms | 2 ms | equal * |
| people within 6 hops (22,553)        | 10 ms | **3 ms** | **3 times faster** |
| people within 9 hops (3.3 million)   | 2.28 s | **0.046 s** | **50 times faster** |
| shortest path, 12 hops               | 15 ms | **4 ms** | **3.8 times faster** |
| connected components, counted        | 0.67 s, after a 5.8 s projection | **0.37 s** | **1.8 times faster** |
| PageRank, 20 iterations              | 18.1 s, after the same projection | **1.54 s** | **12 times faster** |
| load the data                        | 45 s (9 s export, 26 s import, 10 s index) | 17 s | |
| prepare for graph queries            | 5.8 s projection, again after every restart | 19 s (`refresh_graph`) | |
| size on disk                         | 2.8 GB | **1.1 GB** (table 444 MB + snapshot 667 MB) | **2.5 times smaller** |

\* At or below the cost of `SELECT 1;`.

![Neo4j and Vertica side by side](blog/blog_figures/03_neo4j_vertica_100m.png)

Notes. This table is the run of 2026-09-21 on newly generated data, loaded into
both systems (in this graph person 1 reaches 3.3 million people within 9 hops;
the run published before had another random graph with 5.8 million). Neo4j
k-hop: the better of a Cypher pattern and APOC (6 hops: Cypher 10 ms, APOC
13 ms; 9 hops: APOC 2.28 s, Cypher 2.35 s). At 6 hops Vertica's 3 ms is itself the cost of a
statement, so read "3 times" as "at least". Components and PageRank: Graph Data Science
(`gds.wcc.stats`, `gds.pageRank.stats`), which only counts and, in the
community edition, uses at most 4 threads; the vgraph side returns rows that
SQL can join. The results are for this data shape and this machine; run the
script on yours.

### One billion rows: Neo4j and Vertica side by side

The same generator with 1,000,000,000 rows: 166,666,666 people, 999,999,982
distinct edges, a 6.67 GB snapshot. **Both systems were loaded with this data.**
The generator picks random contacts on every run, so this is a different graph
from the 100 million row one above: here person 1 reaches 7,291,022 people
within 9 hops.

Neo4j's store for this graph is 26 GB on a machine with 34 GB, so the two
systems cannot both be resident. Each was measured **alone on the machine with a
warm cache**: one run to warm it, then the best of the runs that followed.

| Question (1 billion rows, 166.7 million people) | Neo4j | Vertica + vgraph | Vertica is |
|---|---:|---:|---|
| people within 2 hops of David (52)   | 2 ms | 3 ms | equal * |
| people within 3 hops (284)           | 2 ms | 3 ms | equal * |
| people within 6 hops (45,975)        | 24 ms | **4 ms** | **6 times faster** |
| people within 9 hops (7,291,022)     | 5.25 s | **0.076 s** | **69 times faster** |
| shortest path, 13 hops               | 33 ms | **7 ms** | **4.7 times faster** |
| connected components (1)             | 10.9 s, after a 169.5 s projection | **3.60 s** | **3 times** (50 with the projection) |
| PageRank, 20 iterations              | 204.3 s, after the same projection | **15.5 s** | **13 times** (24 with the projection) |
| get the data in                      | 9.7 min (export 1.9 + import 6.3 + index 1.4) | **2.2 min** (generated in place) | 4.3 times |
| prepare for graph queries            | **169.5 s** projection, again after every restart | 10.2 min `refresh_graph`, once per refresh | Neo4j 3.6 times |
| size on disk                         | 26.4 GB | **11.6 GB** (table 4.9 + index 6.7) | **2.3 times smaller** |

\* At or below the cost of `SELECT 1;`.

![Neo4j and Vertica side by side at one billion rows](blog/blog_figures/04_neo4j_vertica_1b.png)

**These numbers were re-measured on 2026-09-21, and the earlier ones are worth knowing.** The
first publication of this table had 31 ms at 6 hops, 0.19 s at 9 hops and 116 ms for the shortest
path, and Neo4j won the path row by 3.5 times. The cause was not the search. Every statement mapped
the 6.7 GB index file anew, and a new mapping pays a page fault for every page a search touches:
34,000 faults for that one path, 50 ms of faults around an 8 ms search. The library now keeps the
mapping between statements (see `docs/design.md`, "Cache rules", for what that means for the
Vertica process). `gpath` also got a `threads` parameter and stops at the first meeting of its two
searches. Neo4j's numbers are the ones measured before: nothing changed on its side.

**Memory: no single Neo4j setting answers everything at this size.**

- The k-hop questions need a large **page cache**. With 10 GB of page cache,
  9 hops took **242.9 s**. With 22 GB, the same question took **5.25 s**.
- The whole-graph algorithms need a large **heap**. With a 14 GB heap,
  `gds.graph.project` stopped with `java.lang.OutOfMemoryError`. It needed a
  **26 GB heap**, then 169.5 s to build its in-memory copy of the graph.
- A 22 GB page cache and a 26 GB heap do not both fit in 34 GB.

Vertica's index for the same graph is 6.7 GB, so the operating system keeps it
cached and one configuration answers every question.

**Vertica alone, on a second billion-row graph** (generated separately: here
person 1 reaches 3.7 million people within 9 hops. The three search times were
re-measured on 2026-09-21; the last two rows are from the earlier run):

| Step | Time |
|---|---:|
| people within 9 hops (3.7 million), counted | 0.045 s |
| the same, all 3.7 million rows returned | 0.50 s |
| shortest path, 14 hops | 10 ms |
| people within 12 hops (159.6 million, nearly everyone) | 1.0 s |
| SQL BFS procedure of the earlier repository, 9 hops | 16.8 s |
| `refresh_graph` without `both_directions` declared | 17.9 min |

Query time follows the size of the answer, not the size of the table: on a
billion rows, reaching 3.7 million people took 0.045 s and reaching 7.3 million
took 0.076 s. `refresh_graph` chose the streaming build by itself; the build
function holds 8 MB and the machine did not swap.

![People reached and server time per depth](blog/blog_figures/05_one_billion_depth_sweep.png)

### More measurements

- **Threads.** With one thread (100M rows): 9 hops 0.11 s, components 1.25 s,
  PageRank 6.7 s. 1B rows: 9 hops 0.43 s on one thread and 0.076 s on 8;
  12 hops 5.7 s and 1.0 s. The shortest path gains little from threads (1B
  rows: 8 ms on one, 7 ms on 8): its search visits about 70,000 people.
- **Fenced or unfenced.** Fenced costs 3 to 7 ms more per call (100M rows, a
  new session for every call: 5 ms for 2 hops, 9 ms for 6 hops, 57 ms for
  9 hops, 10 ms for the path; later path calls of the same session 7 ms). A
  fenced process ends with its session, so the first graph query of a session
  maps the index anew. Every test passes in both modes. Unfenced code runs inside the Vertica process, so a
  fault in it can take the node down. `make deploy` installs fenced.
- **Compiler flags.** `make OPT="-O3 -mcpu=native"` changed no number: the work
  is random memory access. The portable build is the fast one.
- **Returning rows costs more than finding them.** Use `gkhop_count`,
  `gcomponents_count` and `top=N` when you need numbers.
- **Pending changes.** Through the delta view with 1000 pending changes, 2 hops
  take 14 ms and 9 hops 1.4 s (100M rows, fenced, first release).
- **3-node Eon cluster** (x86_64, 2 cores and 15 GB per node, a small machine
  that was swapping; 100M rows, fenced, first release): build 80.9 s, load on
  three nodes 5.1 s, 9 hops counted 0.48 s, 2 hops 22 ms, SQL procedure 4.87 s.
  This system proves portability, not speed.

## 3. The files in this repository

| File | What it does | How to use it |
|---|---|---|
| `Makefile` | builds `build/libvgraph.so`, runs the unit tests, installs or removes the library | `make`, `make test`, `make deploy [FENCED=no]`, `make undeploy` |
| `src/engine/` | the graph engine, pure C++17 without Vertica includes: CSR index, snapshot, delta overlay, BFS (serial and parallel), path, components, PageRank | compiled by `make`; usable without Vertica |
| `src/udx/` | the Vertica adapters: one small file per SQL function | compiled by `make` |
| `sql/install.sql` | creates schema `vgraph`, its tables, the functions and the role `vgraph_admin` | run by `make deploy` |
| `sql/procedures.sql` | the procedures `register_graph`, `refresh_graph`, `schedule_refresh`, `load_all`, `status`, `unregister_graph` | run by `make deploy` |
| `sql/uninstall.sql` | removes functions and library | run by `make undeploy` |
| `scripts/deploy.sh` | what `make deploy` and `make undeploy` call | `scripts/deploy.sh [--fenced=yes\|no] [--undeploy]` |
| `scripts/demo.sh` | a small end-to-end demo: journal, register, refresh, the four queries, a change seen without a refresh. Needs nothing else, cleans up | `scripts/demo.sh` |
| `scripts/benchmark.sh` | the Vertica measurements: load, build, load on nodes, `gkhop` and `gkhop_count` against the SQL procedure, fenced and unfenced. Prints a results table | `scripts/benchmark.sh --rows=100000000 [--streaming]` |
| `scripts/neo4j_compare.sh` | the side-by-side test: the same data and questions in Vertica and Neo4j. Fails if the answers differ | `scripts/neo4j_compare.sh --neo4j_home=DIR --rows=100000000` |
| `scripts/register.sh` | registers your edge table as a graph | `scripts/register.sh --graph=contacts --table=app.contacts --src=src --dst=dst --op=del --ver=ts [--both_directions]` |
| `scripts/refresh.sh` | rebuilds the snapshot; also `--load_only`, `--status`, `--schedule='0 * * * *'` | `scripts/refresh.sh --graph=contacts` |
| `tests/engine/` | unit tests of the engine, no Vertica needed | `make test`, `make bench` |
| `tests/sql/` | integration tests against the SQL procedure of the earlier repository | see [Tests](#tests) |
| `notebook/vgraph_demo.ipynb` | the article as a notebook: David's graph, the functions live, the results | Jupyter; needs `vertica-python`, `networkx`, `matplotlib`, `pandas`, and the `VSQL_*` variables |
| `blog/` | the article and its figures | [blog/README.md](blog/README.md) |
| `docs/` | `design.md` (freshness model, limits), `format.md` (snapshot format), `build-x86.md` (install step by step), `VERTICA_NOTES.md` (verified Vertica behaviour) | read |

Every script reads the connection from the environment (`VSQL_HOST`,
`VSQL_PORT`, `VSQL_USER`, `VSQL_PASSWORD`, `VSQL_DATABASE`); nothing is stored
in this repository. Every script accepts `--help`, and every script that
changes the database accepts `--echo_only`: it prints the commands and changes
nothing.

## 4. Run the benchmark yourself

On one Vertica node (Vertica 26.x with the C++ SDK in `/opt/vertica/sdk`, g++
with C++17, GNU make):

    git clone https://github.com/mogomo/vertica-graph-udx
    git clone https://github.com/mogomo/vertica-graphs-and-trees
    cd vertica-graph-udx
    make && make test
    make deploy FENCED=no
    scripts/demo.sh
    scripts/benchmark.sh --rows=100000000

The two repositories must be siblings: the scripts read the generator from
`../vertica-graphs-and-trees`. `benchmark.sh` drops and recreates schema
`GRAPH_DEMO` (`--schema=NAME` picks another, `--keep` reuses the data) and
leaves the library in fenced mode. Start with `--rows=2000000` for a quick run.

For the comparison with Neo4j, unpack into one directory a Neo4j 5 Community
tarball, put the APOC core jar and the Graph Data Science jar into its
`plugins` directory, and have a Java 21 runtime. Nothing is installed
system-wide. Then:

    scripts/neo4j_compare.sh --neo4j_home=DIR [--java_home=DIR] --rows=100000000

At a billion rows Neo4j's store is 26 GB and its memory settings decide what it
can answer. Give it the page cache for traversals, or the heap for the
whole-graph algorithms, but on a 34 GB machine not both:

    scripts/neo4j_compare.sh --neo4j_home=DIR --rows=1000000000 --heap=6g  --pagecache=22g
    scripts/neo4j_compare.sh --neo4j_home=DIR --rows=1000000000 --heap=26g --pagecache=2g  --keep

A question Neo4j cannot answer is reported as "did not run" with the reason kept
in a log file, never as a time.

The script owns that Neo4j installation: it replaces its database, writes its
configuration (localhost only) and stops the server at the end. It uses schema
`VGNEO` and the graph name `vgneo` (`--schema=NAME --graph=NAME --work=DIR`
keep a second size next to the first), needs about 1.5 GB for the exported CSV
files per 100 million rows, and takes about 25 minutes.

### Run the notebook

The notebook runs on any computer that can reach the database; the library must
be installed there first (`make deploy`, above). It needs Python 3.9 or later:

    python3 -m venv .venv
    .venv/bin/pip install jupyterlab vertica-python networkx matplotlib pandas
    export VSQL_HOST=... VSQL_PORT=5433 VSQL_USER=dbadmin VSQL_PASSWORD=... VSQL_DATABASE=...
    .venv/bin/python -m jupyterlab notebook/vgraph_demo.ipynb

Then choose Run, Run All Cells. It takes about a minute. What it does in the
database: it drops and recreates schema `VGRAPH_NB` (50,000 people) and the
graph `nb`, so the user needs the role `vgraph_admin` or must be the database
administrator. The side-by-side charts draw the numbers measured by
`scripts/neo4j_compare.sh`; Neo4j is not needed to run the notebook. The cells
for one billion rows ask the largest registered graph with more than 500
million edges; without one they print "skipped". The file in the repository is
stored with its results, so GitHub shows it without running anything.

## 5. Use it on your own tables

### Requirements and install

- Vertica 26.x with the C++ SDK in `/opt/vertica/sdk`.
- g++ with C++17 support and GNU make, on one Vertica node.
- Builds on x86_64 and aarch64. Tested on RHEL 8.10 x86_64 with g++ 8.5 and on
  Rocky Linux 9 aarch64 with g++ 11.5.

Step by step, with versions and expected output: [docs/build-x86.md](docs/build-x86.md).
Short form:

    make
    make test
    make deploy              # fenced mode, the default
    make deploy FENCED=no    # unfenced: faster, but inside the Vertica process
    make undeploy            # removes library and functions; schema vgraph stays

Everything is created in schema `vgraph`. Write `vgraph.gkhop(...)`, or run
`SET SEARCH_PATH TO public, vgraph;` and drop the prefix. Build, load and the
procedures need the role `vgraph_admin`; the query functions are open to all.

### The edge table

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
  default do it and never list the column in an INSERT or COPY. It is not a
  business date. If your edges have a business date, keep it in a column of
  its own next to the version:

      valid_from DATE,                                       -- yours
      ts TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP()      -- the version

  A row written today with `valid_from = '2020-01-01'` still gets today's
  version and is seen as new. Vertica cannot stop a writer from setting `ts`
  itself; `vgraph.status` warns when it finds versions in the future.
- Use `DEFAULT CLOCK_TIMESTAMP()`:

  | Default | Value | Effect |
  |---|---|---|
  | `CLOCK_TIMESTAMP()` | the clock when the row is written | best: an add and a delete in one transaction are ordered; exact together with the lock check below |
  | `SYSDATE()`, `GETDATE()` | start of the statement | works; rows of one statement tie; the margin has to cover a statement that waited for a lock |
  | `NOW()`, `CURRENT_TIMESTAMP` | start of the transaction | avoid: a long transaction stamps its rows in the past, and all its rows tie |

  `TIMESTAMPTZ` is an absolute instant. A plain `TIMESTAMP` is the local time
  of the writing session, so it only works if all writers and the refresh use
  the same session time zone.
- An INT version (a sequence or an IDENTITY column) is accepted, but it is the
  second choice. Vertica hands every session its own block of 250,000 sequence
  numbers, so numbers do not follow the order of writing: three rows written
  one after the other by two sessions got 250001, 500001, 250002. "Last row
  wins" is then wrong across sessions. It is safe with one writer session, or
  with `CACHE 1`, which is slow for bulk loads. See margin below as well.
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

### Register, refresh, schedule

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

The query functions map the cache file into memory and keep that mapping
between statements, because a new mapping of a large file costs more than a
search. Unfenced, the mapping belongs to the Vertica process: it uses address
space, and the pages are file cache of the operating system. Fenced, the
process ends with the session, so the first graph query of a session is slower
(1 billion rows: 34 ms for a path, then 10 ms). A replaced snapshot is let go
at the next graph query of that process.

### Query

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
                            USING PARAMETERS graph='contacts', iterations=20, damping=0.85, top=100) OVER()
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
  `start` and `target`, `threads`.
- `gcomponents` returns `(node, component)`. `component` is the smallest node
  id in it. Direction is ignored. `gcomponents_count` returns `(component,
  nodes)`, one row per component.
- `gpagerank` returns `(node, rank)`. Ranks sum to 1 over the whole graph.
  `top=N` returns only the N highest ranks.
- `gkhop`, `gkhop_count`, `gpath`, `gcomponents`, `gcomponents_count` and
  `gpagerank` use worker threads (`gpath` with `weighted=true` runs on one): `threads` (default: one per core of the node, 1 to 64;
  1 switches them off). A small search never starts a thread. The result does not
  depend on the number of threads. The threads run outside Vertica's resource
  pools and end before the function returns.

### Large results

A deep k-hop returns millions of rows, and most of the query time is spent
moving them. If you need numbers and not the nodes, ask for the counts:

    SELECT vgraph.gkhop_count(start, target, src, dst, del, weight, ver, snapshot_id
                              USING PARAMETERS graph='contacts', start=12345, depth=9) OVER()
    FROM app.contacts_delta;        -- (start, hops, nodes): one row per hop count

It takes the same parameters as `gkhop`. On the 100 million row test it
answers in 0.046 s where `gkhop` needs 0.35 s to return 3.3 million rows.
The same holds for whole-graph results: `gcomponents_count` and
`gpagerank ... top=N` return the answer, not one row per node.

If you need the nodes, keep them in the database and join there:

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

## 6. Reference

### Functions and procedures

| Function / procedure        | Purpose |
|-----------------------------|---------|
| `vgraph.gkhop`              | k-hop neighbourhood (BFS) |
| `vgraph.gkhop_count`        | the same search, returns only the number of nodes per hop count |
| `vgraph.gpath`              | shortest path, by hops or by weight |
| `vgraph.gcomponents`        | connected components |
| `vgraph.gcomponents_count`  | the same, returns one row per component with its size |
| `vgraph.gpagerank`          | PageRank |
| `vgraph.register_graph`     | register an edge table as a graph |
| `vgraph.refresh_graph`      | rebuild the snapshot and load it on every node |
| `vgraph.schedule_refresh`   | run refresh_graph on a cron schedule |
| `vgraph.load_all`           | load the active snapshot on every node again (cache repair) |
| `vgraph.status`             | size and read time of the pending changes |
| `vgraph.unregister_graph`   | remove a graph |
| `vgraph.ginfo`, `vgraph.gversion` | what every node has cached; library version |
| `vgraph.gbuild`, `vgraph.gbuild_mapped`, `vgraph.gbuild_header`, `vgraph.gload`, `vgraph.gnode` | building blocks used by the procedures |

### Large graphs: the streaming build

`refresh_graph` builds the snapshot in one of two ways and writes the same
file, byte for byte, either way:

- **In memory** (`gbuild`): fastest. The function holds all edges: about 16
  bytes per edge row plus 32 per node, outside Vertica's resource pools.
- **Streaming** (`gbuild_mapped`): Vertica builds a node map and the mapped,
  sorted edges in tables, which spill to disk under its own memory management.
  The function writes the file section by section and needs one 8 MB buffer,
  whatever the graph size. About 4 times slower at 100 million rows (82 s
  against 21 s for the whole refresh).

The choice is automatic: rows x 24 bytes is compared with
`vgraph.manifest.build_memory_mb` (default 4096).

    UPDATE vgraph.manifest SET build_memory_mb = 16384 WHERE graph = 'contacts';   -- allow 16 GB in memory
    UPDATE vgraph.manifest SET build_memory_mb = 0     WHERE graph = 'contacts';   -- always stream
    COMMIT;

**Tables that store both directions.** Many contact tables store a contact
a-b twice, as the rows (a, b) and (b, a). The snapshot then needs no reverse
index and is about half the size: the "in" lists equal the "out" lists. Both
builds find this out by themselves. For the streaming build the proof is a join
of all edges with themselves, which takes minutes at a billion rows. If you
know your table, declare it and the join is skipped:

    UPDATE vgraph.manifest SET both_directions = TRUE WHERE graph = 'contacts'; COMMIT;
    -- or: scripts/register.sh ... --both_directions

A cheap test (one scan, order-independent hash sums) still runs at every
refresh. If the declaration is wrong, `refresh_graph` fails with a clear
message and the previous snapshot stays active. This is not the `directed`
parameter: `directed = false` is for tables that store a contact once, and the
build adds the reverse rows itself.

The streaming build creates and drops the tables `vgraph.build_<graph>_map`
and `vgraph.build_<graph>_edges`. It needs disk space for one copy of the
consolidated edges and for Vertica's sort.

### Memory

Queries map the snapshot file read-only. All concurrent queries share one copy
in the operating system's page cache, and the file may be larger than free
memory. Private memory per query: gkhop 1 bit per node (3 bits in a search that
covers most of the graph); gpath a small hash table (8 bytes per node only when
a search visits a large part of the graph); gcomponents 12 bytes per node;
gpagerank 32. The in-memory build holds about 16 bytes per edge plus 32 per
node in the UDx process; the streaming build holds 8 MB.

### Why not WITH RECURSIVE

Vertica unrolls a recursive WITH into a fixed number of levels: the
configuration parameter `WithClauseRecursionLimit`, 8 by default. Deeper levels
are dropped without an error or warning: a recursive query that should count to
50 returns 9 as its deepest level. A recursive query also follows every path,
not every node, which explodes on a graph with cycles. The SQL reference used
in the tests and measurements below is therefore a stored procedure that runs
one INSERT per level.

### Tests

    make test                              # engine unit tests, no Vertica needed
    make bench                             # engine build and BFS timings, no Vertica needed
    tests/sql/test_khop_reference.sh       # gkhop without snapshot against the BFS procedure of the earlier repository
    tests/sql/test_snapshot.sh             # build, load on every node, all query functions, cache rules
    tests/sql/test_freshness.sh            # journal, exactness without refresh, refresh, repair, schedule
    tests/sql/test_build_paths.sh          # in-memory and streaming build write identical files

The integration tests need https://github.com/mogomo/vertica-graphs-and-trees
cloned as a sibling directory. They drop and recreate schema `GRAPH_DEMO`
(`--schema=NAME` picks another, `--keep` reuses it). Run them on a database node.

### Not supported

- More than 2^32 - 2 nodes in one graph.
- Node ids other than Vertica INT.
- Parallel execution of one query over several nodes: a query runs on the node that started it
  (on that node it uses all cores).
- Two refreshes of the same graph at the same time.
- Ties: two rows for the same edge with the same version have no defined order.
- Big-endian hosts.
- Cypher and the property-graph model: properties are columns in your own
  tables, reached by a join.

### Possible later: a changes table

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

More detail: [docs/design.md](docs/design.md), [docs/format.md](docs/format.md).

## License

MIT. Author: Mo (github.com/mogomo).

## Disclaimer

This repository is a demo. It is not a product of, and is not endorsed or
supported by, Rocket Software, Vertica, Neo4j or any other company.

The software and everything in this repository, including the measurements, are
provided "as is", without warranty of any kind, as the MIT license says, and the
author cannot take responsibility for how it is used. Please try it on your own
systems and data before you rely on it, and take special care with unfenced
mode, where the code runs inside the Vertica process.

The benchmark results describe one data shape, on one machine, with the software
versions and settings named above. They are not a statement about any product
in general, and your results will differ. The scripts are included so that you
can repeat every measurement yourself.

Vertica, Rocket Software, Neo4j, APOC, Graph Data Science, Cypher and all other
product and company names are trademarks or registered trademarks of their
respective owners. They are used here only to identify the products.
