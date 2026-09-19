-- vgraph install. Run by scripts/deploy.sh, which sets two vsql variables:
--   libfile  quoted absolute path of libvgraph.so on the initiator node
--   fenced   FENCED or NOT FENCED
\set ON_ERROR_STOP on

CREATE OR REPLACE LIBRARY vgraph AS :libfile LANGUAGE 'C++';

CREATE OR REPLACE TRANSFORM FUNCTION gversion
    AS LANGUAGE 'C++' NAME 'GVersionFactory' LIBRARY vgraph :fenced;

GRANT EXECUTE ON TRANSFORM FUNCTION gversion() TO PUBLIC;

CREATE OR REPLACE TRANSFORM FUNCTION gkhop
    AS LANGUAGE 'C++' NAME 'GKhopFactory' LIBRARY vgraph :fenced;

GRANT EXECUTE ON TRANSFORM FUNCTION gkhop(INT, INT, INT, INT, INT, INT, INT) TO PUBLIC;
