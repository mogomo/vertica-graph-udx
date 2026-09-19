#!/usr/bin/env bash
# Integration test: the in-memory build (gbuild) and the streaming build
# (gbuild_mapped, chosen by vgraph.manifest.build_memory_mb = 0) must write the
# same snapshot file, byte for byte, for every kind of graph.
#
#   tests/sql/test_build_paths.sh [--rows=N] [--schema=NAME] [--cache_dir=DIR] [--keep] [--echo_only]
#
# Data comes from ../vertica-graphs-and-trees/graph_contacts_demo.sql (schema
# GRAPH_DEMO or --schema=NAME; dropped and recreated unless --keep is given).
# Run it on a database node: it compares the cache files of that node.
#
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD,
# VSQL_DATABASE from the environment.
set -uo pipefail

cd "$(dirname "$0")/../.."

ROWS=2000000
SCHEMA=GRAPH_DEMO
CACHE_DIR=/tmp/vgraph
KEEP=no
ECHO_ONLY=no
DEMO_SQL=../vertica-graphs-and-trees/graph_contacts_demo.sql

for arg in "$@"; do
    case "$arg" in
        --rows=*)      ROWS="${arg#--rows=}" ;;
        --schema=*)    SCHEMA="${arg#--schema=}" ;;
        --cache_dir=*) CACHE_DIR="${arg#--cache_dir=}" ;;
        --keep)        KEEP=yes ;;
        --echo_only)   ECHO_ONLY=yes ;;
        -h|--help)     sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "test_build_paths.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

PRE="ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '$CACHE_DIR';"
FAILED=0
run_sql() {
    if [ "$ECHO_ONLY" = yes ]; then echo "$PRE"; echo "$1"; return 0; fi
    printf '%s\n%s\n' "$PRE" "$1" | vsql -X -A -t -q 2>&1
}

if [ "$KEEP" = no ]; then
    if [ ! -f "$DEMO_SQL" ]; then
        echo "test_build_paths.sh: $DEMO_SQL not found. Clone vertica-graphs-and-trees as a sibling directory." >&2
        exit 1
    fi
    if [ "$ECHO_ONLY" = yes ]; then
        echo "sed -e 's/^\\\\set ROWS .*/\\\\set ROWS $ROWS/' -e 's/^\\\\set SCHEMA .*/\\\\set SCHEMA $SCHEMA/' $DEMO_SQL | vsql -X -q"
    else
        echo "== generating demo data: $ROWS rows in schema $SCHEMA"
        sed -e "s/^\\\\set ROWS .*/\\\\set ROWS $ROWS/" -e "s/^\\\\set SCHEMA .*/\\\\set SCHEMA $SCHEMA/" "$DEMO_SQL" | vsql -X -q -v ON_ERROR_STOP=1 > /dev/null || exit 1
    fi
fi

echo "== test tables"
run_sql "
DROP TABLE IF EXISTS $SCHEMA.bp_plain CASCADE;
DROP TABLE IF EXISTS $SCHEMA.bp_journal CASCADE;
-- one direction of every contact, with isolated ends and a self loop
CREATE TABLE $SCHEMA.bp_plain AS SELECT src_id AS src, dst_id AS dst FROM $SCHEMA.contact WHERE src_id <= dst_id;
-- a weighted journal with deletes and re-adds
CREATE TABLE $SCHEMA.bp_journal (src INT NOT NULL, dst INT NOT NULL, del BOOLEAN NOT NULL DEFAULT FALSE, weight FLOAT,
                                 ts TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP());
INSERT INTO $SCHEMA.bp_journal (src, dst, weight, ts) SELECT src_id, dst_id, (src_id % 7 + dst_id % 5 + 1) / 2.0, CLOCK_TIMESTAMP() - INTERVAL '2 days' FROM (SELECT DISTINCT src_id, dst_id FROM $SCHEMA.contact) c;
INSERT INTO $SCHEMA.bp_journal (src, dst, del, ts) SELECT src, dst, TRUE, CLOCK_TIMESTAMP() - INTERVAL '1 day' FROM $SCHEMA.bp_journal WHERE src % 50 = 0;
INSERT INTO $SCHEMA.bp_journal (src, dst, weight, ts) SELECT src, dst, 9.5, CLOCK_TIMESTAMP() - INTERVAL '12 hours' FROM $SCHEMA.bp_journal WHERE del AND src % 100 = 0;
COMMIT;" > /dev/null

