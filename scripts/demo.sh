#!/usr/bin/env bash
# A small end-to-end demo: a contacts journal, register, refresh, the four
# queries, a change that is seen without a refresh, cleanup.
#
#   scripts/demo.sh [--people=N] [--keep] [--echo_only]
#
# Creates and drops schema VGRAPH_DEMO and graph vgdemo. Needs nothing else.
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE from the environment.
set -euo pipefail

cd "$(dirname "$0")/.."
PEOPLE=200000 KEEP=no ECHO_ONLY=no
for arg in "$@"; do
    case "$arg" in
        --people=*)  PEOPLE="${arg#*=}" ;;
        --keep)      KEEP=yes ;;
        --echo_only) ECHO_ONLY=yes ;;
        -h|--help)   sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "demo.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done
case "$PEOPLE" in ''|*[!0-9]*) echo "demo.sh: --people must be a number" >&2; exit 2 ;; esac

COLS="start, target, src, dst, del, weight, ver, snapshot_id"
SQL=$(cat <<SQLEOF
\set ON_ERROR_STOP on
\echo == 1. a contacts journal: rows are only inserted; a delete is a row with del = true
DROP SCHEMA IF EXISTS VGRAPH_DEMO CASCADE;
CREATE SCHEMA VGRAPH_DEMO;
CREATE TABLE VGRAPH_DEMO.contacts (
    src INT NOT NULL, dst INT NOT NULL,
    del BOOLEAN NOT NULL DEFAULT FALSE,
    strength FLOAT,
    ts TIMESTAMPTZ NOT NULL DEFAULT CLOCK_TIMESTAMP())
ORDER BY src, dst SEGMENTED BY HASH(src) ALL NODES
PARTITION BY (ts AT TIME ZONE 'UTC')::DATE GROUP BY CALENDAR_HIERARCHY_DAY((ts AT TIME ZONE 'UTC')::DATE, 2, 2);
-- every person knows 3 random people; the contact is stored in both directions
CREATE LOCAL TEMP TABLE picks ON COMMIT PRESERVE ROWS AS
    SELECT 1 + RANDOMINT($PEOPLE) AS a, 1 + RANDOMINT($PEOPLE) AS b, 1 + RANDOMINT(9) AS s
    FROM vgraph.probe p1 CROSS JOIN vgraph.probe p2 LIMIT $((PEOPLE * 3)) KSAFE 0;
INSERT INTO VGRAPH_DEMO.contacts (src, dst, strength, ts) SELECT a, b, s, CLOCK_TIMESTAMP() - INTERVAL '1 day' FROM picks WHERE a <> b;
INSERT INTO VGRAPH_DEMO.contacts (src, dst, strength, ts) SELECT b, a, s, CLOCK_TIMESTAMP() - INTERVAL '1 day' FROM picks WHERE a <> b;
COMMIT;
SELECT COUNT(*) AS contact_rows FROM VGRAPH_DEMO.contacts;

\echo == 2. register and build
CALL vgraph.register_graph('vgdemo', 'VGRAPH_DEMO.contacts', 'src', 'dst', 'del', 'strength', TRUE, 'ts', NULL);
CALL vgraph.refresh_graph('vgdemo');
SELECT graph, active_snapshot, node_count, edge_count, build_seconds FROM vgraph.manifest WHERE graph = 'vgdemo';
SELECT node_name, snapshot_id, node_count, edge_count, loaded FROM (SELECT vgraph.ginfo(USING PARAMETERS graph='vgdemo') OVER(PARTITION NODES) FROM vgraph.probe) i ORDER BY 1;

\echo == 3. who is within 4 hops of person 1? counts per hop (hops 0 = person 1)
SELECT hops, nodes FROM (SELECT vgraph.gkhop_count($COLS USING PARAMETERS graph='vgdemo', start=1, depth=4) OVER() FROM VGRAPH_DEMO.vgdemo_delta) r ORDER BY hops;
\echo == the people exactly 2 hops away, first 10
SELECT node FROM (SELECT vgraph.gkhop($COLS USING PARAMETERS graph='vgdemo', start=1, depth=2, exact=true) OVER() FROM VGRAPH_DEMO.vgdemo_delta) r ORDER BY node LIMIT 10;

\echo == 4. shortest path from person 1 to person 4242, by hops and by strength
SELECT hop_no, node FROM (SELECT vgraph.gpath($COLS USING PARAMETERS graph='vgdemo', start=1, target=4242) OVER() FROM VGRAPH_DEMO.vgdemo_delta) r ORDER BY hop_no;
SELECT hop_no, node FROM (SELECT vgraph.gpath($COLS USING PARAMETERS graph='vgdemo', start=1, target=4242, weighted=true) OVER() FROM VGRAPH_DEMO.vgdemo_delta) r ORDER BY hop_no;

\echo == 5. connected components: the 5 largest
SELECT component, COUNT(*) AS people FROM (SELECT vgraph.gcomponents($COLS USING PARAMETERS graph='vgdemo') OVER() FROM VGRAPH_DEMO.vgdemo_delta) r GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 5;

\echo == 6. PageRank: the 5 best connected people
SELECT node, rank FROM (SELECT vgraph.gpagerank($COLS USING PARAMETERS graph='vgdemo') OVER() FROM VGRAPH_DEMO.vgdemo_delta) r ORDER BY rank DESC, node LIMIT 5;

\echo == 7. freshness: person 1 meets a new person 999999999, and loses nobody. No refresh.
INSERT INTO VGRAPH_DEMO.contacts (src, dst, strength) VALUES (1, 999999999, 1);
COMMIT;
SELECT node, hops FROM (SELECT vgraph.gkhop($COLS USING PARAMETERS graph='vgdemo', start=1, depth=1) OVER() FROM VGRAPH_DEMO.vgdemo_delta) r WHERE node = 999999999;
\echo == the contact is deleted again: one more journal row
INSERT INTO VGRAPH_DEMO.contacts (src, dst, del) VALUES (1, 999999999, TRUE);
COMMIT;
SELECT COUNT(*) AS still_a_contact FROM (SELECT vgraph.gkhop($COLS USING PARAMETERS graph='vgdemo', start=1, depth=1) OVER() FROM VGRAPH_DEMO.vgdemo_delta) r WHERE node = 999999999;
CALL vgraph.status('vgdemo');
SQLEOF
)
CLEAN="CALL vgraph.unregister_graph('vgdemo'); DROP SCHEMA IF EXISTS VGRAPH_DEMO CASCADE;"

if [ "$ECHO_ONLY" = yes ]; then
    echo "vsql -X <<'SQL'"; echo "$SQL"; [ "$KEEP" = yes ] || echo "$CLEAN"; echo "SQL"; exit 0
fi
vsql -X -q -c "CALL vgraph.unregister_graph('vgdemo');" > /dev/null 2>&1 || true
echo "$SQL" | vsql -X -q
if [ "$KEEP" = no ]; then
    echo "== cleanup (use --keep to keep schema VGRAPH_DEMO and graph vgdemo)"
    vsql -X -q -c "$CLEAN"
fi
echo "demo.sh: done"
