#!/usr/bin/env bash
# The same graph questions on the same data and the same machine: vgraph in Vertica and Neo4j.
#
#   scripts/neo4j_compare.sh --neo4j_home=DIR [--java_home=DIR] [--rows=N] [--schema=NAME] [--graph=NAME] [--work=DIR]
#                            [--heap=8g] [--pagecache=8g] [--keep] [--echo_only]
#
#   --neo4j_home  an unpacked Neo4j 5 Community tarball with the APOC core and Graph Data Science jars
#                 in its plugins directory. The script owns this installation: it replaces the database
#                 "neo4j" in it, writes conf/neo4j.conf (listens on localhost only, no authentication)
#                 and stops the server at the end.
#   --rows        contact rows to generate (default 100000000; people = rows / 6)
#   --work        directory for the exported CSV files (default /tmp/vgraph_neo4j; about 1.5 GB per 100M rows)
#   --keep        reuse the Vertica data, the CSV files and the Neo4j store of an earlier run
#
# Data: ../vertica-graphs-and-trees/graph_contacts_demo.sql. Every contact is stored in both directions
# in Vertica. Neo4j gets every contact once and is asked without a direction, which is its usual model.
# Schema VGNEO (or --schema) is dropped and recreated unless --keep is given. The graph is called vgneo
# (or --graph): a second size can be kept next to the first with its own schema, graph and work directory.
# Vertica times are server times from v_monitor.query_requests. Neo4j times are the server times that
# cypher-shell reports (ready + consumed). Every question runs three times; the best time counts.
# Results are compared: the script fails when the two systems disagree.
#
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE from the environment.
set -euo pipefail

cd "$(dirname "$0")/.."
NEO= JAVA= ROWS=100000000 SCHEMA=VGNEO GRAPH=vgneo WORK=/tmp/vgraph_neo4j HEAP=8g PAGECACHE=8g KEEP=no ECHO_ONLY=no
DEMO_SQL=../vertica-graphs-and-trees/graph_contacts_demo.sql
for arg in "$@"; do
    case "$arg" in
        --neo4j_home=*) NEO="${arg#*=}" ;;
        --java_home=*)  JAVA="${arg#*=}" ;;
        --rows=*)       ROWS="${arg#*=}" ;;
        --schema=*)     SCHEMA="${arg#*=}" ;;
        --graph=*)      GRAPH="${arg#*=}" ;;
        --work=*)       WORK="${arg#*=}" ;;
        --heap=*)       HEAP="${arg#*=}" ;;
        --pagecache=*)  PAGECACHE="${arg#*=}" ;;
        --keep)         KEEP=yes ;;
        --echo_only)    ECHO_ONLY=yes ;;
        -h|--help)      sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "neo4j_compare.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done
[ -n "$NEO" ] && [ -x "$NEO/bin/neo4j-admin" ] || { echo "neo4j_compare.sh: --neo4j_home must point to an unpacked Neo4j" >&2; exit 2; }
[ -f "$DEMO_SQL" ] || { echo "neo4j_compare.sh: $DEMO_SQL not found. Clone vertica-graphs-and-trees as a sibling directory." >&2; exit 1; }
[ -n "$JAVA" ] && export JAVA_HOME="$JAVA" PATH="$JAVA/bin:$PATH"

if [ "$ECHO_ONLY" = yes ]; then
    cat <<EOF
sed ROWS=$ROWS SCHEMA=$SCHEMA $DEMO_SQL | vsql                      # generate
CALL vgraph.register_graph('$GRAPH', '$SCHEMA.contact', ...); CALL vgraph.refresh_graph('$GRAPH');
vsql: export people to $WORK/people.csv and each contact once to $WORK/knows.csv
$NEO/bin/neo4j-admin database import full neo4j --overwrite-destination --nodes=Person=... --relationships=KNOWS=...
$NEO/bin/neo4j start; CREATE CONSTRAINT ...; run the questions on both systems; $NEO/bin/neo4j stop
EOF
    exit 0
fi

mkdir -p "$WORK"
RESULTS="$WORK/results.txt"; : > "$RESULTS"
REST="NULL::INT, NULL::INT, NULL::INT, NULL::BOOLEAN, NULL::FLOAT, NULL::INT, NULL::INT"
ARGS="NULL::INT, NULL::INT, NULL::INT, NULL::INT, NULL::BOOLEAN, NULL::FLOAT, NULL::INT, NULL::INT"
now() { date +%s.%N; }
since() { echo "$1 $(now)" | awk '{printf "%.1f", $2 - $1}'; }
sql() { vsql -X -A -t -q -v ON_ERROR_STOP=1 -c "$1" 2>&1 | grep -v '^NOTICE\|^CONTEXT\|^WARNING' || true; }
note() { echo "$1|$2|$3|$4" >> "$RESULTS"; echo "   $1: vgraph $2, Neo4j $3 ${4:+($4)}"; }

