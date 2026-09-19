#!/usr/bin/env bash
# Integration test of the freshness model: journal, delta view, overlay,
# refresh_graph, load_all, status, schedule_refresh, unregister_graph.
#
#   tests/sql/test_freshness.sh [--rows=N] [--schema=NAME] [--cache_dir=DIR] [--keep] [--echo_only]
#
# Data and reference come from ../vertica-graphs-and-trees/graph_contacts_demo.sql
# (schema GRAPH_DEMO or --schema=NAME; dropped and recreated unless --keep is given).
# The test adds a journal table <schema>.journal and registers it as graph vgfresh.
# Every journaled change is also applied to the demo's contact table, so the
# demo's BFS procedure stays the reference.
# Run it on a database node: one test removes a local cache file.
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
G=vgfresh

for arg in "$@"; do
    case "$arg" in
        --rows=*)      ROWS="${arg#--rows=}" ;;
        --schema=*)    SCHEMA="${arg#--schema=}" ;;
        --cache_dir=*) CACHE_DIR="${arg#--cache_dir=}" ;;
        --keep)        KEEP=yes ;;
        --echo_only)   ECHO_ONLY=yes ;;
        -h|--help)     sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "test_freshness.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

active_snapshot() { printf "SELECT active_snapshot FROM vgraph.manifest WHERE graph = '%s';\n" "$G" | vsql -X -A -t -q; }

# Every vsql session of this test uses the same cache directory, also inside the procedures.
PRE="ALTER SESSION SET UDPARAMETER FOR vgraph cache_dir = '$CACHE_DIR';"
FAILED=0
run_sql() {
    if [ "$ECHO_ONLY" = yes ]; then echo "-- $1"; echo "$PRE"; echo "$2"; return 0; fi
    printf '%s\n%s\n' "$PRE" "$2" | vsql -X -A -t -q 2>&1
}
expect() {   # NAME PATTERN SQL
    local out
    out=$(run_sql "$1" "$3")
    if [ "$ECHO_ONLY" = yes ]; then echo "$out"; return 0; fi
    if echo "$out" | grep -q -- "$2"; then echo "PASS  $1"
    else echo "FAIL  $1"; echo "      wanted: $2"; echo "$out" | sed 's/^/      got: /' | head -6; FAILED=$((FAILED + 1)); fi
}

# gkhop through the delta view against the demo BFS on the (changed) contact table.
compare_sql() {
    local sql="SET SEARCH_PATH TO $SCHEMA, public;
\\o /dev/null
CALL bfs(1, 10, 50);
\\o
"
    for d in 1 3 6 9; do
        sql+="
DROP TABLE IF EXISTS got;
CREATE LOCAL TEMP TABLE got ON COMMIT PRESERVE ROWS AS
SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id USING PARAMETERS graph='$G', depth=$d) OVER()
FROM (SELECT * FROM $SCHEMA.${G}_delta UNION ALL SELECT 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL) q KSAFE 0;
SELECT 'depth $d: ' || g.nodes || ' nodes, ' || d.differences || ' differences' ||
       CASE WHEN d.differences = 0 AND g.nodes > 0 THEN ' ok' ELSE ' WRONG' END
FROM (SELECT COUNT(*) AS nodes FROM got) g
CROSS JOIN (SELECT COUNT(*) AS differences
      FROM ((SELECT node, hops FROM got EXCEPT SELECT person_id, lvl - 1 FROM reach WHERE lvl <= $d + 1)
            UNION ALL
            (SELECT person_id, lvl - 1 FROM reach WHERE lvl <= $d + 1 EXCEPT SELECT node, hops FROM got)) x) d;
"
    done
    echo "$sql"
}
compare() {   # NAME
    if [ "$ECHO_ONLY" = yes ]; then run_sql "$1" "$(compare_sql)"; return 0; fi
    local out
    out=$(run_sql "$1" "$(compare_sql)")
    echo "$out" | grep '^depth' | sed 's/^/      /'
    if [ "$(echo "$out" | grep -c ' ok$')" -eq 4 ]; then echo "PASS  $1"
    else echo "FAIL  $1"; echo "$out" | grep -v '^depth' | head -5 | sed 's/^/      got: /'; FAILED=$((FAILED + 1)); fi
}

