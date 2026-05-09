CREATE EXTENSION toastapi;
SET search_path = pgpro_toast, public, pg_catalog;
SET default_toast_compression=pglz;

CREATE FUNCTION dummy_toaster_handler(internal)
RETURNS internal
AS '$libdir/toastapi'
LANGUAGE C;

CREATE FUNCTION dummy2_toaster_handler(internal)
RETURNS internal
AS '$libdir/toastapi', 'dummy_toaster_handler'
LANGUAGE C;

-- add_toaster()
SELECT add_toaster(NULL, NULL);
SELECT add_toaster(NULL, 'foo');
SELECT add_toaster('bar', NULL);
SELECT add_toaster('', '');
SELECT add_toaster('foo', '');
SELECT add_toaster('foo', 'bar');
SELECT add_toaster('dummy', 'foo');

SELECT add_toaster('dummy', 'dummy_toaster_handler') AS dummy_toaster_oid
\gset

SELECT :dummy_toaster_oid <> 0;
SELECT add_toaster('dummy', 'foo');
SELECT :dummy_toaster_oid = add_toaster('dummy', 'dummy_toaster_handler');

SELECT add_toaster('foo', 'dummy_toaster_handler') AS foo_toaster_oid
\gset

SELECT :foo_toaster_oid <> 0;
SELECT :foo_toaster_oid <> :dummy_toaster_oid;
SELECT :foo_toaster_oid = add_toaster('foo', 'dummy_toaster_handler');

-- drop_toaster()
SELECT drop_toaster(NULL);
SELECT drop_toaster('');
SELECT drop_toaster('bar');
SELECT drop_toaster('foo') = :foo_toaster_oid;
SELECT drop_toaster('foo');


CREATE TABLE tab (id int, jb jsonb, b_comp bytea, b_uncomp bytea);
ALTER TABLE tab ALTER COLUMN b_uncomp SET STORAGE external;

-- set_toaster()
SELECT set_toaster('', '', '');
SELECT set_toaster('foo', 'bar', 'baz');
SELECT set_toaster('foo', 'tab', 'baz');
SELECT set_toaster('dummy', 'tab', 'bar');
SELECT set_toaster('dummy', 'tab', 'id');
SELECT set_toaster('dummy', 'tab', 'jb');
SELECT set_toaster('dummy', 'tab', 'b_uncomp');
SELECT set_toaster('dummy', 'tab', 'b_comp') = :dummy_toaster_oid;

-- get_toaster()
SELECT get_toaster('', '');
SELECT get_toaster('foo', 'bar');
SELECT get_toaster('tab', 'bar');
SELECT get_toaster('tab', 'id');
SELECT get_toaster('tab', 'jb');
SELECT get_toaster('tab', 'b_uncomp');
SELECT get_toaster('tab', 'b_comp') = :dummy_toaster_oid;

-- get_toaster_id()
SELECT get_toaster_id('');
SELECT get_toaster_id(NULL);
SELECT get_toaster_id('dummy') = :dummy_toaster_oid;
SELECT get_toaster_id('incorrect_toaster_name');

-- reset_toaster()
SELECT reset_toaster('', '');
SELECT reset_toaster('foo', 'bar');
SELECT reset_toaster('tab', 'bar');
SELECT reset_toaster('tab', 'id');
SELECT reset_toaster('tab', 'jb');
SELECT reset_toaster('tab', 'b_uncomp');
SELECT reset_toaster('tab', 'b_comp');
SELECT get_toaster('tab', 'b_comp');
SELECT reset_toaster('tab', 'b_comp');
SELECT get_toaster('tab', 'b_comp');

SELECT set_toaster('dummy', 'tab', 'b_comp') = :dummy_toaster_oid;
SELECT get_toaster('tab', 'b_comp') = :dummy_toaster_oid;
SELECT reset_toaster('tab', 'b_comp');
SELECT get_toaster('tab', 'b_comp');
SELECT set_toaster('dummy', 'tab', 'b_comp') = :dummy_toaster_oid;