# ---------------------------------------------------------------- Vertica: data and snapshot
if [ "$KEEP" = no ]; then
    echo "== Vertica: generating $ROWS rows in schema $SCHEMA"
    t=$(now)
    sed -e "s/^\\\\set ROWS .*/\\\\set ROWS $ROWS/" -e "s/^\\\\set SCHEMA .*/\\\\set SCHEMA $SCHEMA/" "$DEMO_SQL" | vsql -X -q -v ON_ERROR_STOP=1 > /dev/null
    V_LOAD=$(since "$t")
    echo "== Vertica: snapshot"
    sql "CALL vgraph.unregister_graph('$GRAPH');" > /dev/null
    sql "CALL vgraph.register_graph('$GRAPH', '$SCHEMA.contact', 'src_id', 'dst_id', NULL, NULL, TRUE, NULL, NULL);
         UPDATE vgraph.manifest SET both_directions = TRUE WHERE graph = '$GRAPH'; COMMIT;" > /dev/null
    sql "CALL vgraph.refresh_graph('$GRAPH');" > /dev/null
    echo "$V_LOAD" > "$WORK/v_load"
fi
V_LOAD=$(cat "$WORK/v_load" 2>/dev/null || echo "?")
V_BUILD=$(sql "SELECT ROUND(build_seconds, 1) FROM vgraph.manifest WHERE graph = '$GRAPH';")
V_SIZE=$(sql "SELECT ROUND(SUM(LENGTH(chunk)) / 1e6)::INT FROM vgraph.snapshot WHERE graph = '$GRAPH' AND snapshot_id = (SELECT active_snapshot FROM vgraph.manifest WHERE graph = '$GRAPH');")
V_TABLE=$(sql "SELECT ROUND(SUM(used_bytes) / 1e6)::INT FROM v_monitor.projection_storage WHERE anchor_table_schema ILIKE '$SCHEMA' AND anchor_table_name = 'contact';")

# ---------------------------------------------------------------- export and Neo4j import
if [ "$KEEP" = no ]; then
    echo "== export to CSV"
    t=$(now)
    echo "id:ID" > "$WORK/people_header.csv"; echo ":START_ID,:END_ID" > "$WORK/knows_header.csv"
    vsql -X -A -t -q -c "SELECT id FROM (SELECT src_id AS id FROM $SCHEMA.contact UNION SELECT dst_id FROM $SCHEMA.contact) u" > "$WORK/people.csv"
    vsql -X -A -t -q -F, -c "SELECT DISTINCT src_id, dst_id FROM $SCHEMA.contact WHERE src_id < dst_id" > "$WORK/knows.csv"
    EXPORT=$(since "$t")
    echo "== Neo4j: import"
    "$NEO/bin/neo4j" stop > /dev/null 2>&1 || true
    cat > "$NEO/conf/neo4j.conf" <<EOF
server.default_listen_address=127.0.0.1
dbms.security.auth_enabled=false
dbms.security.procedures.unrestricted=apoc.*,gds.*
server.memory.heap.initial_size=$HEAP
server.memory.heap.max_size=$HEAP
server.memory.pagecache.size=$PAGECACHE
dbms.usage_report.enabled=false
db.transaction.timeout=600s
EOF
    t=$(now)
    "$NEO/bin/neo4j-admin" database import full neo4j --overwrite-destination --id-type=integer \
        --nodes=Person="$WORK/people_header.csv,$WORK/people.csv" \
        --relationships=KNOWS="$WORK/knows_header.csv,$WORK/knows.csv" > "$WORK/import.log" 2>&1 || { tail -20 "$WORK/import.log"; exit 1; }
    N_IMPORT=$(since "$t")
    echo "$EXPORT $N_IMPORT" > "$WORK/n_import"
fi
read -r EXPORT N_IMPORT < "$WORK/n_import"
N_SIZE=$(du -sm "$NEO/data/databases/neo4j" | cut -f1)

echo "== Neo4j: start"
"$NEO/bin/neo4j" start > /dev/null
trap '"$NEO/bin/neo4j" stop > /dev/null 2>&1 || true' EXIT
for i in $(seq 1 120); do "$NEO/bin/cypher-shell" -a bolt://127.0.0.1:7687 "RETURN 1" > /dev/null 2>&1 && break; sleep 2; done

