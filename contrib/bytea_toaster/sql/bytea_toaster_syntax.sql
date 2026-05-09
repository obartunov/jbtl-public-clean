CREATE EXTENSION bytea_toaster;

CREATE TABLE tst_failed (
	t jsonb TOASTER bytea_toaster
);

CREATE TABLE tst1 (
	t bytea TOASTER bytea_toaster
);

DROP TABLE tst_failed;

DROP TABLE tst1;

SELECT pgpro_toast.get_toaster_id('bytea_toaster') AS bytea_toaster_oid
\gset

SELECT pgpro_toast.drop_toaster('bytea_toaster') = :bytea_toaster_oid;

DROP EXTENSION bytea_toaster;
