-- qgeom--1.0.sql
\echo Use "CREATE EXTENSION qgeom" to load this file. \quit

CREATE TYPE qgeom;

CREATE FUNCTION qgeom_in(cstring)
RETURNS qgeom
AS 'MODULE_PATHNAME', 'qgeom_in'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION qgeom_out(qgeom)
RETURNS cstring
AS 'MODULE_PATHNAME', 'qgeom_out'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION qgeom_recv(internal)
RETURNS qgeom
AS 'MODULE_PATHNAME', 'qgeom_recv'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION qgeom_send(qgeom)
RETURNS bytea
AS 'MODULE_PATHNAME', 'qgeom_send'
LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE qgeom (
    input          = qgeom_in,
    output         = qgeom_out,
    receive        = qgeom_recv,
    send           = qgeom_send,
    internallength = variable,
    storage        = extended
);

CREATE FUNCTION qgeom_from_xy(kind int, coords float8[])
RETURNS qgeom
AS 'MODULE_PATHNAME', 'qgeom_from_xy'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION qgeom_to_xy(g qgeom)
RETURNS float8[]
AS 'MODULE_PATHNAME', 'qgeom_to_xy'
LANGUAGE C IMMUTABLE STRICT;