if [ "$KEEP" = no ]; then
    if [ ! -f "$DEMO_SQL" ]; then
        echo "test_freshness.sh: $DEMO_SQL not found. Clone vertica-graphs-and-trees as a sibling directory." >&2
        exit 1
    fi
    if [ "$ECHO_ONLY" = yes ]; then
        echo "sed -e 's/^\\\\set ROWS .*/\\\\set ROWS $ROWS/' -e 's/^\\\\set SCHEMA .*/\\\\set SCHEMA $SCHEMA/' $DEMO_SQL | vsql -X -q"
    else
        echo "== generating demo data: $ROWS rows in schema $SCHEMA"
        sed -e "s/^\\\\set ROWS .*/\\\\set ROWS $ROWS/" -e "s/^\\\\set SCHEMA .*/\\\\set SCHEMA $SCHEMA/" "$DEMO_SQL" | vsql -X -q -v ON_ERROR_STOP=1 > /dev/null || exit 1
    fi
fi

echo "== journal table, register, first refresh"
run_sql "cleanup of an earlier run" "CALL vgraph.unregister_graph('$G');" > /dev/null
[ "$ECHO_ONLY" = yes ] || rm -rf "$CACHE_DIR/$G"
expect "journal table (BOOLEAN del, TIMESTAMPTZ version from CLOCK_TIMESTAMP, partitioned by version date)" "^journal rows: [1-9]" "
DROP TABLE IF EXISTS $SCHEMA.journal CASCADE;
CREATE TABLE $SCHEMA.journal (src INT NOT NULL, dst INT NOT NULL, del BOOLEAN NOT NULL DEFAULT FALSE,
                              ts TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP())
    ORDER BY src, dst SEGMENTED BY HASH(src) ALL NODES
    PARTITION BY (ts AT TIME ZONE 'UTC')::DATE GROUP BY CALENDAR_HIERARCHY_DAY((ts AT TIME ZONE 'UTC')::DATE, 2, 2);
INSERT INTO $SCHEMA.journal (src, dst, ts) SELECT src_id, dst_id, CLOCK_TIMESTAMP() - INTERVAL '1 day' FROM $SCHEMA.contact;
COMMIT;
SELECT 'journal rows: ' || COUNT(*) FROM $SCHEMA.journal;"

expect "register_graph" "graph $G registered" "CALL vgraph.register_graph('$G', '$SCHEMA.journal', 'src', 'dst', 'del', NULL, TRUE, 'ts', NULL);"
expect "register_graph refuses a second registration" "already registered" "CALL vgraph.register_graph('$G', '$SCHEMA.journal', 'src', 'dst', 'del', NULL, TRUE, 'ts', NULL);"
expect "register_graph refuses a bad identifier" "plain identifiers" "CALL vgraph.register_graph('vgbad', '$SCHEMA.journal', 'src; DROP TABLE x', 'dst', NULL, NULL, TRUE, NULL, NULL);"
expect "query before the first refresh says what to do" "run gload" "
SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id USING PARAMETERS graph='$G', depth=1, start=1) OVER() FROM $SCHEMA.${G}_delta;"
expect "refresh_graph" "graph $G refreshed: snapshot [0-9]" "CALL vgraph.refresh_graph('$G');"
expect "delta view holds only the sentinel after the refresh" "^rows 1, journal rows 0$" "
SELECT 'rows ' || COUNT(*) || ', journal rows ' || COUNT(src) FROM $SCHEMA.${G}_delta;"
compare "gkhop through the delta view equals the reference (no changes yet)"
expect "parameter form: start given as parameter, input is the view alone" "^found [1-9]" "
SELECT 'found ' || COUNT(*) FROM (SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id
       USING PARAMETERS graph='$G', depth=2, start=1) OVER() FROM $SCHEMA.${G}_delta) r;"

echo "== 1000 journaled adds and 1000 journaled deletes, no refresh"
expect "journal the changes (and mirror them in the reference table)" "^delta rows: 2000$" "
SET SEARCH_PATH TO $SCHEMA, public;
CREATE LOCAL TEMP TABLE adds ON COMMIT PRESERVE ROWS AS
    SELECT 1 + RANDOMINT(100000) AS s, 1 + RANDOMINT(100000) AS d FROM contact LIMIT 996 KSAFE 0;
-- two new people behind person 1, and a shortcut from person 1
INSERT INTO adds VALUES (1, 900000001); INSERT INTO adds VALUES (900000001, 900000002);
INSERT INTO adds VALUES (900000002, 77); INSERT INTO adds VALUES (1, 4242);
CREATE LOCAL TEMP TABLE dels ON COMMIT PRESERVE ROWS AS
    SELECT src_id AS s, dst_id AS d FROM (SELECT DISTINCT src_id, dst_id FROM contact WHERE src_id <= 400 AND (src_id, dst_id) NOT IN (SELECT s, d FROM adds)) c
    ORDER BY src_id, dst_id LIMIT 1000 KSAFE 0;
