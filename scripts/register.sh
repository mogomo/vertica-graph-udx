#!/usr/bin/env bash
# Register an edge table as a graph.
#
#   scripts/register.sh --graph=NAME --table=SCHEMA.TABLE --src=COL --dst=COL
#                       [--op=COL] [--weight=COL] [--ver=COL] [--margin=N] [--undirected]
#                       [--both_directions] [--echo_only]
#
#   --op      delete flag column: BOOLEAN (true = deleted) or INT (+1 / -1)
#   --ver     version column: TIMESTAMPTZ DEFAULT CLOCK_TIMESTAMP() (recommended), TIMESTAMP or INT
#   --margin  overlap of the changes: seconds for a timestamp version (default 60), units for an INT version
#   --both_directions  declare that the table stores every edge in both directions (a contact a-b as the
#             rows a,b and b,a). The build of a large graph then skips its exact test of that. A wrong
#             declaration is caught by a cheap test at the next refresh.
#
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE from the environment.
set -euo pipefail

GRAPH= TABLE= SRC= DST= OP=NULL WEIGHT=NULL VER=NULL MARGIN=NULL DIRECTED=TRUE BOTH=no ECHO_ONLY=no
q() { printf "'%s'" "$1"; }
for arg in "$@"; do
    case "$arg" in
        --graph=*)   GRAPH="${arg#*=}" ;;
        --table=*)   TABLE="${arg#*=}" ;;
        --src=*)     SRC="${arg#*=}" ;;
        --dst=*)     DST="${arg#*=}" ;;
        --op=*)      OP=$(q "${arg#*=}") ;;
        --weight=*)  WEIGHT=$(q "${arg#*=}") ;;
        --ver=*)     VER=$(q "${arg#*=}") ;;
        --margin=*)  MARGIN="${arg#*=}" ;;
        --undirected) DIRECTED=FALSE ;;
        --both_directions) BOTH=yes ;;
        --echo_only) ECHO_ONLY=yes ;;
        -h|--help)   sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "register.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done
if [ -z "$GRAPH" ] || [ -z "$TABLE" ] || [ -z "$SRC" ] || [ -z "$DST" ]; then
    echo "register.sh: --graph, --table, --src and --dst are required" >&2; exit 2
fi
case "$MARGIN" in NULL|[0-9]*) ;; *) echo "register.sh: --margin must be a number" >&2; exit 2 ;; esac

SQL="CALL vgraph.register_graph($(q "$GRAPH"), $(q "$TABLE"), $(q "$SRC"), $(q "$DST"), $OP, $WEIGHT, $DIRECTED, $VER, $MARGIN);"
if [ "$BOTH" = yes ]; then
    case "$GRAPH" in *[!A-Za-z0-9_]*) echo "register.sh: --graph must be letters, digits or underscores" >&2; exit 2 ;; esac
    SQL="$SQL UPDATE vgraph.manifest SET both_directions = TRUE WHERE graph = $(q "$GRAPH"); COMMIT;"
fi
if [ "$ECHO_ONLY" = yes ]; then echo "vsql -X -c \"$SQL\""; exit 0; fi
vsql -X -v ON_ERROR_STOP=1 -c "$SQL"
