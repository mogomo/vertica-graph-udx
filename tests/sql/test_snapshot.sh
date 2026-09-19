#!/usr/bin/env bash
# Integration test of the snapshot path: gbuild -> vgraph.snapshot -> gload ->
# ginfo -> gkhop, gpath, gcomponents, gpagerank reading the node cache.
#
#   tests/sql/test_snapshot.sh [--rows=N] [--keep] [--echo_only]
#
# Data and reference come from ../vertica-graphs-and-trees/graph_contacts_demo.sql
# (schema GRAPH_DEMO, dropped and recreated unless --keep is given).
# The test graph is called vgtest. Its snapshot rows are removed at the end.
#
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD,
# VSQL_DATABASE from the environment.
set -uo pipefail

cd "$(dirname "$0")/../.."

ROWS=2000000
KEEP=no
ECHO_ONLY=no
DEMO_SQL=../vertica-graphs-and-trees/graph_contacts_demo.sql
NULLS6="NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT"
NULLS5="NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::INT"

for arg in "$@"; do
    case "$arg" in
        --rows=*)    ROWS="${arg#--rows=}" ;;
        --keep)      KEEP=yes ;;
        --echo_only) ECHO_ONLY=yes ;;
        -h|--help)   sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "test_snapshot.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

FAILED=0
# run_sql NAME SQL: prints the vsql output, one test per call.
run_sql() {
    if [ "$ECHO_ONLY" = yes ]; then echo "-- $1"; echo "$2"; return 0; fi
    echo "$2" | vsql -X -A -t -q 2>&1
}
# expect NAME PATTERN SQL: the output must contain PATTERN.
expect() {
    local out
    out=$(run_sql "$1" "$3")
    if [ "$ECHO_ONLY" = yes ]; then echo "$out"; return 0; fi
    if echo "$out" | grep -q -- "$2"; then
        echo "PASS  $1"
    else
        echo "FAIL  $1"; echo "      wanted: $2"; echo "$out" | sed 's/^/      got: /' | head -5
        FAILED=$((FAILED + 1))
    fi
}

if [ "$KEEP" = no ]; then
    if [ ! -f "$DEMO_SQL" ]; then
        echo "test_snapshot.sh: $DEMO_SQL not found. Clone vertica-graphs-and-trees as a sibling directory." >&2
        exit 1
    fi
    if [ "$ECHO_ONLY" = yes ]; then
        echo "sed 's/^\\\\set ROWS .*/\\\\set ROWS $ROWS/' $DEMO_SQL | vsql -X -q"
    else
        echo "== generating demo data: $ROWS rows in schema GRAPH_DEMO"
        sed "s/^\\\\set ROWS .*/\\\\set ROWS $ROWS/" "$DEMO_SQL" | vsql -X -q -v ON_ERROR_STOP=1 > /dev/null || exit 1
    fi
fi

echo "== build and load"
expect "gbuild into vgraph.snapshot" "^chunks: [1-9]" "
DELETE FROM vgraph.snapshot WHERE graph = 'vgtest';
INSERT INTO vgraph.snapshot
SELECT 'vgtest', 1, chunk_no, chunk FROM (
  SELECT gbuild(src, dst, weight, max_epoch USING PARAMETERS graph='vgtest', directed=true) OVER(ORDER BY src, dst)
  FROM (SELECT src_id AS src, dst_id AS dst, NULL::FLOAT AS weight,
               (SELECT MAX(epoch) FROM GRAPH_DEMO.contact) AS max_epoch
        FROM GRAPH_DEMO.contact) e) b;
COMMIT;
SELECT 'chunks: ' || COUNT(*) FROM vgraph.snapshot WHERE graph = 'vgtest';"

expect "gload on every node" "^loaded on all nodes" "
SELECT CASE WHEN l.loaded = u.up THEN 'loaded on all nodes' ELSE 'loaded on ' || l.loaded || ' of ' || u.up || ' nodes' END
FROM (SELECT COUNT(DISTINCT node_name) AS loaded
      FROM (SELECT gload(chunk_no, chunk USING PARAMETERS graph='vgtest', snapshot_id=1) OVER(PARTITION NODES)
            FROM vgraph.snapshot WHERE graph = 'vgtest' AND snapshot_id = 1) g WHERE status = 'loaded') l
CROSS JOIN (SELECT COUNT(*) AS up FROM nodes WHERE node_state = 'UP') u;"

expect "gload again (idempotent)" "^loaded$" "
SELECT DISTINCT status FROM (SELECT gload(chunk_no, chunk USING PARAMETERS graph='vgtest', snapshot_id=1) OVER(PARTITION NODES)
      FROM vgraph.snapshot WHERE graph = 'vgtest' AND snapshot_id = 1) l;"

