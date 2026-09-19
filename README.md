# vertica-graph-udx (vgraph)

A Vertica C++ UDx library for graph traversal on tables Vertica already
stores: k-hop neighbourhoods, shortest paths, connected components, PageRank.

Status: early development. Only `gversion` exists today. The list below says
what is planned and what works.

| Function      | Purpose                                  | Status  |
|---------------|------------------------------------------|---------|
| `gversion`    | library version, format version, flags   | works   |
| `gkhop`       | k-hop neighbourhood (BFS)                | planned |
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

## Not supported

- Node ids other than Vertica INT (int64). Keep business keys in your own
  nodes table.
- Big-endian hosts.

## License

MIT. Author: Mo (github.com/mogomo).
