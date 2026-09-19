-- vgraph uninstall. Dropping the library with CASCADE drops its functions.
\set ON_ERROR_STOP on

DROP LIBRARY IF EXISTS vgraph CASCADE;