-- drop_toaster()
SELECT drop_toaster(NULL);
SELECT drop_toaster('');
SELECT drop_toaster('foo');
SELECT drop_toaster('dummy');
SELECT drop_toaster('dummy');
SELECT reset_toaster('tab', 'b_comp');
SELECT drop_toaster('dummy') = :dummy_toaster_oid;
SELECT drop_toaster('dummy');

-- test retoast
SELECT add_toaster('dummy', 'dummy_toaster_handler') <> 0;
SELECT add_toaster('dummy2', 'dummy2_toaster_handler') <> 0;

CREATE TABLE test_dummy_toaster(id int, data bytea);
SELECT set_toaster('dummy', 'test_dummy_toaster', 'data') <> 0;

INSERT INTO test_dummy_toaster
SELECT id, repeat('a', 1000000)::bytea
FROM generate_series(1, 2) id;

SELECT pg_column_size(data) FROM test_dummy_toaster;
SELECT pg_column_size(data::text) FROM test_dummy_toaster;
SELECT substring(data, 999990, 20) FROM test_dummy_toaster;

-- detoast when copying to another table with default toaster
CREATE TABLE test_dummy_toaster2(id int, data bytea);
INSERT INTO test_dummy_toaster2
SELECT * FROM test_dummy_toaster;

SELECT pg_column_size(data) FROM test_dummy_toaster2;
SELECT pg_column_size(data::text) FROM test_dummy_toaster2;
SELECT substring(data, 999990, 20) FROM test_dummy_toaster2;

-- toaster.copy() is called when to another table with the same toaster
SELECT set_toaster('dummy', 'test_dummy_toaster2', 'data') <> 0;
TRUNCATE test_dummy_toaster2;
INSERT INTO test_dummy_toaster2
SELECT * FROM test_dummy_toaster;

SELECT pg_column_size(data) FROM test_dummy_toaster2;
SELECT pg_column_size(data::text) FROM test_dummy_toaster2;
SELECT substring(data, 999990, 20) FROM test_dummy_toaster2;

-- detoast when copying to another table with different toaster
SELECT set_toaster('dummy2', 'test_dummy_toaster2', 'data') <> 0;
TRUNCATE test_dummy_toaster2;
INSERT INTO test_dummy_toaster2
SELECT * FROM test_dummy_toaster;

SELECT pg_column_size(data) FROM test_dummy_toaster2;
SELECT pg_column_size(data::text) FROM test_dummy_toaster2;
SELECT substring(data, 999990, 20) FROM test_dummy_toaster2;

-- toaster.update() is called when copying to the same toaster
UPDATE test_dummy_toaster t1 SET data = (SELECT data FROM test_dummy_toaster t2 WHERE t2.id = 3 - t1.id);
SELECT pg_column_size(data) FROM test_dummy_toaster;
SELECT pg_column_size(data::text) FROM test_dummy_toaster;
SELECT substring(data, 999990, 20) FROM test_dummy_toaster;

-- retoast when copying with different toaster
SELECT set_toaster('dummy2', 'test_dummy_toaster', 'data') <> 0;
UPDATE test_dummy_toaster t1 SET data = (SELECT data FROM test_dummy_toaster t2 WHERE t2.id = 3 - t1.id);
SELECT pg_column_size(data) FROM test_dummy_toaster;
SELECT pg_column_size(data::text) FROM test_dummy_toaster;
SELECT substring(data, 999990, 20) FROM test_dummy_toaster;

-- retoast with default toaster after toaster was reset
SELECT reset_toaster('test_dummy_toaster', 'data') <> 0;
UPDATE test_dummy_toaster t1 SET data = (SELECT data FROM test_dummy_toaster t2 WHERE t2.id = 3 - t1.id);
SELECT pg_column_size(data) FROM test_dummy_toaster;
SELECT pg_column_size(data::text) FROM test_dummy_toaster;
SELECT substring(data, 999990, 20) FROM test_dummy_toaster;

SELECT reset_toaster('test_dummy_toaster2', 'data');
SELECT reset_toaster('test_dummy_toaster', 'data');

-- tests for using simple user
CREATE TABLE test_dummy_toaster3(id int, data bytea);
SELECT set_toaster('dummy', 'test_dummy_toaster3', 'data') <> 0;

