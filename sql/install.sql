-- vgraph install. Run by scripts/deploy.sh, which sets two vsql variables:
--   libfile  quoted absolute path of libvgraph.so on the initiator node
--   fenced   FENCED or NOT FENCED
-- Safe to run again: tables and their data are kept.
\set ON_ERROR_STOP on

CREATE OR REPLACE LIBRARY vgraph AS :libfile LANGUAGE 'C++';

-- Catalog. UNSEGMENTED ALL NODES: every node holds a full copy, so the
-- snapshot is backed up and replicated like any other table.
CREATE SCHEMA IF NOT EXISTS vgraph;

CREATE TABLE IF NOT EXISTS vgraph.snapshot (
    graph        VARCHAR(64) NOT NULL,
    snapshot_id  INT NOT NULL,
    byte_offset  INT NOT NULL,               -- where the piece goes in the snapshot file
    chunk        LONG VARBINARY(8388608) NOT NULL
) ORDER BY graph, snapshot_id, byte_offset UNSEGMENTED ALL NODES;

CREATE TABLE IF NOT EXISTS vgraph.manifest (
    graph             VARCHAR(64) NOT NULL PRIMARY KEY,
    edge_table        VARCHAR(256) NOT NULL,
    src_col           VARCHAR(128) NOT NULL,
    dst_col           VARCHAR(128) NOT NULL,
    op_col            VARCHAR(128),           -- BOOLEAN (true = deleted) or INT (+1 / -1)
    weight_col        VARCHAR(128),
    ver_col           VARCHAR(128),           -- TIMESTAMP or INT; orders the journal
    ver_margin        INT,                    -- overlap of the delta, in units of ver (microseconds for timestamps)
    directed          BOOLEAN NOT NULL,
    active_snapshot   INT,
    active_max_ver    INT,
    delta_from        VARCHAR(64),            -- SQL literal: the delta view reads ver_col > delta_from
    node_count        INT,
    edge_count        INT,
    built_at          TIMESTAMPTZ,
    build_seconds     FLOAT,
    format_version    INT,
    build_memory_mb   INT DEFAULT 4096,       -- above this estimate refresh_graph uses the streaming build
    both_directions   BOOLEAN                 -- TRUE = declared: the table stores every edge in both directions (skips the exact test of the streaming build)
) UNSEGMENTED ALL NODES;

-- An install from before this column existed gets it here.
ALTER TABLE vgraph.manifest ADD COLUMN IF NOT EXISTS both_directions BOOLEAN;

-- Snapshot ids. Never reused, so an old cache file can never pass as a newer snapshot.
CREATE SEQUENCE IF NOT EXISTS vgraph.snapshot_seq CACHE 1;

-- Rows on every node, so that functions with OVER(PARTITION NODES) run on
-- every node. It must be segmented: Vertica reads an unsegmented table on one
-- node only. An older one-row probe table is replaced.
\set ON_ERROR_STOP off
DROP TABLE IF EXISTS vgraph.probe CASCADE;
\set ON_ERROR_STOP on
CREATE TABLE vgraph.probe (k INT NOT NULL) SEGMENTED BY HASH(k) ALL NODES;
INSERT INTO vgraph.probe
SELECT ROW_NUMBER() OVER()
FROM (SELECT 1 FROM (SELECT '2000-01-01 00:00:00'::TIMESTAMP AS t UNION ALL SELECT '2000-01-01 00:17:03'::TIMESTAMP) b
      TIMESERIES ts AS '1 second' OVER (ORDER BY t)) g;
COMMIT;

-- CREATE ROLE has no IF NOT EXISTS: on a second install the error is expected.
\set ON_ERROR_STOP off
CREATE ROLE vgraph_admin;
\set ON_ERROR_STOP on

-- Functions live in schema vgraph. Call them as vgraph.gkhop(...) or put vgraph
-- on the search path.
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gversion    AS LANGUAGE 'C++' NAME 'GVersionFactory'    LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gbuild      AS LANGUAGE 'C++' NAME 'GBuildFactory'      LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gbuild      AS LANGUAGE 'C++' NAME 'GBuildWeightedFactory' LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gbuild_mapped AS LANGUAGE 'C++' NAME 'GBuildMappedFactory' LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gbuild_mapped AS LANGUAGE 'C++' NAME 'GBuildMappedWeightedFactory' LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gbuild_header AS LANGUAGE 'C++' NAME 'GBuildHeaderFactory' LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gload       AS LANGUAGE 'C++' NAME 'GLoadFactory'       LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gnode       AS LANGUAGE 'C++' NAME 'GNodeFactory'       LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.ginfo       AS LANGUAGE 'C++' NAME 'GInfoFactory'       LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gkhop       AS LANGUAGE 'C++' NAME 'GKhopFactory'       LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gkhop_count AS LANGUAGE 'C++' NAME 'GKhopCountFactory'  LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gpath       AS LANGUAGE 'C++' NAME 'GPathFactory'       LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gcomponents AS LANGUAGE 'C++' NAME 'GComponentsFactory' LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION vgraph.gpagerank   AS LANGUAGE 'C++' NAME 'GPagerankFactory'   LIBRARY vgraph :fenced;

-- Query functions: everyone. Build and load: vgraph_admin only.
GRANT USAGE ON SCHEMA vgraph TO PUBLIC;
GRANT SELECT ON vgraph.manifest, vgraph.probe TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gversion() TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.ginfo() TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gnode(INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gkhop(INT, INT, INT, INT, BOOLEAN, FLOAT, INT, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gkhop_count(INT, INT, INT, INT, BOOLEAN, FLOAT, INT, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gpath(INT, INT, INT, INT, BOOLEAN, FLOAT, INT, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gcomponents(INT, INT, INT, INT, BOOLEAN, FLOAT, INT, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gpagerank(INT, INT, INT, INT, BOOLEAN, FLOAT, INT, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gbuild(INT, INT) TO vgraph_admin;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gbuild(INT, INT, FLOAT) TO vgraph_admin;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gload(INT, LONG VARBINARY) TO vgraph_admin;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gbuild_mapped(INT, INT) TO vgraph_admin;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gbuild_mapped(INT, INT, FLOAT) TO vgraph_admin;
GRANT EXECUTE ON TRANSFORM FUNCTION vgraph.gbuild_header(INT, LONG VARBINARY) TO vgraph_admin;
GRANT ALL ON vgraph.snapshot, vgraph.manifest TO vgraph_admin;
GRANT SELECT ON SEQUENCE vgraph.snapshot_seq TO vgraph_admin;

-- Stored procedures: register_graph, refresh, load_all, schedule_refresh, unregister_graph.
\i sql/procedures.sql
