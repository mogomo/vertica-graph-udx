#!/usr/bin/env bash
# Benchmark: snapshot build and load, gkhop and gkhop_count against the SQL BFS
# procedure of the demo repository, fenced and unfenced.
#
#   scripts/benchmark.sh [--rows=N] [--depths="3 6 9"] [--modes="fenced unfenced"] [--streaming]
#                        [--schema=NAME] [--cache_dir=DIR] [--keep] [--echo_only]
#
#   --rows       contact rows to generate (default 100000000; people = rows / 6)
#   --streaming  also time the streaming build (always used above vgraph.manifest.build_memory_mb)
#   --keep       reuse the data of an earlier run
#
# Data and the SQL reference come from ../vertica-graphs-and-trees/graph_contacts_demo.sql.
# Schema GRAPH_DEMO (or --schema) is dropped and recreated unless --keep is given. The graph is
# called vgbench. The library is redeployed for every mode and left in fenced mode.
# Times are server times from v_monitor.query_requests, found by LABEL. Results are counted inside
# the database, not sent to the client. Every gkhop result is compared with the SQL BFS.
#
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE from the environment.
set -euo pipefail

cd "$(dirname "$0")/.."
ROWS=100000000 DEPTHS="3 6 9" MODES="fenced unfenced" STREAMING=no SCHEMA=GRAPH_DEMO CACHE_DIR=/tmp/vgraph KEEP=no ECHO_ONLY=no
DEMO_SQL=../vertica-graphs-and-trees/graph_contacts_demo.sql
for arg in "$@"; do
    case "$arg" in
        --rows=*)      ROWS="${arg#*=}" ;;
        --depths=*)    DEPTHS="${arg#*=}" ;;
        --modes=*)     MODES="${arg#*=}" ;;
        --streaming)   STREAMING=yes ;;
        --schema=*)    SCHEMA="${arg#*=}" ;;
        --cache_dir=*) CACHE_DIR="${arg#*=}" ;;
        --keep)        KEEP=yes ;;
        --echo_only)   ECHO_ONLY=yes ;;
        -h|--help)     sed -n '2,19p' "$0"; exit 0 ;;
        *) echo "benchmark.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done
[ -f "$DEMO_SQL" ] || { echo "benchmark.sh: $DEMO_SQL not found. Clone vertica-graphs-and-trees as a sibling directory." >&2; exit 1; }

PRE="ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '$CACHE_DIR';"
REST="NULL::INT, NULL::INT, NULL::INT, NULL::BOOLEAN, NULL::FLOAT, NULL::INT, NULL::INT"
MAXD=$(echo $DEPTHS | tr ' ' '\n' | sort -n | tail -1)
RESULTS=$(mktemp)
trap 'rm -f "$RESULTS"' EXIT

sql() { printf '%s\n%s\n' "$PRE" "$1" | vsql -X -A -t -q -v ON_ERROR_STOP=1; }

# One refresh. Prints "<label> build_ms load_ms".
refresh_sql() {   # budget_mb
    cat <<SQLEOF
UPDATE vgraph.manifest SET build_memory_mb = $1 WHERE graph = 'vgbench'; COMMIT;
CALL vgraph.refresh_graph('vgbench');
SELECT 'build_ms ' || SUM(CASE WHEN request_label = 'vgraph_build' THEN request_duration_ms END) ||
       ' load_ms ' || SUM(CASE WHEN request_label = 'vgraph_load' THEN request_duration_ms END)
FROM v_monitor.query_requests WHERE session_id = CURRENT_SESSION() AND request_label IN ('vgraph_build', 'vgraph_load');
SQLEOF
}

query_sql() {   # mode
    local s="CREATE LOCAL TEMP TABLE one_row (x INT) ON COMMIT PRESERVE ROWS; INSERT INTO one_row VALUES (1);
SET SEARCH_PATH TO $SCHEMA, public;
\\o /dev/null
CALL bfs(1, $((MAXD + 1)), 50);
\\o
"
    for d in $DEPTHS; do
        s+="
SELECT /*+LABEL(vgq_count_$d)*/ SUM(nodes) FROM (SELECT vgraph.gkhop_count(1, $REST USING PARAMETERS graph='vgbench', depth=$d) OVER() FROM one_row) r;
DROP TABLE IF EXISTS got;
CREATE LOCAL TEMP TABLE got ON COMMIT PRESERVE ROWS AS /*+LABEL(vgq_khop_$d)*/
SELECT vgraph.gkhop(1, $REST USING PARAMETERS graph='vgbench', depth=$d) OVER() FROM one_row KSAFE 0;
SELECT 'depth $d nodes ' || g.n || ' differences ' || x.n FROM (SELECT COUNT(*) AS n FROM got) g CROSS JOIN
  (SELECT COUNT(*) AS n FROM ((SELECT node, hops FROM got EXCEPT SELECT person_id, lvl - 1 FROM reach WHERE lvl <= $d + 1)
     UNION ALL (SELECT person_id, lvl - 1 FROM reach WHERE lvl <= $d + 1 EXCEPT SELECT node, hops FROM got)) u) x;
SELECT 'depth $d count_ms ' || MAX(CASE WHEN request_label = 'vgq_count_$d' THEN request_duration_ms END) ||
       ' khop_ms ' || MAX(CASE WHEN request_label = 'vgq_khop_$d' THEN request_duration_ms END) ||
       ' sql_ms ' || (SELECT SUM(request_duration_ms) FROM v_monitor.query_requests WHERE session_id = CURRENT_SESSION()
                      AND request_label LIKE 'g_r1_hop%' AND RIGHT(request_label, 2)::INT <= $d + 1)
FROM v_monitor.query_requests WHERE session_id = CURRENT_SESSION() AND request_label IN ('vgq_count_$d', 'vgq_khop_$d');"
    done
    echo "$s"
}

