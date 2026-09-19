-- vgraph uninstall. Dropping the library with CASCADE drops its functions.
\set ON_ERROR_STOP on

DROP LIBRARY IF EXISTS vgraph CASCADE;

-- The snapshots and the manifest are kept. To remove them as well:
--   DROP SCHEMA vgraph CASCADE;
--   DROP ROLE vgraph_admin;
-- Cache files are under <cache_dir>/<graph>/ on every node (default /tmp/vgraph).