CREATE USER regress_toastapi_test_user;
GRANT ALL ON TABLE test_dummy_toaster3 TO regress_toastapi_test_user;

-- try to call toaster functions from simple user
SET SESSION AUTHORIZATION regress_toastapi_test_user;

-- ERROR:	permission denied to create toaster "dummy3"
SELECT add_toaster('dummy3', 'dummy_toaster_handler');
-- ERROR:	permission denied to drop toaster "dummy"
SELECT drop_toaster('dummy');
-- ERROR:	permission denied to assign toaster "dummy2"
SELECT set_toaster('dummy2', 'test_dummy_toaster3', 'data');
-- ERROR:	permission denied to reset toaster for table "test_dummy_toaster"
SELECT reset_toaster('test_dummy_toaster3', 'data');

-- no ERROR
SELECT get_toaster_id('dummy') <> 0;
SELECT get_toaster('test_dummy_toaster3', 'data') = get_toaster_id('dummy');

-- simple user can work with table
INSERT INTO test_dummy_toaster3
SELECT id, repeat('a', 1000000)::bytea
FROM generate_series(1, 2) id;

SELECT pg_column_size(data) FROM test_dummy_toaster3;
SELECT pg_column_size(data::text) FROM test_dummy_toaster3;
SELECT substring(data, 999990, 20) FROM test_dummy_toaster3;

RESET SESSION AUTHORIZATION;

-- cleanup
REVOKE CREATE ON DATABASE :"DBNAME" FROM regress_toastapi_test_user;
REVOKE USAGE ON SCHEMA public FROM regress_toastapi_test_user;
SELECT get_toaster_id('dummy2') = drop_toaster('dummy2');
SELECT reset_toaster('test_dummy_toaster', 'data');
SELECT :dummy_toaster_oid = drop_toaster('dummy');
DROP TABLE test_dummy_toaster3;
DROP ROLE regress_toastapi_test_user;

-- ====================================================================
-- Schema-qualified toaster lookup (set_toaster handles "schema.name").
-- pg_toaster.tsrname stores unqualified names, but users may pass a
-- schema-qualified form (typically when a toaster's handler proc lives
-- in an extension installed outside the search path).  The lookup
-- splits on '.' and validates the prefix against the handler proc's
-- pronamespace.
-- ====================================================================

-- Register the handler proc explicitly in the pgpro_toast schema, then
-- add a toaster whose tsrname is unqualified.  This mirrors the layout
-- created by an extension installed outside the search path.
CREATE FUNCTION pgpro_toast.qualtest_handler(internal)
RETURNS internal AS '$libdir/toastapi', 'dummy_toaster_handler'
LANGUAGE C;

SELECT add_toaster('qualtest', 'pgpro_toast.qualtest_handler') > 0
       AS added;

-- Bare name still works (existing behaviour).
SELECT get_toaster_id('qualtest') > 0 AS bare_name_ok;

-- Schema-qualified name resolves to the same toaster when the schema
-- matches the handler's pronamespace.
SELECT get_toaster_id('pgpro_toast.qualtest') > 0 AS qual_name_ok;
SELECT get_toaster_id('qualtest') = get_toaster_id('pgpro_toast.qualtest')
       AS bare_eq_qual;

-- Wrong schema prefix is correctly rejected.
SELECT get_toaster_id('public.qualtest') AS wrong_schema_returns_zero;

-- set_toaster with schema-qualified name attaches successfully.
CREATE TABLE qualtest_tbl (id int, data bytea);
ALTER TABLE qualtest_tbl ALTER data SET STORAGE EXTERNAL;
SELECT set_toaster('pgpro_toast.qualtest', 'qualtest_tbl', 'data') > 0
       AS qual_set_toaster_ok;

-- Cleanup
SELECT reset_toaster('qualtest_tbl', 'data');
DROP TABLE qualtest_tbl;
SELECT drop_toaster('qualtest') > 0 AS dropped;
DROP FUNCTION pgpro_toast.qualtest_handler(internal);

DROP EXTENSION toastapi;