INSERT INTO journal (src, dst, del) SELECT s, d, FALSE FROM adds;
INSERT INTO journal (src, dst, del) SELECT s, d, TRUE FROM dels;
INSERT INTO contact SELECT s, d FROM adds;
DELETE FROM contact WHERE (src_id, dst_id) IN (SELECT s, d FROM dels);
COMMIT;
SELECT 'delta rows: ' || COUNT(src) FROM ${G}_delta;"
compare "exact after 1000 adds and 1000 deletes without refresh"

expect "delete and re-add of the same edge: the last one wins" "^1 -> 4242 present: 1, 1 -> 900000001 present: 0$" "
SET SEARCH_PATH TO $SCHEMA, public;
INSERT INTO journal (src, dst, del) VALUES (1, 4242, TRUE); COMMIT;
INSERT INTO journal (src, dst, del) VALUES (1, 4242, FALSE); COMMIT;
INSERT INTO journal (src, dst, del) VALUES (1, 900000001, TRUE); COMMIT;
DELETE FROM contact WHERE src_id = 1 AND dst_id = 900000001; COMMIT;
SELECT '1 -> 4242 present: ' || SUM((node = 4242)::INT) || ', 1 -> 900000001 present: ' || SUM((node = 900000001)::INT)
FROM (SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id USING PARAMETERS graph='$G', depth=1, start=1) OVER()
      FROM ${G}_delta) r;"
compare "exact after the re-add and the second delete"
expect "status reports the delta" "journal rows in the delta" "CALL vgraph.status('$G');"
expect "gkhop_count equals the per-level counts of gkhop, through the delta" "^levels compared: [1-9][0-9]*, different: 0$" "
SELECT 'levels compared: ' || COUNT(*) || ', different: ' || SUM((COALESCE(a.nodes, -1) <> COALESCE(b.nodes, -2))::INT)
FROM (SELECT hops, nodes FROM (SELECT vgraph.gkhop_count(start, target, src, dst, del, weight, ver, snapshot_id
             USING PARAMETERS graph='$G', depth=9, start=1) OVER() FROM $SCHEMA.${G}_delta) c) a
FULL OUTER JOIN
     (SELECT hops, COUNT(*) AS nodes FROM (SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id
             USING PARAMETERS graph='$G', depth=9, start=1) OVER() FROM $SCHEMA.${G}_delta) k GROUP BY hops) b
ON a.hops = b.hops;"
expect "status warns about a version in the future (a writer that sets the version itself)" "have a version in the future" "
INSERT INTO $SCHEMA.journal (src, dst, ts) VALUES (424242, 424243, CLOCK_TIMESTAMP() + INTERVAL '3 days'); COMMIT;
CALL vgraph.status('$G');
DELETE FROM $SCHEMA.journal WHERE src = 424242 AND dst = 424243; COMMIT;"

echo "== second refresh"
expect "refresh_graph again" "graph $G refreshed: snapshot [0-9]" "CALL vgraph.refresh_graph('$G');"
compare "exact after the refresh (rows inside the margin are applied twice, harmless)"
expect "only the active and the previous snapshot are kept after a third refresh" "^snapshots kept: 2$" "
CALL vgraph.refresh_graph('$G');
SELECT 'snapshots kept: ' || COUNT(DISTINCT snapshot_id) FROM vgraph.snapshot WHERE graph = '$G';"
SID=$(active_snapshot)

echo "== cache rules"
if [ "$ECHO_ONLY" = yes ]; then
    echo "rm -f $CACHE_DIR/$G/<active>.vg"
else
    rm -f "$CACHE_DIR/$G/$SID.vg"
fi
expect "a deleted cache file gives a clear error" "run gload" "
SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id USING PARAMETERS graph='$G', depth=1, start=1) OVER() FROM $SCHEMA.${G}_delta;"
expect "load_all repairs it" "loaded on all nodes" "CALL vgraph.load_all('$G');"
expect "ginfo after load_all: every node has the active snapshot" "^nodes with the active snapshot: all$" "
SELECT 'nodes with the active snapshot: ' || CASE WHEN i.ok = u.up THEN 'all' ELSE i.ok || ' of ' || u.up END
FROM (SELECT COUNT(DISTINCT node_name) AS ok FROM (SELECT vgraph.ginfo(USING PARAMETERS graph='$G') OVER(PARTITION NODES) FROM vgraph.probe) g
      WHERE loaded AND snapshot_id = $SID) i