if [ "$ECHO_ONLY" = yes ]; then
    [ "$KEEP" = yes ] || echo "sed -e 's/^\\\\set ROWS .*/\\\\set ROWS $ROWS/' -e 's/^\\\\set SCHEMA .*/\\\\set SCHEMA $SCHEMA/' $DEMO_SQL | vsql -X -q"
    echo "CALL vgraph.register_graph('vgbench', '$SCHEMA.contact', 'src_id', 'dst_id', NULL, NULL, TRUE, NULL, NULL);"
    for mode in $MODES; do
        echo "make deploy FENCED=$([ "$mode" = fenced ] && echo yes || echo no)"
        refresh_sql 1000000000; [ "$STREAMING" = yes ] && refresh_sql 0; query_sql "$mode"
    done
    echo "make deploy FENCED=yes"; echo "CALL vgraph.unregister_graph('vgbench');"
    exit 0
fi

if [ "$KEEP" = no ]; then
    echo "== generating $ROWS rows in schema $SCHEMA"
    sed -e "s/^\\\\set ROWS .*/\\\\set ROWS $ROWS/" -e "s/^\\\\set SCHEMA .*/\\\\set SCHEMA $SCHEMA/" "$DEMO_SQL" | vsql -X -q -v ON_ERROR_STOP=1 > /dev/null
fi
make >/dev/null
for mode in $MODES; do
    echo "== mode: $mode"
    make deploy FENCED=$([ "$mode" = fenced ] && echo yes || echo no) > /dev/null
    sql "CALL vgraph.unregister_graph('vgbench');" > /dev/null 2>&1 || true
    sql "CALL vgraph.register_graph('vgbench', '$SCHEMA.contact', 'src_id', 'dst_id', NULL, NULL, TRUE, NULL, NULL);" > /dev/null
    if [ "$STREAMING" = yes ]; then
        out=$(sql "$(refresh_sql 0)" | grep '^build_ms'); echo "   streaming build: $out"
        echo "$mode streaming $out" >> "$RESULTS"
    fi
    out=$(sql "$(refresh_sql 1000000000)" | grep '^build_ms'); echo "   memory build:    $out"
    echo "$mode memory $out" >> "$RESULTS"
    sql "$(query_sql "$mode")" | grep '^depth' | sed "s/^/$mode /" | tee -a "$RESULTS" | sed 's/^/   /'
done
make deploy FENCED=yes > /dev/null
SIZE=$(sql "SELECT (SUM(LENGTH(chunk)) / 1048576)::INT FROM vgraph.snapshot WHERE graph = 'vgbench' AND snapshot_id = (SELECT active_snapshot FROM vgraph.manifest WHERE graph = 'vgbench');")
COUNTS=$(sql "SELECT node_count || ' nodes, ' || edge_count || ' edges' FROM vgraph.manifest WHERE graph = 'vgbench';")
sql "CALL vgraph.unregister_graph('vgbench');" > /dev/null

if grep -q 'differences [1-9]' "$RESULTS"; then echo "benchmark.sh: gkhop and the SQL BFS returned different results" >&2; exit 1; fi

sec() { awk -v ms="$1" 'BEGIN { if (ms == "") print "-"; else if (ms < 1000) printf "%d ms", ms; else printf "%.1f s", ms / 1000 }'; }
field() { grep "^$1 " "$RESULTS" | grep "$2" | head -1 | sed -n "s/.* $3 \([0-9]*\).*/\1/p"; }
echo
echo "$ROWS rows: $COUNTS, snapshot $SIZE MB. gkhop results equal the SQL BFS at every depth."
echo
printf '| Step |'; for mode in $MODES; do printf ' %s |' "$mode"; done; echo
printf '|---|'; for mode in $MODES; do printf -- '---:|'; done; echo
row() { printf '| %s |' "$1"; for mode in $MODES; do printf ' %s |' "$(sec "$(field "$mode" "$2" "$3")")"; done; echo; }
row "build the snapshot in memory" " memory " build_ms
[ "$STREAMING" = yes ] && row "build the snapshot, streaming" " streaming " build_ms
row "load it on all nodes" " memory " load_ms
for d in $DEPTHS; do
    row "gkhop_count depth $d" "depth $d count_ms" count_ms
    row "gkhop depth $d, nodes into a table ($(field "$(echo $MODES | cut -d' ' -f1)" "depth $d nodes" nodes) nodes)" "depth $d count_ms" khop_ms
    row "SQL BFS procedure, $d hops" "depth $d count_ms" sql_ms
done
