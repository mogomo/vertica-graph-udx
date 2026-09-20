-- vgraph stored procedures (PL/vSQL). Installed by sql/install.sql.
--
--   vgraph.register_graph(graph, edge_table, src_col, dst_col, op_col, weight_col, directed, ver_col, margin)
--   vgraph.refresh_graph(graph)
--   vgraph.load_all(graph)
--   vgraph.status(graph)
--   vgraph.schedule_refresh(graph, cron_expr)
--   vgraph.unregister_graph(graph)
--
-- Identifiers given by the caller are checked against a strict pattern before
-- they are put into dynamic SQL.

-- Builds <edge schema>.<graph>_delta from the manifest row.
-- The view returns the journal rows that may be newer than the active snapshot,
-- plus one sentinel row, so its result is never empty. The boundary is a
-- literal, so Vertica can prune partitions and storage containers.
CREATE OR REPLACE PROCEDURE vgraph.make_delta_view(g VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    tab VARCHAR(256); src VARCHAR(128); dst VARCHAR(128); op VARCHAR(128); w VARCHAR(128); ver VARCHAR(128);
    op_type VARCHAR(128); ver_type VARCHAR(128);
    sid INT; v_from VARCHAR(64);
    del_expr VARCHAR(400); w_expr VARCHAR(400); ver_expr VARCHAR(400);
    view_name VARCHAR(400); stmt VARCHAR(8000); sid_text VARCHAR(32);
BEGIN
    tab := (SELECT MAX(edge_table) FROM vgraph.manifest WHERE graph = g);
    IF tab IS NULL THEN
        RAISE EXCEPTION 'vgraph.make_delta_view: graph % is not registered', g;
    END IF;
    src := (SELECT src_col FROM vgraph.manifest WHERE graph = g);
    dst := (SELECT dst_col FROM vgraph.manifest WHERE graph = g);
    op := (SELECT op_col FROM vgraph.manifest WHERE graph = g);
    w := (SELECT weight_col FROM vgraph.manifest WHERE graph = g);
    ver := (SELECT ver_col FROM vgraph.manifest WHERE graph = g);
    sid := (SELECT active_snapshot FROM vgraph.manifest WHERE graph = g);
    v_from := (SELECT m.delta_from FROM vgraph.manifest m WHERE m.graph = g);
    view_name := SPLIT_PART(tab, '.', 1) || '.' || g || '_delta';
    sid_text := COALESCE(sid::VARCHAR, 'NULL::INT');

    stmt := 'CREATE OR REPLACE VIEW ' || view_name || ' AS ';
    IF ver IS NOT NULL AND v_from IS NOT NULL THEN
        op_type := (SELECT data_type FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(tab, '.', 1)
                    AND table_name ILIKE SPLIT_PART(tab, '.', 2) AND column_name ILIKE op);
        ver_type := (SELECT data_type FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(tab, '.', 1)
                     AND table_name ILIKE SPLIT_PART(tab, '.', 2) AND column_name ILIKE ver);
        del_expr := CASE WHEN op IS NULL THEN 'FALSE'
                         WHEN op_type ILIKE 'bool%' THEN 'COALESCE(' || op || ', FALSE)'
                         ELSE '(COALESCE(' || op || ', 1) < 0)' END;
        w_expr := CASE WHEN w IS NULL THEN 'NULL::FLOAT' ELSE w || '::FLOAT' END;
        ver_expr := CASE WHEN ver_type ILIKE 'int%' THEN ver || '::INT'
                         ELSE '(EXTRACT(EPOCH FROM ' || ver || ') * 1000000)::INT' END;
        stmt := stmt || 'SELECT NULL::INT AS start, NULL::INT AS target, ' || src || '::INT AS src, ' || dst
             || '::INT AS dst, ' || del_expr || ' AS del, ' || w_expr || ' AS weight, ' || ver_expr || ' AS ver, '
             || sid_text || ' AS snapshot_id FROM ' || tab || ' WHERE ' || ver || ' > ' || v_from || ' UNION ALL ';
    END IF;
    stmt := stmt || 'SELECT NULL::INT AS start, NULL::INT AS target, NULL::INT AS src, NULL::INT AS dst, '
         || 'NULL::BOOLEAN AS del, NULL::FLOAT AS weight, NULL::INT AS ver, ' || sid_text || ' AS snapshot_id';
    EXECUTE stmt;
END;
$$;

-- op_col: BOOLEAN (true = edge deleted) or INT (+1 added, -1 deleted), or NULL when edges are only added.
-- ver_col: TIMESTAMP, TIMESTAMPTZ or INT column that orders the journal (last row wins). NULL = static
--          graph: queries see the snapshot only, changes show up at the next refresh.
-- margin:  overlap of the delta. Timestamp ver_col: seconds (NULL = 60). Open transactions are found
--          through their locks, so it only covers clock differences and statement-start versions.
--          INT ver_col: units of that column; there it must also cover the longest write transaction.
CREATE OR REPLACE PROCEDURE vgraph.register_graph(g VARCHAR, edge_table VARCHAR, src_col VARCHAR, dst_col VARCHAR,
                                                  op_col VARCHAR, weight_col VARCHAR, directed BOOLEAN,
                                                  ver_col VARCHAR, margin INT) LANGUAGE PLvSQL AS $$
DECLARE
    ident VARCHAR(64) := '^[A-Za-z_][A-Za-z0-9_]*$';
    n INT; ver_type VARCHAR(128); op_type VARCHAR(128); m INT;
BEGIN
    IF g IS NULL OR NOT REGEXP_LIKE(g, '^[A-Za-z0-9_]{1,64}$') THEN
        RAISE EXCEPTION 'vgraph.register_graph: graph name must be 1 to 64 letters, digits or underscores';
    END IF;
    IF edge_table IS NULL OR NOT REGEXP_LIKE(edge_table, '^[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*$') THEN
        RAISE EXCEPTION 'vgraph.register_graph: edge_table must be given as schema.table';
    END IF;
    IF src_col IS NULL OR dst_col IS NULL OR NOT REGEXP_LIKE(src_col, ident) OR NOT REGEXP_LIKE(dst_col, ident)
       OR NOT REGEXP_LIKE(COALESCE(op_col, 'x'), ident) OR NOT REGEXP_LIKE(COALESCE(weight_col, 'x'), ident)
       OR NOT REGEXP_LIKE(COALESCE(ver_col, 'x'), ident) THEN
        RAISE EXCEPTION 'vgraph.register_graph: column names must be plain identifiers';
    END IF;
    n := (SELECT COUNT(*) FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(edge_table, '.', 1)
          AND table_name ILIKE SPLIT_PART(edge_table, '.', 2)
          AND (column_name ILIKE src_col OR column_name ILIKE dst_col OR column_name ILIKE op_col
               OR column_name ILIKE weight_col OR column_name ILIKE ver_col));
    IF n <> 2 + (op_col IS NOT NULL)::INT + (weight_col IS NOT NULL)::INT + (ver_col IS NOT NULL)::INT THEN
        RAISE EXCEPTION 'vgraph.register_graph: table % or one of the given columns does not exist', edge_table;
    END IF;
    IF (SELECT COUNT(*) FROM vgraph.manifest WHERE graph = g) > 0 THEN
        RAISE EXCEPTION 'vgraph.register_graph: graph % is already registered', g;
    END IF;
    IF op_col IS NOT NULL AND ver_col IS NULL THEN
        RAISE EXCEPTION 'vgraph.register_graph: op_col needs ver_col: deletes must be ordered against adds';
    END IF;
    m := NULL;
    IF ver_col IS NOT NULL THEN
        ver_type := (SELECT data_type FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(edge_table, '.', 1)
                     AND table_name ILIKE SPLIT_PART(edge_table, '.', 2) AND column_name ILIKE ver_col);
        IF ver_type ILIKE 'timestamp%' THEN
            m := COALESCE(margin, 60) * 1000000;
        ELSIF ver_type ILIKE 'int%' THEN
            IF margin IS NULL THEN
                RAISE EXCEPTION 'vgraph.register_graph: an INT ver_col needs an explicit margin (units of %)', ver_col;
            END IF;
            m := margin;
        ELSE
            RAISE EXCEPTION 'vgraph.register_graph: ver_col % must be TIMESTAMP, TIMESTAMPTZ or INT, not %', ver_col, ver_type;
        END IF;
        IF m < 0 THEN
            RAISE EXCEPTION 'vgraph.register_graph: margin must not be negative';
        END IF;
    END IF;
    IF op_col IS NOT NULL THEN
        op_type := (SELECT data_type FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(edge_table, '.', 1)
                    AND table_name ILIKE SPLIT_PART(edge_table, '.', 2) AND column_name ILIKE op_col);
        IF NOT (op_type ILIKE 'bool%' OR op_type ILIKE 'int%') THEN
            RAISE EXCEPTION 'vgraph.register_graph: op_col % must be BOOLEAN or INT, not %', op_col, op_type;
        END IF;
    END IF;

    PERFORM INSERT INTO vgraph.manifest (graph, edge_table, src_col, dst_col, op_col, weight_col, ver_col, ver_margin, directed)
            VALUES (g, edge_table, src_col, dst_col, op_col, weight_col, ver_col, m, COALESCE(directed, TRUE));
    PERFORM COMMIT;
    PERFORM CALL vgraph.make_delta_view(g);
    RAISE NOTICE 'vgraph: graph % registered. Next: CALL vgraph.refresh_graph(''%''). Queries read %.%_delta; grant SELECT on it to the users who may query the graph.',
                 g, g, SPLIT_PART(edge_table, '.', 1), g;
END;
$$;

-- gload of one snapshot on every node, and a check that every node loaded it.
CREATE OR REPLACE PROCEDURE vgraph.load_on_nodes(g VARCHAR, sid INT) LANGUAGE PLvSQL AS $$
DECLARE
    want INT; got INT;
BEGIN
    want := (SELECT COUNT(*) FROM (SELECT vgraph.gnode(k) OVER(PARTITION NODES) FROM vgraph.probe) n);
    got := EXECUTE 'SELECT /*+LABEL(vgraph_load)*/ COUNT(DISTINCT node_name) FROM (SELECT vgraph.gload(byte_offset, chunk USING PARAMETERS graph='
        || QUOTE_LITERAL(g) || ', snapshot_id=' || sid || ') OVER(PARTITION NODES) FROM (SELECT s.byte_offset, s.chunk '
        || 'FROM vgraph.snapshot s CROSS JOIN vgraph.probe p WHERE s.graph=' || QUOTE_LITERAL(g) || ' AND s.snapshot_id=' || sid
        || ' AND p.k IN (SELECT k FROM (SELECT vgraph.gnode(k) OVER(PARTITION NODES) FROM vgraph.probe) n)) c) l WHERE status = ''loaded''';
    IF got IS NULL OR got < want THEN
        RAISE EXCEPTION 'vgraph.load_on_nodes: graph %, snapshot %: loaded on % of % nodes', g, sid, COALESCE(got, 0), want;
    END IF;
    RAISE NOTICE 'vgraph: graph %, snapshot % loaded on % nodes', g, sid, got;
END;
$$;

-- Cache repair: loads the active snapshot again on every node. Safe at any time.
CREATE OR REPLACE PROCEDURE vgraph.load_all(g VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    sid INT;
BEGIN
    sid := (SELECT MAX(active_snapshot) FROM vgraph.manifest WHERE graph = g);
    IF sid IS NULL THEN
        RAISE EXCEPTION 'vgraph.load_all: graph % is not registered or has no snapshot yet: run vgraph.refresh_graph', g;
    END IF;
    PERFORM CALL vgraph.load_on_nodes(g, sid);
    RAISE NOTICE 'vgraph: graph %: snapshot % loaded on all nodes', g, sid;
END;
$$;

-- How many journal rows a query has to apply now, and how long it takes to read them.
CREATE OR REPLACE PROCEDURE vgraph.status(g VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    tab VARCHAR(256); ver VARCHAR(128); n INT; t0 TIMESTAMPTZ; ms INT;
BEGIN
    tab := (SELECT MAX(edge_table) FROM vgraph.manifest WHERE graph = g);
    IF tab IS NULL THEN
        RAISE EXCEPTION 'vgraph.status: graph % is not registered', g;
    END IF;
    t0 := (SELECT CLOCK_TIMESTAMP());
    n := EXECUTE 'SELECT COUNT(src) FROM (SELECT src, dst, del, weight, ver FROM ' || SPLIT_PART(tab, '.', 1) || '.' || g || '_delta) d';
    ms := (SELECT DATEDIFF('millisecond', t0, CLOCK_TIMESTAMP()));
    RAISE NOTICE 'vgraph: graph %: % journal rows in the delta, read in % ms', g, n, ms;
    n := (SELECT COUNT(DISTINCT transaction_id) FROM v_monitor.locks WHERE LOWER(object_name) = LOWER('Table:' || tab)
          AND (lock_mode ILIKE '%I%' OR lock_mode = 'X'));
    IF n > 0 THEN
        ms := (SELECT DATEDIFF('second', MIN(request_timestamp), CLOCK_TIMESTAMP()) FROM v_monitor.locks
               WHERE LOWER(object_name) = LOWER('Table:' || tab) AND (lock_mode ILIKE '%I%' OR lock_mode = 'X'));
        RAISE NOTICE 'vgraph: graph %: % open transactions are writing to %, the oldest for % seconds. A refresh now keeps their rows in the delta.', g, n, tab, ms;
    END IF;
    IF ms > 500 THEN
        RAISE WARNING 'vgraph: graph %: reading the delta is slow (% ms) and every query pays for it. Usual causes: the journal is not partitioned by the date of its version column, so new rows were merged into old storage; or ENCODING RLE on the src column. Fix: partition the journal by the version date, or refresh more often. See README, section Freshness.', g, ms;
    END IF;
    IF n > 1000000 THEN
        RAISE WARNING 'vgraph: graph %: the delta holds % rows. Refresh more often.', g, n;
    END IF;
    -- A version that lies in the future was not set by the database clock: some writer fills the
    -- version column itself. (A version set too far in the past cannot be recognised afterwards.)
    ver := (SELECT ver_col FROM vgraph.manifest WHERE graph = g);
    IF ver IS NOT NULL AND (SELECT data_type ILIKE 'timestamp%' FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(tab, '.', 1)
                            AND table_name ILIKE SPLIT_PART(tab, '.', 2) AND column_name ILIKE ver) THEN
        n := EXECUTE 'SELECT COUNT(*) FROM ' || tab || ' WHERE ' || ver || ' > CLOCK_TIMESTAMP() + INTERVAL ''5 minutes''';
        IF n > 0 THEN
            RAISE WARNING 'vgraph: graph %: % rows of % have a version in the future. The version column must be filled by its default (CLOCK_TIMESTAMP), never by the application; results can be wrong.', g, n, tab;
        END IF;
    END IF;
END;
$$;

-- build -> insert chunks -> gload on all nodes -> update manifest and delta view -> delete older snapshots.
CREATE OR REPLACE PROCEDURE vgraph.refresh_graph(g VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    tab VARCHAR(256); src VARCHAR(128); dst VARCHAR(128); op VARCHAR(128); w VARCHAR(128); ver VARCHAR(128);
    dir BOOLEAN; margin INT; op_type VARCHAR(128); ver_type VARCHAR(128);
    prev INT; sid INT; chunks INT; max_ver INT; v_from VARCHAR(64);
    cols VARCHAR(1000); source VARCHAR(4000); del_expr VARCHAR(400); del_a VARCHAR(400); del_k VARCHAR(400);
    t0 TIMESTAMPTZ; cut TIMESTAMPTZ; secs FLOAT; nodes INT; edges INT; fmt INT;
    budget INT; est_rows INT; est_mb INT; how VARCHAR(16); t_map VARCHAR(200); t_edges VARCHAR(200);
    pair VARCHAR(200); stmt VARCHAR(8000); flags VARCHAR(1000); head VARCHAR(1000); n_nodes INT; n_edges INT; n INT; same BOOLEAN; declared BOOLEAN;
BEGIN
    tab := (SELECT MAX(edge_table) FROM vgraph.manifest WHERE graph = g);
    IF tab IS NULL THEN
        RAISE EXCEPTION 'vgraph.refresh_graph: graph % is not registered', g;
    END IF;
    src := (SELECT src_col FROM vgraph.manifest WHERE graph = g);
    dst := (SELECT dst_col FROM vgraph.manifest WHERE graph = g);
    op := (SELECT op_col FROM vgraph.manifest WHERE graph = g);
    w := (SELECT weight_col FROM vgraph.manifest WHERE graph = g);
    ver := (SELECT ver_col FROM vgraph.manifest WHERE graph = g);
    dir := (SELECT directed FROM vgraph.manifest WHERE graph = g);
    margin := (SELECT ver_margin FROM vgraph.manifest WHERE graph = g);
    prev := (SELECT active_snapshot FROM vgraph.manifest WHERE graph = g);
    declared := (SELECT both_directions FROM vgraph.manifest WHERE graph = g);

    IF prev IS NOT NULL THEN
        PERFORM CALL vgraph.status(g);      -- reports a slow or large delta before it is folded into the new snapshot
    END IF;
    t0 := (SELECT CLOCK_TIMESTAMP());

    -- 1. Delta boundary, taken BEFORE the build reads the table.
    --    A row is missing from the snapshot only if it is committed after the build read starts.
    --    Timestamp version (insertion clock time): such a row was written either after now, or by a
    --    transaction that is open right now. An open writer holds an insert lock on the table, and
    --    v_monitor.locks shows when it asked for it. So:
    --        boundary = LEAST(now, earliest lock request of an open writer) - margin
    --    The margin only has to cover clock differences between nodes and versions taken at statement
    --    start (SYSDATE) instead of at write time (CLOCK_TIMESTAMP).
    --    INT version: highest version minus the margin; the margin has to cover open writers.
    --    Rows that are in both the snapshot and the delta are harmless.
    max_ver := 0;
    v_from := NULL;
    IF ver IS NOT NULL THEN
        ver_type := (SELECT data_type FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(tab, '.', 1)
                     AND table_name ILIKE SPLIT_PART(tab, '.', 2) AND column_name ILIKE ver);
        IF ver_type ILIKE 'int%' THEN
            max_ver := EXECUTE 'SELECT MAX(' || ver || ')::INT FROM ' || tab;
            v_from := (max_ver - margin)::VARCHAR;
        ELSE
            max_ver := EXECUTE 'SELECT MAX((EXTRACT(EPOCH FROM ' || ver || ') * 1000000)::INT) FROM ' || tab;
            cut := (SELECT LEAST(CLOCK_TIMESTAMP(), COALESCE(MIN(request_timestamp), CLOCK_TIMESTAMP()))
                    FROM v_monitor.locks
                    WHERE LOWER(object_name) = LOWER('Table:' || tab)
                      AND (lock_mode ILIKE '%I%' OR lock_mode = 'X')
                      AND transaction_id <> (SELECT transaction_id FROM v_monitor.current_session));
            cut := (SELECT cut - (margin // 1000000) * INTERVAL '1 second');
            IF ver_type ILIKE 'timestamptz%' OR ver_type ILIKE '%with time zone%' THEN
                v_from := (SELECT 'TIMESTAMPTZ ''' || TO_CHAR(cut AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS.US') || '+00''');
            ELSE
                -- Plain TIMESTAMP versions are local times of the writing session: all writers and this
                -- session must use the same time zone.
                v_from := (SELECT 'TIMESTAMP ''' || TO_CHAR(cut::TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS.US') || '''');
            END IF;
        END IF;
    END IF;

    -- 2. Consolidated edges: the latest row of every (src, dst) by version, kept if it is not a delete.
    --    Unweighted: gbuild stores a repeated edge once, so only edges that were ever deleted need
    --    ranking. All add rows are taken, minus the edges whose latest row is a delete. That is one
    --    scan plus a small join, instead of ranking the whole journal.
    --    Weighted: the latest weight must win, so every edge is ranked.
    cols := src || '::INT AS src, ' || dst || '::INT AS dst';
    IF w IS NOT NULL THEN
        cols := cols || ', ' || w || '::FLOAT AS weight';
    END IF;
    IF op IS NULL THEN
        source := 'SELECT ' || cols || ' FROM ' || tab || ' WHERE ' || src || ' IS NOT NULL AND ' || dst || ' IS NOT NULL';
    ELSE
        op_type := (SELECT data_type FROM v_catalog.columns WHERE table_schema ILIKE SPLIT_PART(tab, '.', 1)
                    AND table_name ILIKE SPLIT_PART(tab, '.', 2) AND column_name ILIKE op);
        del_expr := CASE WHEN op_type ILIKE 'bool%' THEN 'COALESCE(' || op || ', FALSE)' ELSE '(COALESCE(' || op || ', 1) < 0)' END;
        IF w IS NOT NULL THEN
            source := 'SELECT src, dst, weight FROM (SELECT ' || cols || ', ' || del_expr || ' AS del, ROW_NUMBER() OVER(PARTITION BY '
                   || src || ', ' || dst || ' ORDER BY ' || ver || ' DESC) AS rn FROM ' || tab || ' WHERE ' || src || ' IS NOT NULL AND '
                   || dst || ' IS NOT NULL) j WHERE rn = 1 AND NOT del';
        ELSE
            IF op_type ILIKE 'bool%' THEN
                del_a := 'COALESCE(a.' || op || ', FALSE)';
                del_k := 'COALESCE(k.' || op || ', FALSE)';
            ELSE
                del_a := '(COALESCE(a.' || op || ', 1) < 0)';
                del_k := '(COALESCE(k.' || op || ', 1) < 0)';
            END IF;
            source := 'SELECT a.' || src || '::INT AS src, a.' || dst || '::INT AS dst FROM ' || tab || ' a LEFT JOIN ('
                   || 'SELECT s, d FROM (SELECT k.' || src || ' AS s, k.' || dst || ' AS d, ' || del_k || ' AS del, '
                   || 'ROW_NUMBER() OVER(PARTITION BY k.' || src || ', k.' || dst || ' ORDER BY k.' || ver || ' DESC) AS rn FROM ' || tab || ' k '
                   || 'WHERE (k.' || src || ', k.' || dst || ') IN (SELECT ' || src || ', ' || dst || ' FROM ' || tab || ' WHERE ' || del_expr || ')) r '
                   || 'WHERE rn = 1 AND del) dead ON dead.s = a.' || src || ' AND dead.d = a.' || dst
                   || ' WHERE dead.s IS NULL AND NOT ' || del_a
                   || ' AND a.' || src || ' IS NOT NULL AND a.' || dst || ' IS NOT NULL';
        END IF;
    END IF;

    -- 3. Build and store the chunks.
    --    Snapshot ids come from a sequence: they never repeat, also not after unregister and register,
    --    so a cache file left behind by an older graph of the same name is always recognised as stale.
    sid := (SELECT NEXTVAL('vgraph.snapshot_seq'));
    --    Two ways to build, same bytes:
    --    in memory (gbuild): fast, needs about 16 bytes per edge row plus 32 per node in the UDx process.
    --    streaming (gbuild_mapped): Vertica builds the node map and the mapped, sorted edges in tables,
    --    which spill to disk; the function needs one chunk of memory. Chosen when the estimate is
    --    above vgraph.manifest.build_memory_mb (0 = always stream).
    budget := (SELECT COALESCE(build_memory_mb, 4096) FROM vgraph.manifest WHERE graph = g);
    est_rows := EXECUTE 'SELECT COUNT(*) FROM ' || tab;
    IF NOT dir THEN
        est_rows := est_rows * 2;
    END IF;
    est_mb := (est_rows * 24) // 1000000;       -- 16 per edge row, and up to 32 per node at about 4 rows per node
    IF est_mb <= budget THEN
        how := 'memory';
        EXECUTE 'INSERT /*+LABEL(vgraph_build)*/ INTO vgraph.snapshot SELECT ' || QUOTE_LITERAL(g) || ', ' || sid || ', byte_offset, chunk FROM (SELECT vgraph.gbuild(src, dst'
             || CASE WHEN w IS NOT NULL THEN ', weight' ELSE '' END || ' USING PARAMETERS graph=' || QUOTE_LITERAL(g)
             || ', directed=' || CASE WHEN dir THEN 'true' ELSE 'false' END || ', max_ver=' || COALESCE(max_ver, 0)
             || ') OVER(ORDER BY src, dst) FROM (' || source || ') e) b';
        PERFORM COMMIT;
    ELSE
        how := 'streaming';
        t_map := 'vgraph.build_' || g || '_map';
        t_edges := 'vgraph.build_' || g || '_edges';
        EXECUTE 'DROP TABLE IF EXISTS ' || t_map || ' CASCADE';
        EXECUTE 'DROP TABLE IF EXISTS ' || t_edges || ' CASCADE';
        -- a. consolidated edges, once
        EXECUTE 'CREATE TABLE ' || t_edges || '_raw AS SELECT /*+LABEL(vgraph_build)*/ * FROM (' || source || ') e';
        -- b. node map: position = rank of the id
        EXECUTE 'CREATE TABLE ' || t_map || ' AS SELECT /*+LABEL(vgraph_build)*/ id, ROW_NUMBER() OVER(ORDER BY id) - 1 AS pos FROM (SELECT src AS id FROM '
             || t_edges || '_raw UNION SELECT dst FROM ' || t_edges || '_raw) u ORDER BY id SEGMENTED BY HASH(id) ALL NODES';
        -- c. edges as positions, unique; both directions for an undirected graph
        pair := CASE WHEN w IS NOT NULL THEN ', MIN(e.weight) AS weight' ELSE '' END;
        stmt := 'SELECT ms.pos AS s, md.pos AS d' || pair || ' FROM ' || t_edges || '_raw e JOIN ' || t_map || ' ms ON ms.id = e.src JOIN '
             || t_map || ' md ON md.id = e.dst GROUP BY 1, 2';
        IF NOT dir THEN
            stmt := 'SELECT s, d' || CASE WHEN w IS NOT NULL THEN ', MIN(weight) AS weight' ELSE '' END || ' FROM (SELECT s, d'
                 || CASE WHEN w IS NOT NULL THEN ', weight' ELSE '' END || ' FROM (' || stmt || ') f UNION ALL SELECT d, s'
                 || CASE WHEN w IS NOT NULL THEN ', weight' ELSE '' END || ' FROM (' || stmt || ') r WHERE s <> d) b GROUP BY 1, 2';
        END IF;
        EXECUTE 'CREATE TABLE ' || t_edges || ' AS SELECT /*+LABEL(vgraph_build)*/ * FROM (' || stmt || ') m ORDER BY s, d SEGMENTED BY HASH(s) ALL NODES';
        EXECUTE 'DROP TABLE IF EXISTS ' || t_edges || '_raw CASCADE';
        n_nodes := EXECUTE 'SELECT COUNT(*) FROM ' || t_map;
        n_edges := EXECUTE 'SELECT COUNT(*) FROM ' || t_edges;

        -- d. Does the table store both directions of every edge? Then no reverse CSR is needed.
        --    Cheap test first (order-independent hash sums), exact test only if that one says yes.
        --    The exact test is an anti join of the edges with themselves: minutes at a billion edges.
        --    vgraph.manifest.both_directions = TRUE declares the answer and skips it. The cheap test
        --    still runs: a wrong declaration would give wrong results for direction 'in'.
        same := FALSE;
        IF dir AND n_edges > 0 THEN
            pair := CASE WHEN w IS NOT NULL THEN ', weight' ELSE '' END;
            same := EXECUTE 'SELECT SUM(HASH(s, d' || pair || ') % 1000000007) = SUM(HASH(d, s' || pair || ') % 1000000007) FROM ' || t_edges;
            IF COALESCE(declared, FALSE) AND NOT same THEN
                EXECUTE 'DROP TABLE IF EXISTS ' || t_map || ' CASCADE';
                EXECUTE 'DROP TABLE IF EXISTS ' || t_edges || ' CASCADE';
                RAISE EXCEPTION 'vgraph.refresh_graph: graph %: both_directions is declared, but table % does not store every edge in both directions', g, tab;
            END IF;
            IF same AND NOT COALESCE(declared, FALSE) THEN
                n := EXECUTE 'SELECT COUNT(*) FROM ' || t_edges || ' a LEFT JOIN ' || t_edges || ' b ON b.s = a.d AND b.d = a.s'
                  || CASE WHEN w IS NOT NULL THEN ' AND b.weight = a.weight' ELSE '' END || ' WHERE b.s IS NULL';
                same := (n = 0);
            END IF;
        END IF;

        -- e. one statement per section
        flags := ' USING PARAMETERS graph=' || QUOTE_LITERAL(g) || ', node_count=' || n_nodes || ', edge_count=' || n_edges
              || ', directed=' || CASE WHEN dir THEN 'true' ELSE 'false' END
              || ', weighted=' || CASE WHEN w IS NOT NULL THEN 'true' ELSE 'false' END
              || ', in_equals_out=' || CASE WHEN same THEN 'true' ELSE 'false' END || ', max_ver=' || COALESCE(max_ver, 0);
        head := 'INSERT /*+LABEL(vgraph_build)*/ INTO vgraph.snapshot SELECT ' || QUOTE_LITERAL(g) || ', ' || sid || ', byte_offset, chunk FROM (SELECT vgraph.gbuild_mapped(';
        EXECUTE head || 'a, b' || flags || ', section=''ids'') OVER(ORDER BY a, b) FROM (SELECT pos AS a, id AS b FROM ' || t_map || ') q) x';
        EXECUTE head || 'a, b' || flags || ', section=''out_offsets'') OVER(ORDER BY a, b) FROM (SELECT s AS a, COUNT(*) AS b FROM ' || t_edges || ' GROUP BY s) q) x';
        EXECUTE head || 'a, b' || flags || ', section=''out_nbrs'') OVER(ORDER BY a, b) FROM (SELECT s AS a, d AS b FROM ' || t_edges || ') q) x';
        IF w IS NOT NULL THEN
            EXECUTE head || 'a, b, w' || flags || ', section=''out_weights'') OVER(ORDER BY a, b) FROM (SELECT s AS a, d AS b, weight AS w FROM ' || t_edges || ') q) x';
        END IF;
        IF dir AND NOT same THEN
            EXECUTE head || 'a, b' || flags || ', section=''in_offsets'') OVER(ORDER BY a, b) FROM (SELECT d AS a, COUNT(*) AS b FROM ' || t_edges || ' GROUP BY d) q) x';
            EXECUTE head || 'a, b' || flags || ', section=''in_nbrs'') OVER(ORDER BY a, b) FROM (SELECT d AS a, s AS b FROM ' || t_edges || ') q) x';
            IF w IS NOT NULL THEN
                EXECUTE head || 'a, b, w' || flags || ', section=''in_weights'') OVER(ORDER BY a, b) FROM (SELECT d AS a, s AS b, weight AS w FROM ' || t_edges || ') q) x';
            END IF;
        END IF;
        PERFORM COMMIT;
        -- f. the header, from the checksum rows of the sections (byte_offset < 0)
        EXECUTE 'INSERT /*+LABEL(vgraph_build)*/ INTO vgraph.snapshot SELECT ' || QUOTE_LITERAL(g) || ', ' || sid || ', byte_offset, chunk FROM (SELECT vgraph.gbuild_header(byte_offset, chunk'
             || flags || ') OVER() FROM vgraph.snapshot WHERE graph = ' || QUOTE_LITERAL(g) || ' AND snapshot_id = ' || sid || ' AND byte_offset < 0) h';
        PERFORM DELETE FROM vgraph.snapshot WHERE graph = g AND snapshot_id = sid AND byte_offset < 0;
        PERFORM COMMIT;
        EXECUTE 'DROP TABLE IF EXISTS ' || t_map || ' CASCADE';
        EXECUTE 'DROP TABLE IF EXISTS ' || t_edges || ' CASCADE';
    END IF;
    chunks := (SELECT COUNT(*) FROM vgraph.snapshot WHERE graph = g AND snapshot_id = sid);
    IF chunks = 0 THEN
        RAISE EXCEPTION 'vgraph.refresh_graph: graph %: table % has no edges, nothing to build', g, tab;
    END IF;

    -- 4. Load on every node. Until the manifest changes, queries keep using the previous snapshot's view.
    PERFORM CALL vgraph.load_on_nodes(g, sid);

    -- 5. Manifest and delta view.
    nodes := EXECUTE 'SELECT MAX(node_count) FROM (SELECT vgraph.ginfo(USING PARAMETERS graph=' || QUOTE_LITERAL(g) || ') OVER(PARTITION NODES) FROM vgraph.probe) i WHERE snapshot_id = ' || sid;
    edges := EXECUTE 'SELECT MAX(edge_count) FROM (SELECT vgraph.ginfo(USING PARAMETERS graph=' || QUOTE_LITERAL(g) || ') OVER(PARTITION NODES) FROM vgraph.probe) i WHERE snapshot_id = ' || sid;
    fmt := (SELECT format_version FROM (SELECT vgraph.gversion() OVER()) v);
    secs := (SELECT DATEDIFF('millisecond', t0, CLOCK_TIMESTAMP()) / 1000.0);
    PERFORM UPDATE vgraph.manifest SET active_snapshot = sid, active_max_ver = max_ver, delta_from = v_from,
                   node_count = nodes, edge_count = edges, built_at = CLOCK_TIMESTAMP(), build_seconds = secs, format_version = fmt
            WHERE graph = g;
    PERFORM COMMIT;
    PERFORM CALL vgraph.make_delta_view(g);

    -- 6. Keep the active and the previous snapshot.
    PERFORM DELETE FROM vgraph.snapshot WHERE graph = g AND snapshot_id < COALESCE(prev, sid);
    PERFORM COMMIT;
    RAISE NOTICE 'vgraph: graph % refreshed: snapshot %, % nodes, % edges, % seconds, % build', g, sid, nodes, edges, secs, how;
END;
$$;

-- Runs vgraph.refresh_graph(graph) on a cron schedule, for example '*/15 * * * *'.
CREATE OR REPLACE PROCEDURE vgraph.schedule_refresh(g VARCHAR, cron_expr VARCHAR) LANGUAGE PLvSQL AS $$
BEGIN
    IF (SELECT COUNT(*) FROM vgraph.manifest WHERE graph = g) = 0 THEN
        RAISE EXCEPTION 'vgraph.schedule_refresh: graph % is not registered', g;
    END IF;
    IF cron_expr IS NULL OR NOT REGEXP_LIKE(cron_expr, '^[0-9*/, -]+$') THEN
        RAISE EXCEPTION 'vgraph.schedule_refresh: cron_expr may hold digits, spaces and * / , - only';
    END IF;
    EXECUTE 'DROP TRIGGER IF EXISTS vgraph.' || g || '_refresh_trigger';
    EXECUTE 'DROP SCHEDULE IF EXISTS vgraph.' || g || '_refresh_schedule';
    EXECUTE 'CREATE SCHEDULE vgraph.' || g || '_refresh_schedule USING CRON ' || QUOTE_LITERAL(cron_expr);
    EXECUTE 'CREATE TRIGGER vgraph.' || g || '_refresh_trigger ON SCHEDULE vgraph.' || g
         || '_refresh_schedule EXECUTE PROCEDURE vgraph.refresh_graph(' || QUOTE_LITERAL(g) || ') AS DEFINER';
    RAISE NOTICE 'vgraph: graph % is refreshed on schedule %', g, cron_expr;
END;
$$;

-- Removes the schedule, the delta view, the snapshots and the manifest row.
-- Cache files stay on the nodes: no vgraph function deletes paths on request. Remove <cache_dir>/<graph> by hand.
CREATE OR REPLACE PROCEDURE vgraph.unregister_graph(g VARCHAR) LANGUAGE PLvSQL AS $$
DECLARE
    tab VARCHAR(256);
BEGIN
    tab := (SELECT MAX(edge_table) FROM vgraph.manifest WHERE graph = g);
    IF tab IS NULL THEN
        RAISE EXCEPTION 'vgraph.unregister_graph: graph % is not registered', g;
    END IF;
    EXECUTE 'DROP TRIGGER IF EXISTS vgraph.' || g || '_refresh_trigger';
    EXECUTE 'DROP SCHEDULE IF EXISTS vgraph.' || g || '_refresh_schedule';
    EXECUTE 'DROP VIEW IF EXISTS ' || SPLIT_PART(tab, '.', 1) || '.' || g || '_delta';
    PERFORM DELETE FROM vgraph.snapshot WHERE graph = g;
    PERFORM DELETE FROM vgraph.manifest WHERE graph = g;
    PERFORM COMMIT;
    RAISE NOTICE 'vgraph: graph % unregistered. Cache files under <cache_dir>/% stay on the nodes.', g, g;
END;
$$;

GRANT EXECUTE ON PROCEDURE vgraph.register_graph(VARCHAR, VARCHAR, VARCHAR, VARCHAR, VARCHAR, VARCHAR, BOOLEAN, VARCHAR, INT) TO vgraph_admin;
GRANT EXECUTE ON PROCEDURE vgraph.refresh_graph(VARCHAR) TO vgraph_admin;
GRANT EXECUTE ON PROCEDURE vgraph.load_all(VARCHAR) TO vgraph_admin;
GRANT EXECUTE ON PROCEDURE vgraph.status(VARCHAR) TO vgraph_admin;
GRANT EXECUTE ON PROCEDURE vgraph.schedule_refresh(VARCHAR, VARCHAR) TO vgraph_admin;
GRANT EXECUTE ON PROCEDURE vgraph.unregister_graph(VARCHAR) TO vgraph_admin;
