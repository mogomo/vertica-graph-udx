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
    chunk_no     INT NOT NULL,
    chunk        LONG VARBINARY(8388608) NOT NULL
) ORDER BY graph, snapshot_id, chunk_no UNSEGMENTED ALL NODES;

CREATE TABLE IF NOT EXISTS vgraph.manifest (
    graph             VARCHAR(64) NOT NULL PRIMARY KEY,
    edge_table        VARCHAR(256) NOT NULL,
    src_col           VARCHAR(128) NOT NULL,
    dst_col           VARCHAR(128) NOT NULL,
    op_col            VARCHAR(128),
    weight_col        VARCHAR(128),
    directed          BOOLEAN NOT NULL,
    active_snapshot   INT,
    active_max_epoch  INT,
    node_count        INT,
    edge_count        INT,
    built_at          TIMESTAMPTZ,
    build_seconds     FLOAT,
    format_version    INT
) UNSEGMENTED ALL NODES;

-- One row, so that ginfo() OVER(PARTITION NODES) has input on every node.
CREATE TABLE IF NOT EXISTS vgraph.probe (one INT NOT NULL) UNSEGMENTED ALL NODES;
INSERT INTO vgraph.probe SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM vgraph.probe);
COMMIT;

-- CREATE ROLE has no IF NOT EXISTS: on a second install the error is expected.
\set ON_ERROR_STOP off
CREATE ROLE vgraph_admin;
\set ON_ERROR_STOP on

-- Functions.
CREATE OR REPLACE TRANSFORM FUNCTION gversion    AS LANGUAGE 'C++' NAME 'GVersionFactory'    LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION gbuild      AS LANGUAGE 'C++' NAME 'GBuildFactory'      LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION gload       AS LANGUAGE 'C++' NAME 'GLoadFactory'       LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION ginfo       AS LANGUAGE 'C++' NAME 'GInfoFactory'       LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION gkhop       AS LANGUAGE 'C++' NAME 'GKhopFactory'       LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION gpath       AS LANGUAGE 'C++' NAME 'GPathFactory'       LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION gcomponents AS LANGUAGE 'C++' NAME 'GComponentsFactory' LIBRARY vgraph :fenced;
CREATE OR REPLACE TRANSFORM FUNCTION gpagerank   AS LANGUAGE 'C++' NAME 'GPagerankFactory'   LIBRARY vgraph :fenced;

-- Query functions: everyone. Build and load: vgraph_admin only.
GRANT USAGE ON SCHEMA vgraph TO PUBLIC;
GRANT SELECT ON vgraph.manifest, vgraph.probe TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION gversion() TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION ginfo() TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION gkhop(INT, INT, INT, INT, INT, INT, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION gpath(INT, INT, INT, INT, INT, INT, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION gcomponents(INT, INT, INT, INT, INT, INT, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION gpagerank(INT, INT, INT, INT, INT, INT, INT) TO PUBLIC;
GRANT EXECUTE ON TRANSFORM FUNCTION gbuild(INT, INT, FLOAT, INT) TO vgraph_admin;
GRANT EXECUTE ON TRANSFORM FUNCTION gload(INT, LONG VARBINARY) TO vgraph_admin;
GRANT ALL ON vgraph.snapshot, vgraph.manifest TO vgraph_admin;
