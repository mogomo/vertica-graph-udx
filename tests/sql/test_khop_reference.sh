#!/usr/bin/env bash
# Integration test: gkhop must return exactly what the demo repo's BFS returns.
#
#   tests/sql/test_khop_reference.sh [--rows=N] [--keep] [--echo_only]
#
# Reference: procedure bfs() of ../vertica-graphs-and-trees/graph_contacts_demo.sql.
# It fills reach(person_id, lvl) with lvl 1 = the root, so hops = lvl - 1.
# The demo script is run as it is, only its ROWS line is overridden.
# It drops and recreates schema GRAPH_DEMO. --keep reuses an existing GRAPH_DEMO.
#
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD,
# VSQL_DATABASE from the environment.
set -euo pipefail

cd "$(dirname "$0")/../.."

ROWS=2000000
KEEP=no
ECHO_ONLY=no
DEMO_SQL=../vertica-graphs-and-trees/graph_contacts_demo.sql
DEPTHS="1 3 6 9"
ROOT=1

for arg in "$@"; do
    case "$arg" in
        --rows=*)    ROWS="${arg#--rows=}" ;;
        --keep)      KEEP=yes ;;
        --echo_only) ECHO_ONLY=yes ;;
        -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "test_khop_reference.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [ ! -f "$DEMO_SQL" ]; then
    echo "test_khop_reference.sh: $DEMO_SQL not found. Clone vertica-graphs-and-trees as a sibling directory." >&2
    exit 1
fi

# One session: reach is a LOCAL TEMP table of the session that called bfs().
MAX_LEVEL=$(( $(echo $DEPTHS | tr ' ' '\n' | sort -n | tail -1) + 1 ))
TEST_SQL="SET SEARCH_PATH TO GRAPH_DEMO, public;
\\o /dev/null
CALL bfs($ROOT, $MAX_LEVEL, 50);
\\o
"
for d in $DEPTHS; do
    TEST_SQL+="
DROP TABLE IF EXISTS got;
CREATE LOCAL TEMP TABLE got ON COMMIT PRESERVE ROWS AS
SELECT gkhop(start, target, src, dst, op, epoch, snapshot_epoch USING PARAMETERS depth=$d) OVER()
FROM (SELECT NULL::INT AS start, NULL::INT AS target, src_id AS src, dst_id AS dst,
             NULL::INT AS op, NULL::INT AS epoch, NULL::INT AS snapshot_epoch FROM contact
      UNION ALL SELECT $ROOT, NULL, NULL, NULL, NULL, NULL, NULL) q;
SELECT 'depth $d: ' || g.nodes || ' nodes, ' || d.differences || ' differences' ||
       CASE WHEN d.differences = 0 AND g.nodes > 0 THEN '  PASS' ELSE '  FAIL' END
FROM (SELECT COUNT(*) AS nodes FROM got) g
CROSS JOIN
     (SELECT COUNT(*) AS differences
      FROM ((SELECT node, hops FROM got EXCEPT SELECT person_id, lvl - 1 FROM reach WHERE lvl <= $d + 1)
            UNION ALL
            (SELECT person_id, lvl - 1 FROM reach WHERE lvl <= $d + 1 EXCEPT SELECT node, hops FROM got)) x) d;
"
done

if [ "$ECHO_ONLY" = yes ]; then
    [ "$KEEP" = yes ] || echo "sed 's/^\\\\set ROWS .*/\\\\set ROWS $ROWS/' $DEMO_SQL | vsql -X -q"
    echo "vsql -X -A -t -q <<'SQL'"; echo "$TEST_SQL"; echo "SQL"
    exit 0
fi

if [ "$KEEP" = no ]; then
    echo "== generating demo data: $ROWS rows in schema GRAPH_DEMO"
    sed "s/^\\\\set ROWS .*/\\\\set ROWS $ROWS/" "$DEMO_SQL" | vsql -X -q -v ON_ERROR_STOP=1 > /dev/null
fi

echo "== gkhop against the demo BFS, root $ROOT, depths $DEPTHS"
RESULT=$(echo "$TEST_SQL" | vsql -X -A -t -q -v ON_ERROR_STOP=1)
echo "$RESULT"

PASSED=$(echo "$RESULT" | grep -c 'PASS$' || true)
WANTED=$(echo $DEPTHS | wc -w)
if [ "$PASSED" -ne "$WANTED" ]; then
    echo "test_khop_reference: FAILED"
    exit 1
fi
echo "test_khop_reference: OK"
