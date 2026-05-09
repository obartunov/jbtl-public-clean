/* contrib/toastapi/toastapi--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION toastapi UPDATE TO '1.1'" to load this file. \quit

WITH thndl as (
   SELECT p.oid as thoid, r.routine_name as thname
   FROM information_schema.routines r, pg_proc p
   where p.proname=r.routine_name
      and routine_type='FUNCTION'
)
UPDATE pg_toaster
SET   tsroid = thoid
FROM  thndl
WHERE thndl.thoid = pg_toaster.tsrhandler::oid;

alter table pg_toaster alter column tsrhandler type text;

UPDATE pg_toaster SET tsrhandler = (pg_identify_object('pg_proc'::regclass::oid, pg_toaster.tsroid, 0)).identity;

GRANT USAGE ON SCHEMA @extschema@ TO public;
GRANT SELECT ON pg_toaster TO public;
