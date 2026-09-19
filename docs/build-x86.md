# Build and install on x86_64, step by step

These steps were run as written on:

| Item      | Tested with |
|-----------|-------------|
| Hardware  | x86_64, 2 cores, 15 GB RAM per node |
| OS        | Red Hat Enterprise Linux 8.10 |
| Compiler  | g++ 8.5.0 (package `gcc-c++-8.5.0-28.el8_10`), GNU make 4.2.1 |
| Vertica   | 26.2.0-2, Eon mode, 3 nodes in one subcluster, with `/opt/vertica/sdk` |

The same steps work on a single-node database. They were also run on Rocky
Linux 9 on aarch64 with g++ 11.5 and Vertica 26.2.0-1, single node.

Any g++ with full C++17 support should do (g++ 7 or later). Older compilers
were not tested.

## 1. What you need

- A shell on **one** Vertica node, as the OS user that runs Vertica (usually
  `dbadmin`). You build and deploy from this one node only. `CREATE LIBRARY`
  copies the library to all other nodes.
- The Vertica C++ SDK on that node: `/opt/vertica/sdk/include/Vertica.h` must exist.
  It is installed with the Vertica server package.
- A database user who may create libraries: a superuser, or a user with the
  `UDXDEVELOPER` role enabled.
- Packages:

      sudo dnf install -y gcc-c++ make git

- On every node: a directory for the snapshot cache that the Vertica OS user
  may create and write. The default is `/tmp/vgraph`. Plan for the size of one
  or two snapshots (see docs/format.md; 100 million edges take 1.2 GB).

Check the tools:

    g++ --version        # 7 or later
    make --version
    ls /opt/vertica/sdk/include/Vertica.h

## 2. Get the code

    mkdir -p ~/vgraph && cd ~/vgraph
    git clone https://github.com/mogomo/vertica-graph-udx.git
    # only needed for the tests and the demo:
    git clone https://github.com/mogomo/vertica-graphs-and-trees.git
    cd vertica-graph-udx

The two directories must be siblings.

## 3. Connection settings

The scripts call `vsql` and take the connection from the environment. Nothing
is stored in the repository. On a node, host and port defaults are fine:

    export VSQL_USER=dbadmin
    export VSQL_PASSWORD='...'
    # only if needed: VSQL_HOST, VSQL_PORT, VSQL_DATABASE

    vsql -c "SELECT version();"

## 4. Build and run the unit tests

    make
    make test

`make` takes about 45 seconds on 2 cores and prints no warnings. The result is
`build/libvgraph.so`. `make test` needs no database and ends with

    All engine tests passed.

## 5. Deploy

    scripts/deploy.sh --echo_only     # shows what would run, changes nothing
    make deploy                       # fenced mode (default)

or, faster at run time but inside the Vertica process:

    make deploy FENCED=no

Deploy creates the library `vgraph`, the schema `vgraph` with the tables
`snapshot`, `manifest` and `probe`, the sequence `snapshot_seq`, the role
`vgraph_admin`, the functions and the stored procedures.
It can be run again at any time; snapshots and the manifest are kept. The
message `ROLLBACK 5403: User/role "vgraph_admin" already exists` on a second
run is expected.

At the end it prints the version and the mode of every function:

     library_version | format_version |           build_flags
    -----------------+----------------+---------------------------------
     0.1.0           |              1 | -O2 -std=c++17 x86_64 g++-8.5.0

     function_name | is_fenced
    ---------------+-----------
     gbuild        | t
     ...            (one line per function)

## 6. Check the installation

    tests/sql/test_snapshot.sh --rows=2000000

    tests/sql/test_freshness.sh --rows=2000000
    tests/sql/test_build_paths.sh --keep

The first one creates schema `GRAPH_DEMO` (it is dropped first; pick another name with
`--schema=NAME`), builds a snapshot of 2 million edges, loads it on every node
and checks all query functions against a SQL reference. Use
`--cache_dir=/some/dir` if `/tmp/vgraph` is not wanted. Expected last lines:

    test_snapshot: OK
    test_freshness: OK
    test_build_paths: OK

The second test registers a journal table as a graph, journals 1000 adds and
1000 deletes and checks that queries stay exact without a refresh.

On a cluster the line `PASS  gload on every node` is the important one: it
compares the nodes that loaded the cache with the nodes that are UP.

See what every node has cached:

    SELECT vgraph.ginfo() OVER(PARTITION NODES) FROM vgraph.probe;

## 7. Remove

    make undeploy

removes the library and the functions. The schema `vgraph` with its snapshots
stays. To remove everything:

    DROP SCHEMA vgraph CASCADE;
    DROP ROLE vgraph_admin;

and delete the cache directory (`/tmp/vgraph` by default) on every node.

## Problems seen

- `make: g++: Command not found`: install `gcc-c++`.
- `Vertica.h: No such file or directory`: the SDK is somewhere else. Build with
  `make SDK_HOME=/path/to/sdk`.
- `deploy.sh: ... libvgraph.so not found`: run `make` first.
- `ERROR ... Permission denied` from `CREATE LIBRARY`: the database user is no
  superuser and has no `UDXDEVELOPER` role.
- `no snapshot cache for graph '...' in /tmp/vgraph: run gload`: this node has
  no cache file yet, or another `cache_dir` was used at load time.