expect "ginfo: counts and epoch match the table on every node" "^ginfo ok" "
SELECT CASE WHEN i.nodes_reporting = u.up AND i.min_edges = t.edges AND i.max_edges = t.edges
             AND i.min_epoch = ep.max_epoch AND i.all_loaded = 1
            THEN 'ginfo ok' ELSE 'ginfo mismatch: ' || i.nodes_reporting || ' of ' || u.up || ' nodes, edges ' ||
                 i.min_edges || '..' || i.max_edges || ' vs ' || t.edges END
FROM (SELECT COUNT(DISTINCT node_name) AS nodes_reporting, MIN(edge_count) AS min_edges, MAX(edge_count) AS max_edges,
             MIN(max_epoch) AS min_epoch, MIN(loaded::INT) AS all_loaded
      FROM (SELECT ginfo(USING PARAMETERS graph='vgtest') OVER(PARTITION NODES) FROM vgraph.probe) g) i
CROSS JOIN (SELECT COUNT(*) AS up FROM nodes WHERE node_state = 'UP') u
CROSS JOIN (SELECT COUNT(*) AS edges FROM (SELECT DISTINCT src_id, dst_id FROM GRAPH_DEMO.contact) d) t
CROSS JOIN (SELECT MAX(epoch) AS max_epoch FROM GRAPH_DEMO.contact) ep;"

echo "== gkhop against the demo BFS"
KHOP="SET SEARCH_PATH TO GRAPH_DEMO, public;
\\o /dev/null
CALL bfs(1, 10, 50);
\\o
"
for d in 1 3 6 9; do
KHOP+="
DROP TABLE IF EXISTS got;
CREATE LOCAL TEMP TABLE got ON COMMIT PRESERVE ROWS AS
SELECT gkhop(1, $NULLS6 USING PARAMETERS graph='vgtest', depth=$d) OVER() FROM vgraph.probe;
SELECT 'depth $d: ' || g.nodes || ' nodes, ' || d.differences || ' differences' ||
       CASE WHEN d.differences = 0 AND g.nodes > 0 THEN ' ok' ELSE ' WRONG' END
FROM (SELECT COUNT(*) AS nodes FROM got) g
CROSS JOIN (SELECT COUNT(*) AS differences
      FROM ((SELECT node, hops FROM got EXCEPT SELECT person_id, lvl - 1 FROM reach WHERE lvl <= $d + 1)
            UNION ALL
            (SELECT person_id, lvl - 1 FROM reach WHERE lvl <= $d + 1 EXCEPT SELECT node, hops FROM got)) x) d;
"
done
KHOP+="
DROP TABLE IF EXISTS got;
CREATE LOCAL TEMP TABLE got ON COMMIT PRESERVE ROWS AS
SELECT gkhop(NULL::INT, $NULLS6 USING PARAMETERS graph='vgtest', depth=6, exact=true, start=1) OVER() FROM vgraph.probe;
SELECT 'exact: ' || COUNT(*) || ' differences' || CASE WHEN COUNT(*) = 0 THEN ' ok' ELSE ' WRONG' END
FROM ((SELECT node FROM got EXCEPT SELECT person_id FROM reach WHERE lvl = 7)
      UNION ALL (SELECT person_id FROM reach WHERE lvl = 7 EXCEPT SELECT node FROM got)) x;

-- gpath to a person at level 7: 7 rows, from 1 to the target, every step is a stored contact.
DROP TABLE IF EXISTS path;
CREATE LOCAL TEMP TABLE path ON COMMIT PRESERVE ROWS AS
SELECT gpath(1, t.target, $NULLS5 USING PARAMETERS graph='vgtest') OVER()
FROM (SELECT MIN(person_id) AS target FROM reach WHERE lvl = 7) t;
SELECT 'gpath: ' || COUNT(*) || ' rows, ' || SUM(CASE WHEN c.src_id IS NULL AND p.hop_no > 0 THEN 1 ELSE 0 END) || ' broken steps' ||
       CASE WHEN COUNT(*) = 7 AND MIN(p.node) = 1 AND SUM(CASE WHEN c.src_id IS NULL AND p.hop_no > 0 THEN 1 ELSE 0 END) = 0
             AND MAX(CASE WHEN p.hop_no = 6 THEN p.node END) = MAX(p.target) THEN ' ok' ELSE ' WRONG' END
FROM path p LEFT JOIN path prev ON prev.hop_no = p.hop_no - 1
     LEFT JOIN (SELECT DISTINCT src_id, dst_id FROM contact) c ON c.src_id = prev.node AND c.dst_id = p.node;