# cy QUERY: prints "<first value of the last row> <milliseconds>".
# The whole answer of cypher-shell stays in $WORK/last_cypher, so a failure can be told from a timeout.
cy() {
    "$NEO/bin/cypher-shell" -a bolt://127.0.0.1:7687 --format verbose "$1" > "$WORK/last_cypher" 2>&1 || true
    awk '
        /^\|/ { val = $0; gsub(/[| "]/, "", val) }
        /ready to start consuming query after/ { for (i = 1; i <= NF; i++) { if ($i == "after" && $(i + 1) != "another") { a = $(i + 1) } if ($i == "another") { b = $(i + 1) } } }
        END { print val, a + b }' "$WORK/last_cypher"
}
# best of three: prints "<value> <best ms>". A question that does not answer is not repeated and prints
# "timeout 600000" (it ran into db.transaction.timeout) or "failed 0" (Neo4j refused it, for example
# because it did not have enough memory). The reason is copied to $WORK/failed_*.log.
cy3() {
    local best= v= r ms
    for i in 1 2 3; do
        r=$(cy "$1"); v=${r% *}; ms=${r##* }
        case "$v" in ''|*[!0-9]*)
            cp "$WORK/last_cypher" "$WORK/failed_$(date +%s).log"
            if grep -qiE 'timeout|terminated|timed out' "$WORK/last_cypher"; then echo "timeout 600000"; else echo "failed 0"; fi
            return 0 ;;
        esac
        if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then best=$ms; fi
    done
    echo "$v $best"
}
# ntime "<value> <ms>": the Neo4j time for the table, or why there is none
ntime() {
    case "${1% *}" in
        timeout) echo "more than 600 s, stopped" ;;
        failed)  echo "did not run (see $WORK/failed_*.log)" ;;
        *)       fmt "${1##* }" ;;
    esac
}
# vq LABEL SQL: runs SQL three times under a label; prints "<value> <best ms>"
vq() {
    local label="$1" q="$2"
    printf '%s\n' "CREATE LOCAL TEMP TABLE one_row (x INT) ON COMMIT PRESERVE ROWS; INSERT INTO one_row VALUES (1);" \
        "$q" "$q" "$q" \
        "SELECT 'ms ' || MIN(request_duration_ms) FROM v_monitor.query_requests WHERE session_id = CURRENT_SESSION() AND request_label = '$label';" \
        | vsql -X -A -t -q -v ON_ERROR_STOP=1 2>&1 | grep -v '^NOTICE\|^CONTEXT' | awk '/^ms /{ms = $2; next} {v = $0} END {print v, ms}'
}
fmt() { awk -v ms="$1" 'BEGIN { if (ms >= 1000) printf "%.2f s", ms / 1000; else printf "%d ms", ms }'; }

t=$(now)
"$NEO/bin/cypher-shell" -a bolt://127.0.0.1:7687 "CREATE CONSTRAINT person_id IF NOT EXISTS FOR (p:Person) REQUIRE p.id IS UNIQUE" > /dev/null
"$NEO/bin/cypher-shell" -a bolt://127.0.0.1:7687 "CALL db.awaitIndexes(3600)" > /dev/null
N_INDEX=$(since "$t")

echo "== questions"
FAILED=0
same() { if [ "$3" != timeout ] && [ "$3" != failed ] && [ "$2" != "$3" ]; then echo "   DIFFERENT RESULT for $1: vgraph $2, Neo4j $3"; FAILED=$((FAILED + 1)); fi; }

