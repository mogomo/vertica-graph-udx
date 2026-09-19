#!/usr/bin/env bash
# Rebuild the snapshot of a graph and load it on every node, or only repair the node caches.
#
#   scripts/refresh.sh --graph=NAME [--load_only] [--schedule='CRON'] [--status] [--echo_only]
#
#   --load_only   vgraph.load_all: load the active snapshot again on every node
#   --schedule    vgraph.schedule_refresh with a cron expression, for example '0 * * * *'
#   --status      vgraph.status: pending changes, open writers, warnings
#
# Connection: vsql reads VSQL_HOST, VSQL_PORT, VSQL_USER, VSQL_PASSWORD, VSQL_DATABASE from the environment.
set -euo pipefail

GRAPH= MODE=refresh CRON= ECHO_ONLY=no
for arg in "$@"; do
    case "$arg" in
        --graph=*)    GRAPH="${arg#*=}" ;;
        --load_only)  MODE=load ;;
        --schedule=*) MODE=schedule; CRON="${arg#*=}" ;;
        --status)     MODE=status ;;
        --echo_only)  ECHO_ONLY=yes ;;
        -h|--help)    sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "refresh.sh: unknown argument: $arg" >&2; exit 2 ;;
    esac
done
[ -n "$GRAPH" ] || { echo "refresh.sh: --graph is required" >&2; exit 2; }

case "$MODE" in
    refresh)  SQL="CALL vgraph.refresh_graph('$GRAPH');" ;;
    load)     SQL="CALL vgraph.load_all('$GRAPH');" ;;
    schedule) SQL="CALL vgraph.schedule_refresh('$GRAPH', '$CRON');" ;;
    status)   SQL="CALL vgraph.status('$GRAPH');" ;;
esac
if [ "$ECHO_ONLY" = yes ]; then echo "vsql -X -c \"$SQL\""; exit 0; fi
vsql -X -v ON_ERROR_STOP=1 -c "$SQL"