# check NAME TABLE SRC DST OP WEIGHT DIRECTED VER
check() {
    local name="$1" tab="$2" src="$3" dst="$4" op="$5" w="$6" directed="$7" ver="$8"
    local out a b
    out=$(run_sql "
CALL vgraph.unregister_graph('bpmem'); CALL vgraph.unregister_graph('bpmap');
CALL vgraph.register_graph('bpmem', '$tab', '$src', '$dst', $op, $w, $directed, $ver, NULL);
CALL vgraph.register_graph('bpmap', '$tab', '$src', '$dst', $op, $w, $directed, $ver, NULL);
UPDATE vgraph.manifest SET build_memory_mb = 1000000 WHERE graph = 'bpmem';
UPDATE vgraph.manifest SET build_memory_mb = 0 WHERE graph = 'bpmap';
COMMIT;
CALL vgraph.refresh_graph('bpmem');
CALL vgraph.refresh_graph('bpmap');
SELECT 'ids ' || MAX(CASE WHEN graph = 'bpmem' THEN active_snapshot END) || ' ' || MAX(CASE WHEN graph = 'bpmap' THEN active_snapshot END)
       || ' edges ' || MAX(edge_count) FROM vgraph.manifest WHERE graph IN ('bpmem', 'bpmap');")
    if [ "$ECHO_ONLY" = yes ]; then echo "$out"; echo "cmp $CACHE_DIR/bpmem/<id>.vg $CACHE_DIR/bpmap/<id>.vg"; return 0; fi
    a=$(echo "$out" | sed -n 's/^ids \([0-9]*\) \([0-9]*\) .*/\1/p'); b=$(echo "$out" | sed -n 's/^ids \([0-9]*\) \([0-9]*\) .*/\2/p')
    if echo "$out" | grep -q "memory build" && echo "$out" | grep -q "streaming build" && [ -n "$a" ] && [ -n "$b" ] \
       && cmp -s "$CACHE_DIR/bpmem/$a.vg" "$CACHE_DIR/bpmap/$b.vg"; then
        echo "PASS  $name: $(stat -c %s "$CACHE_DIR/bpmap/$b.vg") identical bytes, $(echo "$out" | sed -n 's/^ids .* edges //p') edges"
    else
        echo "FAIL  $name"; echo "$out" | grep -v "^0$\|^1$\|registered\|unregistered\|is not registered" | head -6 | sed 's/^/      got: /'
        FAILED=$((FAILED + 1))
    fi
}

echo "== memory build against streaming build"
check "directed, both directions stored (no reverse index)" "$SCHEMA.contact"    src_id dst_id NULL    NULL       TRUE  NULL
check "directed, one direction stored (reverse index)"      "$SCHEMA.bp_plain"   src    dst    NULL    NULL       TRUE  NULL
check "undirected"                                          "$SCHEMA.bp_plain"   src    dst    NULL    NULL       FALSE NULL
check "weighted journal with deletes and re-adds"           "$SCHEMA.bp_journal" src    dst    "'del'" "'weight'" TRUE  "'ts'"
check "the same journal, unweighted"                        "$SCHEMA.bp_journal" src    dst    "'del'" NULL       TRUE  "'ts'"

run_sql "CALL vgraph.unregister_graph('bpmem'); CALL vgraph.unregister_graph('bpmap');
DROP TABLE IF EXISTS $SCHEMA.bp_plain CASCADE; DROP TABLE IF EXISTS $SCHEMA.bp_journal CASCADE;" > /dev/null

[ "$ECHO_ONLY" = yes ] && exit 0
if [ "$FAILED" -ne 0 ]; then echo "test_build_paths: $FAILED FAILED"; exit 1; fi
echo "test_build_paths: OK"