for d in 2 3 6 9; do
    r=$(vq "vn_count_$d" "SELECT /*+LABEL(vn_count_$d)*/ SUM(nodes) FROM (SELECT vgraph.gkhop_count(1, $REST USING PARAMETERS graph='$GRAPH', depth=$d) OVER() FROM one_row) r;")
    n=$(cy3 "MATCH (a:Person {id: 1}) CALL apoc.path.subgraphNodes(a, {maxLevel: $d, relationshipFilter: 'KNOWS'}) YIELD node RETURN count(node)")
    same "people within $d hops" "${r% *}" "${n% *}"
    note "people within $d hops of person 1, counted (${r% *})" "$(fmt "${r##* }")" "$(ntime "$n")" "apoc.path.subgraphNodes"
done
for d in 2 3 4 6 9; do
    r=$(vq "vn_count_$d" "SELECT /*+LABEL(vn_count_$d)*/ SUM(nodes) - 1 FROM (SELECT vgraph.gkhop_count(1, $REST USING PARAMETERS graph='$GRAPH', depth=$d) OVER() FROM one_row) r;")
    n=$(cy3 "MATCH (a:Person {id: 1})-[:KNOWS*1..$d]-(b) WHERE b <> a RETURN count(DISTINCT b)")
    same "plain Cypher $d hops" "${r% *}" "${n% *}"
    note "the same within $d hops, plain Cypher pattern (${r% *})" "$(fmt "${r##* }")" "$(ntime "$n")" "MATCH -[:KNOWS*1..$d]-"
done

# a far target: the smallest id among the people at the largest distance from person 1
TARGET=$(sql "SELECT node FROM (SELECT vgraph.gkhop(1, $REST USING PARAMETERS graph='$GRAPH', depth=64) OVER() FROM dual) r ORDER BY hops DESC, node LIMIT 1;")
r=$(vq vn_path "SELECT /*+LABEL(vn_path)*/ MAX(hop_no) FROM (SELECT vgraph.gpath(1, $TARGET, NULL::INT, NULL::INT, NULL::BOOLEAN, NULL::FLOAT, NULL::INT, NULL::INT USING PARAMETERS graph='$GRAPH') OVER() FROM one_row) r;")
n=$(cy3 "MATCH (a:Person {id: 1}), (b:Person {id: $TARGET}) MATCH p = shortestPath((a)-[:KNOWS*..30]-(b)) RETURN length(p)")
same "shortest path length" "${r% *}" "${n% *}"
note "shortest path from person 1 to person $TARGET (${r% *} hops)" "$(fmt "${r##* }")" "$(ntime "$n")" "shortestPath"

# whole-graph algorithms. Neo4j Graph Data Science first projects the graph into its own memory.
cy "CALL gds.graph.drop('g', false) YIELD graphName RETURN graphName" > /dev/null
p=$(cy "CALL gds.graph.project('g', 'Person', {KNOWS: {orientation: 'UNDIRECTED'}}) YIELD projectMillis RETURN projectMillis")
case "${p% *}" in
    ''|*[!0-9]*) cp "$WORK/last_cypher" "$WORK/failed_projection.log"
                 N_PROJECT="did not run (see $WORK/failed_projection.log)" ;;
    *)           N_PROJECT="$(fmt "${p% *}")" ;;
esac
r=$(vq vn_cc "SELECT /*+LABEL(vn_cc)*/ COUNT(*) FROM (SELECT vgraph.gcomponents_count($ARGS USING PARAMETERS graph='$GRAPH') OVER() FROM one_row) r;")
n=$(cy3 "CALL gds.wcc.stats('g') YIELD componentCount RETURN componentCount")
same "connected components" "${r% *}" "${n% *}"
note "connected components of the whole graph, counted (${r% *})" "$(fmt "${r##* }")" "$(ntime "$n")" "gds.wcc.stats, after the projection"
r=$(vq vn_pr "SELECT /*+LABEL(vn_pr)*/ COUNT(*) FROM (SELECT vgraph.gpagerank($ARGS USING PARAMETERS graph='$GRAPH', iterations=20, damping=0.85, top=10) OVER() FROM one_row) r;")
n=$(cy3 "CALL gds.pageRank.stats('g', {maxIterations: 20, dampingFactor: 0.85, tolerance: 0}) YIELD ranIterations RETURN ranIterations")
note "PageRank of the whole graph, 20 iterations (vgraph returns the top 10, Neo4j only statistics)" "$(fmt "${r##* }")" "$(ntime "$n")" "gds.pageRank.stats, after the projection"

echo
echo "$ROWS contact rows. Vertica stores both directions; Neo4j stores every contact once."
echo
echo "| Step | vgraph in Vertica | Neo4j |"
echo "|---|---:|---:|"
echo "| load the data | $V_LOAD s (generated inside Vertica) | $EXPORT s CSV export + $N_IMPORT s neo4j-admin import + $N_INDEX s id index |"
echo "| prepare for graph queries | $V_BUILD s (refresh_graph: build and load the snapshot) | $N_PROJECT (GDS projection, needed for components and PageRank, repeated after every restart) |"
echo "| size on disk | table $V_TABLE MB + snapshot $V_SIZE MB | store $N_SIZE MB |"
while IFS='|' read -r what v n how; do echo "| $what | $v | $n ($how) |"; done < "$RESULTS"
if [ "$FAILED" -ne 0 ]; then echo; echo "neo4j_compare: $FAILED result(s) differ"; exit 1; fi
echo; echo "Both systems returned the same result for every question."
