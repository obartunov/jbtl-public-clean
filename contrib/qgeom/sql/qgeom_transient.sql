-- qgeom_transient.sql — Phase 1.a smoke
-- T1: from_xy / to_xy roundtrip
-- T2: in / out text roundtrip
-- T3: send / recv binary size sanity
-- T4: rejection of bad inputs

CREATE EXTENSION qgeom;

-- T1: from_xy / to_xy roundtrip byte-identical
SELECT qgeom_to_xy(qgeom_from_xy(2, ARRAY[1.0,2.0,3.0,4.0,5.0,6.0]::float8[]))
       = ARRAY[1.0,2.0,3.0,4.0,5.0,6.0]::float8[]
       AS t1_roundtrip_ok;

-- T2: text io roundtrip
SELECT qgeom_out(qgeom_in('3:1.5,2.5;3.5,4.5;5.5,6.5;1.5,2.5')) AS t2_text_out;

SELECT qgeom_to_xy(qgeom_in(qgeom_out(qgeom_from_xy(3,
       ARRAY[10.0,20.0,30.0,40.0,50.0,60.0,10.0,20.0]::float8[]))))
       = ARRAY[10.0,20.0,30.0,40.0,50.0,60.0,10.0,20.0]::float8[]
       AS t2_text_roundtrip_ok;

-- T3: send produces stable bytes; length = 6 header + npoints*16
SELECT octet_length(qgeom_send(qgeom_from_xy(1, ARRAY[7.0,8.0]::float8[])))
       AS t3_send_len_point;
-- expected: 1(ver)+1(kind)+4(npoints)+1*16 = 22 bytes

SELECT octet_length(qgeom_send(qgeom_from_xy(2, ARRAY[1.0,2.0,3.0,4.0,5.0,6.0]::float8[])))
       AS t3_send_len_linestring;
-- expected: 6 + 3*16 = 54 bytes

-- T4: rejection
DO $$
BEGIN
    BEGIN
        PERFORM qgeom_from_xy(99, ARRAY[1.0,2.0]::float8[]);
        RAISE EXCEPTION 'T4a: expected error for bad kind';
    EXCEPTION WHEN invalid_parameter_value THEN
        -- ok
    END;

    BEGIN
        PERFORM qgeom_from_xy(1, ARRAY[1.0,2.0,3.0]::float8[]);
        RAISE EXCEPTION 'T4b: expected error for odd-length array';
    EXCEPTION WHEN invalid_parameter_value THEN
        -- ok
    END;

    BEGIN
        PERFORM qgeom_from_xy(1, ARRAY[NULL,1.0]::float8[]);
        RAISE EXCEPTION 'T4c: expected error for NULL element';
    EXCEPTION WHEN null_value_not_allowed THEN
        -- ok
    END;

    BEGIN
        PERFORM qgeom_from_xy(1, ARRAY[]::float8[]);
        RAISE EXCEPTION 'T4d: expected error for empty array';
    EXCEPTION WHEN invalid_parameter_value THEN
        -- ok
    END;
END $$;

SELECT 't4_all_rejected_as_expected' AS t4_status;

DROP EXTENSION qgeom;