"
if [ "$ECHO_ONLY" = yes ]; then
    run_sql "gkhop and gpath against the reference" "$KHOP"
else
    OUT=$(run_sql "khop" "$KHOP")
    echo "$OUT" | sed 's/^/      /'
    if [ "$(echo "$OUT" | grep -c ' ok$')" -eq 6 ]; then echo "PASS  gkhop depths 1 3 6 9, exact, gpath"
    else echo "FAIL  gkhop / gpath against the reference"; FAILED=$((FAILED + 1)); fi
fi

echo "== gcomponents and gpagerank"
expect "gcomponents: one row per node, no edge crosses components, component = smallest id" "^components ok" "
CREATE LOCAL TEMP TABLE comp ON COMMIT PRESERVE ROWS AS
SELECT gcomponents(NULL::INT, $NULLS6 USING PARAMETERS graph='vgtest') OVER() FROM vgraph.probe;
SELECT CASE WHEN n.nodes = i.node_count AND x.crossing = 0 AND m.wrong_name = 0
            THEN 'components ok' ELSE 'components WRONG: ' || n.nodes || '/' || i.node_count || ' nodes, ' ||
                 x.crossing || ' crossing edges, ' || m.wrong_name || ' wrong names' END
FROM (SELECT COUNT(*) AS nodes FROM comp) n
CROSS JOIN (SELECT MAX(node_count) AS node_count FROM (SELECT ginfo(USING PARAMETERS graph='vgtest') OVER(PARTITION NODES) FROM vgraph.probe) g) i
CROSS JOIN (SELECT COUNT(*) AS crossing FROM GRAPH_DEMO.contact e JOIN comp a ON a.node = e.src_id JOIN comp b ON b.node = e.dst_id
            WHERE a.component <> b.component) x
CROSS JOIN (SELECT COUNT(*) AS wrong_name FROM (SELECT component, MIN(node) AS smallest FROM comp GROUP BY 1) s
            WHERE component <> smallest) m;"

expect "gpagerank: one row per node, ranks sum to 1" "^pagerank ok" "
SELECT CASE WHEN COUNT(*) = MAX(i.node_count) AND ABS(SUM(rank) - 1) < 1e-6 AND MIN(rank) > 0
            THEN 'pagerank ok' ELSE 'pagerank WRONG: ' || COUNT(*) || ' rows, sum ' || SUM(rank) END
FROM (SELECT gpagerank(NULL::INT, $NULLS6 USING PARAMETERS graph='vgtest', iterations=10) OVER() FROM vgraph.probe) r
CROSS JOIN (SELECT MAX(node_count) AS node_count FROM (SELECT ginfo(USING PARAMETERS graph='vgtest') OVER(PARTITION NODES) FROM vgraph.probe) g) i;"

echo "== cache rules"
expect "session parameter cache_dir is used" "no snapshot cache for graph 'vgtest' in /tmp/vgraph_not_there" "
ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '/tmp/vgraph_not_there';
SELECT gkhop(1, $NULLS6 USING PARAMETERS graph='vgtest', depth=1) OVER() FROM vgraph.probe;"

expect "function parameter cache_dir wins over the session parameter" "^found [1-9]" "
ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '/tmp/vgraph_not_there';
SELECT 'found ' || COUNT(*) FROM (SELECT gkhop(1, $NULLS6 USING PARAMETERS graph='vgtest', depth=1, cache_dir='/tmp/vgraph') OVER() FROM vgraph.probe) r;"

expect "stale cache is refused" "snapshot cache stale on .*: run gload" "
SELECT gkhop(1, $NULLS5, e.newer USING PARAMETERS graph='vgtest', depth=1) OVER()
FROM (SELECT MAX(epoch) + 1000000 AS newer FROM GRAPH_DEMO.contact) e;"

expect "unknown graph is refused" "no snapshot cache for graph 'vgtest_none'" "
SELECT gkhop(1, $NULLS6 USING PARAMETERS graph='vgtest_none', depth=1) OVER() FROM vgraph.probe;"

expect "bad graph name is refused" "is not valid" "
SELECT gkhop(1, $NULLS6 USING PARAMETERS graph='../etc', depth=1) OVER() FROM vgraph.probe;"

run_sql "cleanup" "DELETE FROM vgraph.snapshot WHERE graph = 'vgtest'; COMMIT;" > /dev/null

[ "$ECHO_ONLY" = yes ] && exit 0
if [ "$FAILED" -ne 0 ]; then echo "test_snapshot: $FAILED FAILED"; exit 1; fi
echo "test_snapshot: OK"