CROSS JOIN (SELECT COUNT(*) AS up FROM nodes WHERE node_state = 'UP') u;"
if [ "$ECHO_ONLY" = yes ]; then
    echo "cp $CACHE_DIR/$G/<active>.vg $CACHE_DIR/$G/1.vg; echo 1 > $CACHE_DIR/$G/ACTIVE"
else
    # An older snapshot file that is still marked active: what a node looks like that missed a gload.
    cp "$CACHE_DIR/$G/$SID.vg" "$CACHE_DIR/$G/1.vg"; echo 1 > "$CACHE_DIR/$G/ACTIVE"
fi
expect "a cache that points to an older snapshot is refused as stale" "snapshot cache stale on .*: run gload" "
SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id USING PARAMETERS graph='$G', depth=1, start=1) OVER() FROM $SCHEMA.${G}_delta;"
expect "load_all repairs that too" "loaded on all nodes" "CALL vgraph.load_all('$G');"
compare "exact after the cache repairs"

echo "== schedule and unregister"
expect "schedule_refresh creates schedule and trigger" "^triggers: 1$" "
CALL vgraph.schedule_refresh('$G', '*/30 * * * *');
SELECT 'triggers: ' || COUNT(*) FROM v_catalog.stored_proc_triggers WHERE schema_name = 'vgraph' AND trigger_name ILIKE '${G}_refresh_trigger';"
expect "unregister_graph removes trigger, view, snapshots and manifest row" "^left: 0 0 0 0$" "
CALL vgraph.unregister_graph('$G');
SELECT 'left: ' || (SELECT COUNT(*) FROM v_catalog.stored_proc_triggers WHERE trigger_name ILIKE '${G}_refresh_trigger') || ' ' ||
       (SELECT COUNT(*) FROM v_catalog.views WHERE table_name ILIKE '${G}_delta') || ' ' ||
       (SELECT COUNT(*) FROM vgraph.snapshot WHERE graph = '$G') || ' ' || (SELECT COUNT(*) FROM vgraph.manifest WHERE graph = '$G');"

echo "== margin 0: the strictest case"
expect "delta view holds only the sentinel right after a refresh" "^rows 1, journal rows 0$" "
CALL vgraph.register_graph('$G', '$SCHEMA.journal', 'src', 'dst', 'del', NULL, TRUE, 'ts', 0);
CALL vgraph.refresh_graph('$G');
SELECT 'rows ' || COUNT(*) || ', journal rows ' || COUNT(src) FROM $SCHEMA.${G}_delta;"

# A writer that inserted before the refresh and commits after it. Its row is not in the snapshot,
# and its version is older than the refresh. It must still be seen: refresh_graph finds the open
# writer through its lock and starts the delta there.
if [ "$ECHO_ONLY" = yes ]; then
    echo "-- background session: INSERT INTO $SCHEMA.journal (src, dst) VALUES (1, 900000777); SELECT SLEEP(20); COMMIT;"
else
    ( printf "INSERT INTO $SCHEMA.journal (src, dst) VALUES (1, 900000777);\nSELECT SLEEP(20);\nCOMMIT;\n" | vsql -X -A -t -q > /dev/null 2>&1 ) &
    WRITER=$!
    sleep 4
fi
expect "status sees the open writer" "open transactions are writing" "CALL vgraph.status('$G');"
expect "refresh while the writer is open: its row is not visible yet" "^visible during the open transaction: 0$" "
CALL vgraph.refresh_graph('$G');
SELECT 'visible during the open transaction: ' || COUNT(*) FROM (SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id
       USING PARAMETERS graph='$G', depth=1, start=1) OVER() FROM $SCHEMA.${G}_delta) r WHERE node = 900000777;"
[ "$ECHO_ONLY" = yes ] || wait $WRITER
expect "after the late commit the row is seen, without another refresh" "^visible after the commit: 1$" "
SELECT 'visible after the commit: ' || COUNT(*) FROM (SELECT vgraph.gkhop(start, target, src, dst, del, weight, ver, snapshot_id
       USING PARAMETERS graph='$G', depth=1, start=1) OVER() FROM $SCHEMA.${G}_delta) r WHERE node = 900000777;"
run_sql "cleanup" "CALL vgraph.unregister_graph('$G');" > /dev/null

[ "$ECHO_ONLY" = yes ] && exit 0
if [ "$FAILED" -ne 0 ]; then echo "test_freshness: $FAILED FAILED"; exit 1; fi
echo "test_freshness: OK"
