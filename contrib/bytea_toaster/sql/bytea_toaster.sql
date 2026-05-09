CREATE EXTENSION toastapi;
CREATE EXTENSION bytea_toaster;

--
-- Test failure
--

CREATE TABLE tst_fail (t bytea);
-- Must fail
SELECT pgpro_toast.set_toaster('bytea_toaster', 'tst_fail', 't');
DROP TABLE tst_fail;

--
-- Test bytea_toaster
--

-- Get bytea_toaster OID
SELECT pgpro_toast.get_toaster_id('bytea_toaster') AS bytea_toaster_oid
\gset

CREATE TABLE test_bytea_append (id int, a bytea);
ALTER TABLE test_bytea_append ALTER a SET STORAGE external;
-- Must return bytea_toaster OID
SELECT pgpro_toast.set_toaster('bytea_toaster', 'test_bytea_append', 'a') = :bytea_toaster_oid;

INSERT INTO test_bytea_append SELECT i, repeat('a', 10000)::bytea FROM generate_series(1, 10) i;

SELECT id, a = repeat('a', 10000)::bytea FROM test_bytea_append ORDER BY id;
SELECT id, convert_from(substr(a,    1, 100), 'UTF8') FROM test_bytea_append ORDER BY id;
SELECT id, convert_from(substr(a, 9901, 200), 'UTF8') FROM test_bytea_append ORDER BY id;

BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;

SAVEPOINT p1;
UPDATE test_bytea_append SET a = a || repeat('b', 3000)::bytea;
SELECT id, length(convert_from(a, 'UTF8')), convert_from(substr(a, 9990, 20) || substr(a, 12990, 20), 'UTF8') FROM test_bytea_append;

SAVEPOINT p2;
UPDATE test_bytea_append SET a = a || repeat('c', 2000)::bytea;
SELECT id, length(convert_from(a, 'UTF8')), convert_from(substr(a, 9990, 20) || substr(a, 12990, 20) ||  substr(a, 14990, 20), 'UTF8') FROM test_bytea_append;

ROLLBACK TO SAVEPOINT p2;
SELECT id, length(convert_from(a, 'UTF8')), convert_from(substr(a, 9990, 20) || substr(a, 12990, 20), 'UTF8') FROM test_bytea_append;
UPDATE test_bytea_append SET a = a || repeat('d', 4000)::bytea;
SELECT id, length(convert_from(a, 'UTF8')), convert_from(substr(a, 9990, 20) || substr(a, 12990, 20) || substr(a, 16990, 20), 'UTF8') FROM test_bytea_append;

ROLLBACK TO SAVEPOINT p1;
SELECT id, length(convert_from(a, 'UTF8')), convert_from(substr(a, 9990, 20) || substr(a, 12990, 20), 'UTF8') FROM test_bytea_append;
UPDATE test_bytea_append SET a = a || repeat('e', 5000)::bytea;
SELECT id, length(convert_from(a, 'UTF8')), convert_from(substr(a, 9990, 20) || substr(a, 14990, 20), 'UTF8') FROM test_bytea_append;

COMMIT;

SELECT id, length(convert_from(a, 'UTF8')), convert_from(substr(a, 9990, 20) || substr(a, 14990, 20), 'UTF8') FROM test_bytea_append;

UPDATE test_bytea_append SET a = NULL WHERE id < 3;
UPDATE test_bytea_append SET a = 'foo' WHERE id < 5;

VACUUM FULL test_bytea_append;

SELECT id, length(convert_from(a, 'UTF8')), convert_from(substr(a, 9990, 20) || substr(a, 14990, 20), 'UTF8') FROM test_bytea_append ORDER BY id;

TRUNCATE test_bytea_append;
INSERT INTO test_bytea_append SELECT i, repeat('a', 10000)::bytea FROM generate_series(1, 10) i;

BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;

UPDATE test_bytea_append SET a = a || repeat('b', 3000)::bytea;
SELECT id, convert_from(substr(a, 9990, 20) || substr(a, 12990, 20), 'UTF8') FROM test_bytea_append;

UPDATE test_bytea_append SET a = a || repeat('c', 2000)::bytea;
SELECT id, convert_from(substr(a, 9990, 20) || substr(a, 12990, 20) || substr(a, 14990, 20), 'UTF8') FROM test_bytea_append;

UPDATE test_bytea_append SET a = a || repeat('d', 4000)::bytea;
SELECT id, convert_from(substr(a, 9990, 20) || substr(a, 12990, 20) || substr(a, 14990, 20) || substr(a, 18990, 20), 'UTF8') FROM test_bytea_append;

CREATE FUNCTION test_bytea_append_func() RETURNS void AS
$$
DECLARE
  a0 bytea;
  a1 bytea;
  a2 bytea;
  a3 bytea;
BEGIN
  TRUNCATE test_bytea_append;
  INSERT INTO test_bytea_append SELECT i, repeat('a', 10000)::bytea FROM generate_series(1, 10) i;
  SELECT a INTO a0 FROM test_bytea_append LIMIT 1;

  UPDATE test_bytea_append SET a = a || repeat('b', 3000)::bytea;
  SELECT a INTO a1 FROM test_bytea_append LIMIT 1;

  UPDATE test_bytea_append SET a = a || repeat('c', 2000)::bytea;
  SELECT a INTO a2 FROM test_bytea_append LIMIT 1;

  UPDATE test_bytea_append SET a = a || repeat('d', 4000)::bytea;
  SELECT a INTO a3 FROM test_bytea_append LIMIT 1;

  RAISE NOTICE '%', convert_from(substr(a0, 9990, 20), 'UTF8');
  RAISE NOTICE '%', convert_from(substr(a1, 9990, 20) || substr(a1, 12990, 20), 'UTF8');
  RAISE NOTICE '%', convert_from(substr(a2, 9990, 20) || substr(a2, 12990, 20) || substr(a2, 14990, 20), 'UTF8');
  RAISE NOTICE '%', convert_from(substr(a3, 9990, 20) || substr(a3, 12990, 20) || substr(a3, 14990, 20) || substr(a3, 18990, 20), 'UTF8');
END;
$$ LANGUAGE plpgsql;

SELECT test_bytea_append_func();

COMMIT;

-- Must return bytea_toaster OID
SELECT pgpro_toast.get_toaster('test_bytea_append', 'a') = :bytea_toaster_oid;

SELECT pgpro_toast.reset_toaster('test_bytea_append', 'a');

--
-- Cleanup
--

DROP TABLE test_bytea_append;
-- Must return bytea_toaster OID
SELECT pgpro_toast.drop_toaster('bytea_toaster') = :bytea_toaster_oid;
DROP FUNCTION test_bytea_append_func;
DROP EXTENSION bytea_toaster;
DROP EXTENSION toastapi;
